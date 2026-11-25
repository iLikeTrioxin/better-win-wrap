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
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/includes.hpp>
#include <hyprland/src/desktop/rule/Rule.hpp>
#include <hyprland/src/config/lua/bindings/LuaBindingsInternal.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
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

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/config/values/ConfigValues.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/state/MonitorQuery.hpp>
#include "globals.hpp"

// Do NOT change this function
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

// hooks
inline CFunctionHook* subsurfaceHook = nullptr;
inline CFunctionHook* commitHook     = nullptr;
typedef void (*origCommitSubsurface)(Desktop::View::CSubsurface* thisptr);
typedef void (*origCommit)(void* owner, bool initial);

class Widget{
public:
    Vector2D position     = {0, 0};
    Vector2D size         = {0, 0};
    PHLWINDOWREF  window  = nullptr;
    PHLMONITORREF monitor = nullptr;
    Widget* dupeOf   = nullptr;
    bool isParallax  = false;
    bool wasFloating = false;
    bool global      = false;
    bool isDead      = false;
    uint8_t priority = 0;

    std::string tag;
};

std::vector<Widget> widgets;

Widget* addWidget(PHLWINDOWREF window, PHLMONITORREF monitor, const std::string& tag, Vector2D position, Vector2D size, int priority, bool global){
    Widget widget;

    widget.tag      = tag;
    widget.position = position;
    widget.size     = size;
    // widget.priority = priority;
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
    if (!layout->floating()){
        widget.wasFloating = false;

        if(widget.position.x < 0) newBox.x = widget.monitor->m_position.x;
        if(widget.position.y < 0) newBox.y = widget.monitor->m_position.y;

        if(widget.size.x <= 0) newBox.w = widget.monitor->m_size.x;
        if(widget.size.y <= 0) newBox.h = widget.monitor->m_size.y;

        layout->setFloating(true);
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
        // layout->space()->setTargetGeom(newBox, layout);
        // widget.window->m_realSize->setValueAndWarp(newBox.size());
        // widget.window->m_realPosition->setValueAndWarp(newBox.pos());
        // widget.window->m_size     = newBox.size();
        // widget.window->m_position = newBox.pos();
        layout->setPositionGlobal(newBox);
        layout->warpPositionSize();

        widget.window->sizeAnimation()->setValueAndWarp(newBox.size());
        widget.window->positionAnimation()->setValueAndWarp(newBox.pos());
        widget.window->sendWindowSize(true);
        // widget.window->m_pinned   = true;
        // widget.window->sendWindowSize(true);

        widget.window->m_state |= Desktop::View::WINDOW_STATE_PINNED;
        
        widget.window->m_hidden = true;
        
        widget.window->m_ruleApplicator->noFocusOverride(Desktop::Types::COverridableVar<bool>(true, Desktop::Types::PRIORITY_SET_PROP));
        // Hypr 0.56: unmap takes a close-anim snapshot while onCommit has us momentarily unhidden and we see a flash on window kill
        widget.window->m_ruleApplicator->noAnimOverride(Desktop::Types::COverridableVar<bool>(true, Desktop::Types::PRIORITY_SET_PROP));
        
        // widget.window->m_ruleApplicator->m_tagKeeper.applyTag("+" + widget.tag, true);
        // widget.window->m_ruleApplicator->propertiesChanged(Desktop::Rule::RULE_PROP_TAG);
        // Desktop::Rule::ruleEngine()->updateAllRules();

        g_pInputManager->refocus();
        Desktop::focusState()->resetWindowFocus();
    }

    widget.position.x = newBox.x;
    widget.position.y = newBox.y;
    widget.size.x = newBox.w;
    widget.size.y = newBox.h;

    widgets.push_back(widget);

    std::sort(widgets.begin(), widgets.end(), [](const Widget& a, const Widget& b) {
        return a.priority < b.priority;
    });

    Log::logger->log(Log::DEBUG, "[hyperwidgets] new widget added successfully");
    return &(widgets[widgets.size()-1]);
}

