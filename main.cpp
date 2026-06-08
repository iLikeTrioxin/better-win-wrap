#define WLR_USE_UNSTABLE

#include <unistd.h>
#include <vector>

#include <any>
#include <sstream>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#define private public
#define protected public
#include <hyprland/src/includes.hpp>
#include <hyprland/src/desktop/rule/Rule.hpp>
#include <hyprland/src/config/lua/bindings/LuaBindingsInternal.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/desktop/rule/Engine.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRule.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/event/EventBus.hpp>
#undef private
#undef protected

using namespace Render;
using namespace Render::GL;

#include <hyprutils/string/VarList.hpp>
#include <hyprutils/string/ConstVarList.hpp>

#include "globals.hpp"

// Do NOT change this function
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

// hooks
inline CFunctionHook* subsurfaceHook = nullptr;
inline CFunctionHook* commitHook     = nullptr;
typedef void (*origCommitSubsurface)(Desktop::View::CSubsurface* thisptr);
typedef void (*origCommit)(void* owner, void* data);

class Widget{
public:
    PHLWINDOWREF  window  = nullptr;
    PHLMONITORREF monitor = nullptr;
    Vector2D position     = {0, 0};
    Vector2D size         = {0, 0};
    Widget* dupeOf = nullptr;
    int priority = 0;
    bool global = false;
    bool isDead = false;
    bool wasFloating = false;
    std::string tag;
};

std::vector<Widget> widgets;

Widget* addWidget(PHLWINDOWREF window, PHLMONITORREF monitor, const std::string& tag, Vector2D position, Vector2D size, int priority, bool global){
    Widget widget;

    widget.tag      = tag;
    widget.position = position;
    widget.size     = size;
    widget.priority = priority;
    widget.global   = global;
    widget.window   = window;
    widget.monitor  = monitor;

    const auto& original = std::find_if(widgets.begin(), widgets.end(), [widget](const auto& ref) {
        return ref.window == widget.window && ref.dupeOf == nullptr;
    });

    if(original != widgets.end()) widget.dupeOf = &(*original);

    const auto& layout = widget.window->layoutTarget();
    CBox newBox = layout->position();
    widget.wasFloating = true;

    // it will always be false for dupes, cuz window was set to float in original
    if (!widget.window->m_isFloating){
        widget.wasFloating = false;

        if(widget.position.x < 0) newBox.x = widget.monitor->m_position.x;
        if(widget.position.y < 0) newBox.y = widget.monitor->m_position.y;

        if(widget.size.x <= 0) newBox.w = widget.monitor->m_size.x;
        if(widget.size.y <= 0) newBox.h = widget.monitor->m_size.y;

        g_layoutManager->changeFloatingMode(layout);
    }

    if(widget.dupeOf != nullptr){
        Vector2D relativePos  = {0, 0};
        Vector2D relativeSize = {0, 0};

        relativePos.x = widget.dupeOf->position.x - widget.dupeOf->window->m_monitor->m_position.x;
        relativePos.y = widget.dupeOf->position.y - widget.dupeOf->window->m_monitor->m_position.y;

        relativeSize.x = widget.dupeOf->size.x / widget.dupeOf->window->m_monitor->m_size.x;
        relativeSize.y = widget.dupeOf->size.y / widget.dupeOf->window->m_monitor->m_size.y;
        
        if(widget.position.x < 0) newBox.x = widget.monitor->m_position.x + relativePos.x;
        if(widget.position.y < 0) newBox.y = widget.monitor->m_position.y + relativePos.y;

        if(widget.size.x <= 0) newBox.w = widget.monitor->m_size.x * relativeSize.x;
        if(widget.size.y <= 0) newBox.h = widget.monitor->m_size.y * relativeSize.y;
    }

    if(widget.position.x > 99 || widget.position.y > 99 || widget.size.x > 100 || widget.size.y > 100)
        HyprlandAPI::addNotification(PHANDLE, "[hyprwidgets] Widget position and size should be a % value - scaling down.", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);

    if(widget.position.x >= 0) newBox.x = widget.monitor->m_position.x + (widget.monitor->m_size.x / 100.0) * std::clamp(widget.position.x, 1.0 - widget.size.x, 99.0);
    if(widget.position.y >= 0) newBox.y = widget.monitor->m_position.y + (widget.monitor->m_size.y / 100.0) * std::clamp(widget.position.y, 1.0 - widget.size.y, 99.0);
    if(widget.size.x     >  0) newBox.w = (widget.monitor->m_size.x / 100.0) * std::clamp(widget.size.x, 1.0, 100.0);
    if(widget.size.y     >  0) newBox.h = (widget.monitor->m_size.y / 100.0) * std::clamp(widget.size.y, 1.0, 100.0);

    if(widget.dupeOf == nullptr){
        layout->space()->setTargetGeom(newBox, layout);
        widget.window->m_realSize->setValueAndWarp(newBox.size());
        widget.window->m_realPosition->setValueAndWarp(newBox.pos());
        widget.window->m_size     = newBox.size();
        widget.window->m_position = newBox.pos();
        widget.window->m_pinned   = true;
        widget.window->sendWindowSize(true);

        widget.window->m_ruleApplicator->m_tagKeeper.applyTag("+" + widget.tag, true);
        widget.window->m_ruleApplicator->propertiesChanged(Desktop::Rule::RULE_PROP_TAG);
        widget.window->updateDecorationValues();
        Desktop::Rule::ruleEngine()->updateAllRules();
        widget.window->m_hidden = true;
        Desktop::focusState()->resetWindowFocus();
        g_pInputManager->refocus();
    }

    widget.position.x = newBox.x;
    widget.position.y = newBox.y;
    widget.size.x = newBox.w;
    widget.size.y = newBox.h;

    widgets.push_back(widget);

    std::sort(widgets.begin(), widgets.end(), [](const Widget& a, const Widget& b) {
        return a.priority < b.priority;
    });

    HyprlandAPI::addNotification(PHANDLE, "Addded: x:" + std::to_string(widget.position.x) + ", y:" + std::to_string(widget.position.y) + ", w:" + std::to_string(widget.size.x) + ", h:" + std::to_string(widget.size.y) + ", glob:" + std::to_string(widget.global), CHyprColor{0.2, 1.0, 0.2, 1.0}, 8000);
    Log::logger->log(Log::DEBUG, "[hyperwidgets] new widget added successfully");
    return &(widgets[widgets.size()-1]);
}