PHLWINDOW findWindowByClass(const std::string& match){
    if(match == "active"){
        return Desktop::focusState()->window();
    }

    for (auto const& w : Desktop::windowState()->windows()) {
        if(w->metadata().appID() == match) {
            return w;
        }
    }
    return nullptr;
}
PHLMONITOR findMonitorByName(const std::string& name){
    if(name == "active"){
        return Desktop::focusState()->monitor();
    }

    for (auto const& m : State::monitorState()->monitors()) {
        if(m->name() == name) {
            return m;
        }
    }
    return nullptr;
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

    bool global   = (bool)getInt("global",  0);
    bool parallax = (bool)getInt("parallax", 0);

    PHLWINDOWREF window = findWindowByClass(match);

    if (!window){
        HyprlandAPI::addNotification(PHANDLE, "[hyprwidgets] Could not match any window to '"+match+"' match rule.", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        return 0;
    }

    PHLMONITORREF monitor = window->m_monitor;

    if (monName != "active" && monName != "all")
        monitor = findMonitorByName(monName);
    
    if (!monitor){
        HyprlandAPI::addNotification(PHANDLE, "[hyprwidgets] Could not match any monitor to '"+monName+"'.", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        return 0;
    }

    if(monName != "all"){
        addWidget(window, monitor, tag, {x, y}, {w, h}, z, global);
        return 0;
    }
    
    // always begin from monitor that app lives in
    addWidget(window, monitor, tag, {x, y}, {w, h}, z, global);

    for(auto ref : State::monitorState()->monitors()){
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

void quickFix(){
    for (auto const& w : Desktop::windowState()->windows()) {
        w->m_ruleApplicator->noFocusOverride(Desktop::Types::COverridableVar<bool>(false, Desktop::Types::PRIORITY_SET_PROP));
        // Hypr 0.56: unmap takes a close-anim snapshot while onCommit has us momentarily unhidden and we see a flash on window kill
        w->m_ruleApplicator->noAnimOverride(Desktop::Types::COverridableVar<bool>(false, Desktop::Types::PRIORITY_SET_PROP));
    }
}

int freeWidget(const std::string& match, bool preserveFloat = true){
    // Hyprutils::String::CVarList vars(match, 0, ',');
    // PHLWINDOW pWindow = g_pCompositor->getWindowByRegex(vars[0]);
    PHLWINDOW pWindow = findWindowByClass(match);
    
    if(match == "qfix"){
        quickFix();
    }

    for(auto& widget : widgets){
        const auto bgw = widget.window.lock();

        if (bgw != pWindow && match != "" && match != "all") continue;
        if (widget.dupeOf) {
            widget.isDead = true;
            g_pHyprRenderer->damageMonitor(bgw->m_monitor.lock());

            // Final render should be where the window was originally at to avoid any weirdness
            widget.window->m_realPosition->m_Value = widget.dupeOf->position;
            widget.window->m_realSize    ->m_Value = widget.dupeOf->size;

            continue;
        }

        bgw->m_hidden = false;
        bgw->m_state = Desktop::View::WINDOW_STATE_NONE;
        
        bgw->m_ruleApplicator->noFocusOverride(Desktop::Types::COverridableVar<bool>(false, Desktop::Types::PRIORITY_SET_PROP));
        // Hypr 0.56: unmap takes a close-anim snapshot while onCommit has us momentarily unhidden and we see a flash on window kill
        bgw->m_ruleApplicator->noAnimOverride(Desktop::Types::COverridableVar<bool>(false, Desktop::Types::PRIORITY_SET_PROP));
        // bgw->m_pinned = false;
        bgw->m_ruleApplicator->m_tagKeeper.applyTag("-" + widget.tag, true);
        bgw->m_ruleApplicator->propertiesChanged(Desktop::Rule::RULE_PROP_TAG);
        // bgw->updateDecorationValues();

        if ((bgw->layoutTarget()->floating() != widget.wasFloating) == preserveFloat)
            bgw->layoutTarget()->setFloating(widget.wasFloating);

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

static bool shouldPauseBg(const PHLMONITOR &mon)
{
    if (g_pSessionLockManager->isSessionLocked())
        return true;
    if (!mon || !mon->m_dpmsStatus)
        return true;
    return Fullscreen::controller()->getFullscreenModes(mon).internal == Fullscreen::FSMODE_FULLSCREEN;
}

void onRenderStage(const Event::SRenderStageEvent &event) {
    if (event.stage != RENDER_POST_WALLPAPER || !event.context || !event.monitor)
        return;

    Render::CRenderContext &ctx = event.context->get();
    const bool pause = shouldPauseBg(event.monitor);

    for (const auto& widget : widgets) {
        const auto bgw = widget.window.lock();

        if (widget.monitor != event.monitor && !widget.global)
            continue;

        // Here cuz moving windows, workspaces, etc. causes
        // suspend for hidden windows (even when inhabit is set)
        if(bgw->m_suspended) [[unlikely]] bgw->setSuspended(false);

        // cant use setHidden cuz that sends suspended and shit too that would be laggy
        bgw->m_hidden = false;
        bgw->m_realPosition->m_Value = widget.position;
        bgw->m_realSize    ->m_Value = widget.size;

        g_pHyprRenderer->renderWindow(ctx, bgw, widget.monitor.lock(), Render::SWindowRenderPresentation{}, Time::steadyNow(), false, RENDER_PASS_ALL, false, true);

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

    for(const auto& w : widgets){
        if(w.window.lock() != PWINDOW) continue;
        // cant use setHidden cuz that sends suspended and shit too that would be laggy
        PWINDOW->m_hidden = false;

        ((origCommitSubsurface)subsurfaceHook->m_original)(thisptr);
        if (const auto MON = w.monitor.lock(); MON)
            g_pHyprRenderer->damageMonitor(MON);

        PWINDOW->m_hidden = true;
    }
}

void onCommit(void* owner, bool initial) {
    const auto PWINDOW = ((Desktop::View::CWindow*)owner)->m_self.lock();

    const auto& widget = std::find_if(widgets.begin(), widgets.end(), [PWINDOW](const auto& ref) {
        return ref.window.lock() == PWINDOW;
    });

    if (widget == widgets.end()) {
        ((origCommit)commitHook->m_original)(owner, initial);
        return;
    }

    for(const auto& w : widgets){
        if(w.window.lock() != PWINDOW) continue;
        // cant use setHidden cuz that sends suspended and shit too that would be laggy
        PWINDOW->m_hidden = false;

        ((origCommit)commitHook->m_original)(owner, initial);
        if (const auto MON = w.monitor.lock(); MON)
            g_pHyprRenderer->damageMonitor(MON);

        PWINDOW->m_hidden = true;
    }
}

void onConfigReload(){
    for (auto& widget : widgets){
        widget.window->m_hidden = false;
        widget.window->m_ruleApplicator->propertiesChanged(Desktop::Rule::RULE_PROP_TAG);
        // widget.window->updateDecorationValues();
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

    static auto P2 = Event::bus()->m_events.window.close.listen([&](PHLWINDOW w) { onCloseWindow(w); });
    // static auto P2 = Event::bus()->m_events.window.open.listen([&](PHLWINDOW w) { onCloseWindow(w); });
    static auto P3 = Event::bus()->m_events.render.stage.listen([&](const Event::SRenderStageEvent &event) { onRenderStage(event); });
    static auto P4 = Event::bus()->m_events.config.reloaded.listen([&] { onConfigReload(); });

    auto fns = HyprlandAPI::findFunctionsByName(PHANDLE, "_ZN7Desktop4View11CSubsurface8onCommitEv");
    if (fns.size() < 1)
        throw std::runtime_error("[hyprwidgets] onCommit not found");
    subsurfaceHook = HyprlandAPI::createFunctionHook(PHANDLE, fns[0].address, (void*)&onCommitSubsurface);

    fns = HyprlandAPI::findFunctionsByName(PHANDLE, "_ZN7Desktop4View7CWindow12commitWindowEb");
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