int luaAddWidget(lua_State* L) {
    if (!lua_istable(L, 1))
        return Config::Lua::Bindings::Internal::configError(L, "[hyprwidgets] add_widget: expected a table { match?, tag?, x?, y?, w?, h?, z?, global?, monitor? }");

    auto getInt = [&](const std::string& name , int def) -> lua_Integer{
        Hyprutils::Utils::CScopeGuard x([L] { lua_pop(L, 1); });
        lua_getfield(L, 1, name.c_str());
        
        if (!lua_isinteger(L, -1)) return def;

        return lua_tointeger(L, -1);
    };
    auto getStr = [&](const std::string& name, const char* def) -> const char* {
        Hyprutils::Utils::CScopeGuard x([L] { lua_pop(L, 1); });
        lua_getfield(L, 1, name.c_str());
        
        if (!lua_isstring(L, -1)) return def;
        
        return lua_tostring(L, -1);
    };

    std::string match   = getStr("match", "active");
    std::string tag     = getStr("tag", "hyprwidget"); 
    std::string monName = getStr("monitor", "active"); 

    int x = getInt("x", -1);
    int y = getInt("y", -1);
    int w = getInt("w", -1);
    int h = getInt("h", -1);
    int z = getInt("z", -1);

    bool global = (bool)getInt("global",  0);

    Hyprutils::String::CVarList vars(match, 0, ',');
    PHLWINDOWREF window = g_pCompositor->getWindowByRegex(vars[0]);

    if (!window){
        HyprlandAPI::addNotification(PHANDLE, "[hyprwinwrap] Could not match any window to '"+match+"' match rule.", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        return 0;
    }

    PHLMONITORREF monitor = window->m_monitor;

    if (monName != "active" && monName != "all")
        monitor = g_pCompositor->getMonitorFromName(monName);
    
    if (!monitor){
        HyprlandAPI::addNotification(PHANDLE, "[hyprwinwrap] Could not match any monitor to '"+monName+"'.", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        return 0;
    }

    if(monName != "all"){
        addWidget(window, monitor, tag, {x, y}, {w, h}, z, global);
        return 0;
    }
    

    // 'all' setting requires global (it doesn't change the monitors just offsets)
    global = true;
    
    // always begin from monitor that app lives in
    addWidget(window, monitor, tag, {x, y}, {w, h}, z, global);

    for(auto ref : g_pCompositor->m_monitors){
        if(ref == monitor.lock()) continue;
        addWidget(window, ref, tag, {x, y}, {w, h}, z, global);
    }

    return 0;
}

void onCloseWindow(PHLWINDOW pWindow) {
    std::erase_if(widgets, [pWindow](const auto& ref) {
        return ref.window.expired() || ref.window.lock() == pWindow || ref.isDead;
    });

    Log::logger->log(Log::DEBUG, "[hyprwidgets] Widget removed");
}

int freeWidget(std::string match, bool preserveFloat = true){
    Hyprutils::String::CVarList vars(match, 0, ',');
    PHLWINDOW pWindow = g_pCompositor->getWindowByRegex(vars[0]);

    for(auto& widget : widgets){
        const auto bgw = widget.window.lock();

        if (bgw != pWindow && match != "" && match != "all") continue;
        if (widget.dupeOf) {
            widget.isDead = true;
            g_pHyprRenderer->damageMonitor(bgw->m_monitor.lock());
            continue;
        }

        bgw->m_hidden = false;
        bgw->m_pinned = false;
        bgw->m_ruleApplicator->m_tagKeeper.applyTag("-" + widget.tag, true);
        bgw->m_ruleApplicator->propertiesChanged(Desktop::Rule::RULE_PROP_TAG);
        bgw->updateDecorationValues();

        if ((bgw->m_isFloating != widget.wasFloating) == preserveFloat)
            g_layoutManager->changeFloatingMode(bgw->layoutTarget());

        widget.isDead = true;
    }

    onCloseWindow(nullptr);

    Desktop::Rule::ruleEngine()->updateAllRules();
    g_pInputManager->refocus();

    return 0;
}

int luaFreeWidget(lua_State* L) {
    if (!lua_istable(L, 1))
        return Config::Lua::Bindings::Internal::configError(L, "[hyprwidgets] free_widget: expected a table { match }");

    std::string match;
    {
        Hyprutils::Utils::CScopeGuard x([L] { lua_pop(L, 1); });
        lua_getfield(L, 1, "match");
    
        if (!lua_isstring(L, -1))
            return Config::Lua::Bindings::Internal::configError(L, "[hyprwidgets]: free_widget: 'match' must be a class string");

        match = lua_tostring(L, -1);
    }

    return freeWidget(match);
}

void onWorkspaceChange(){
    // Here cuz moving windows, workspaces, etc. causes
    // suspend for hidden windows (even when inhabit is set)
    for (const auto& widget : widgets) {
        const auto bgw = widget.window.lock();
        if(bgw->m_suspended)
            bgw->setSuspended(false);
    }
}

void onRenderStage(eRenderStage stage) {
    if (stage != RENDER_POST_WALLPAPER)
        return;

    for (const auto& widget : widgets) {
        const auto bgw = widget.window.lock();

        if (bgw->m_monitor != g_pHyprRenderer->m_renderData.pMonitor && !widget.global)
            continue;

        // cant use setHidden cuz that sends suspended and shit too that would be laggy
        bgw->m_hidden = false;
        bgw->m_realPosition->m_Value = widget.position;
        bgw->m_realSize    ->m_Value = widget.size;

        g_pHyprRenderer->renderWindow(bgw, widget.monitor.lock(), Time::steadyNow(), false, RENDER_PASS_ALL, false, true);

        bgw->m_hidden = true;
    }
}

void onCommitSubsurface(Desktop::View::CSubsurface* thisptr) {
    const auto PWINDOW = Desktop::View::CWindow::fromView(thisptr->wlSurface()->view());

    if(!PWINDOW){
        ((origCommitSubsurface)subsurfaceHook->m_original)(thisptr);
        return;
    }

    const auto& widget = std::find_if(widgets.begin(), widgets.end(), [PWINDOW](const auto& ref) {
        return ref.window.lock() == PWINDOW;
    });

    if (widget == widgets.end()) {
        ((origCommitSubsurface)subsurfaceHook->m_original)(thisptr);
        return;
    }

    // cant use setHidden cuz that sends suspended and shit too that would be laggy
    PWINDOW->m_hidden = false;

    ((origCommitSubsurface)subsurfaceHook->m_original)(thisptr);
    if (const auto MON = PWINDOW->m_monitor.lock(); MON)
        g_pHyprRenderer->damageMonitor(MON);

    PWINDOW->m_hidden = true;
}

void onCommit(void* owner, void* data) {
    const auto PWINDOW = ((Desktop::View::CWindow*)owner)->m_self.lock();

    const auto& widget = std::find_if(widgets.begin(), widgets.end(), [PWINDOW](const auto& ref) {
        return ref.window.lock() == PWINDOW;
    });

    if (widget == widgets.end()) {
        ((origCommit)commitHook->m_original)(owner, data);
        return;
    }

    // cant use setHidden cuz that sends suspended and shit too that would be laggy
    PWINDOW->m_hidden = false;

    ((origCommit)commitHook->m_original)(owner, data);
    if (const auto MON = PWINDOW->m_monitor.lock(); MON)
        g_pHyprRenderer->damageMonitor(MON);

    PWINDOW->m_hidden = true;
}

void onConfigReload(){
    for (auto& widget : widgets){
        widget.window->m_hidden = false;
        widget.window->m_ruleApplicator->propertiesChanged(Desktop::Rule::RULE_PROP_TAG);
        widget.window->updateDecorationValues();
    }
    Desktop::Rule::ruleEngine()->updateAllRules();
    for(auto& widget : widgets) widget.window->m_hidden = true;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprwidgets] Failure in initialization: Version mismatch (headers ver is not equal to running hyprland ver)",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprwidgets] Version mismatch");
    }

    if (Config::mgr()->type() == Config::CONFIG_LUA) {
        HyprlandAPI::addLuaFunction(PHANDLE, "hyprwidgets", "add_widget", ::luaAddWidget);
        HyprlandAPI::addLuaFunction(PHANDLE, "hyprwidgets", "free_widget", ::luaFreeWidget);
    } else {
        HyprlandAPI::addNotification(PHANDLE, "[hyprwidgets] Failure in initialization: Legacy config not supported, please use lua.", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprwidgets] Config type not supported, please use lua.");
    }

    static auto P1 = Event::bus()->m_events.workspace.active.listen([&](PHLWORKSPACE w) { onWorkspaceChange(); });
    static auto P2 = Event::bus()->m_events.window.close.listen([&](PHLWINDOW w) { onCloseWindow(w); });
    static auto P3 = Event::bus()->m_events.render.stage.listen([&](eRenderStage stage) { onRenderStage(stage); });
    static auto P4 = Event::bus()->m_events.config.reloaded.listen([&] { onConfigReload(); });

    auto fns = HyprlandAPI::findFunctionsByName(PHANDLE, "_ZN7Desktop4View11CSubsurface8onCommitEv");
    if (fns.size() < 1)
        throw std::runtime_error("[hyprwidgets] onCommit not found");
    subsurfaceHook = HyprlandAPI::createFunctionHook(PHANDLE, fns[0].address, (void*)&onCommitSubsurface);

    fns = HyprlandAPI::findFunctionsByName(PHANDLE, "_ZN7Desktop4View7CWindow12commitWindowEv");
    if (fns.size() < 1)
        throw std::runtime_error("[hyprwidgets] listener_commitWindow not found");
    commitHook = HyprlandAPI::createFunctionHook(PHANDLE, fns[0].address, (void*)&onCommit);

    bool hkResult = subsurfaceHook->hook();
    hkResult      = hkResult && commitHook->hook();

    if (!hkResult)
        throw std::runtime_error("[hyprwidgets] hooks failed");

    HyprlandAPI::addNotification(PHANDLE, "[hyprwidgets] Initialized successfully!", CHyprColor{0.2, 1.0, 0.2, 1.0}, 5000);

    return {"hyprwidgets", "Remake of Vaxry's hyprwinwrap.", "iLikeTrioxin", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    freeWidget("all");
}
