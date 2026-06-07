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
    int priority = 0;
    PHLWINDOWREF window = nullptr;
    bool wasFloating = false;
    std::string match;
    std::string tag;
    Vector2D position;
    Vector2D size;
};

std::vector<Widget> widgets;
std::vector<SP<Desktop::Rule::IRule>> bgRules;

static SP<Desktop::Rule::CWindowRule> makeWindowRule(const std::string& name, const Desktop::Rule::eRuleProperty prop, const std::string& match) {
    auto rule = makeShared<Desktop::Rule::CWindowRule>(name);
    rule->registerMatch(prop, match);
    rule->addEffect(Desktop::Rule::WINDOW_RULE_EFFECT_FLOAT, "1");
    rule->addEffect(Desktop::Rule::WINDOW_RULE_EFFECT_NOINITIALFOCUS, "1");
    rule->addEffect(Desktop::Rule::WINDOW_RULE_EFFECT_NO_FOCUS, "1");
    rule->addEffect(Desktop::Rule::WINDOW_RULE_EFFECT_ALLOWS_INPUT, "0");
    rule->addEffect(Desktop::Rule::WINDOW_RULE_EFFECT_BORDER_SIZE, "0");
    return rule;
}

static void clearWindowRules() {
    for (auto& rule : bgRules) {
        if (rule)
            Desktop::Rule::ruleEngine()->unregisterRule(rule);
    }
    bgRules.clear();
}

void configureWidget(Widget& widget){
    const auto PMONITOR = widget.window->m_monitor.lock();

    if (!PMONITOR) return;

    const auto& layout = widget.window->layoutTarget();
    CBox newBox = layout->position();
    widget.wasFloating = true;

    if (!widget.window->m_isFloating){
        widget.wasFloating = false;

        if(widget.position.x < 0) newBoundingBox.x = 0;
        if(widget.position.y < 0) newBoundingBox.y = 0;

        if(widget.size.x <= 0) newBoundingBox.w = PMONITOR->m_size.x;
        if(widget.size.y <= 0) newBoundingBox.h = PMONITOR->m_size.y;

        g_layoutManager->changeFloatingMode(layout);
    }

    if(widget.position.x >= 100 || widget.position.y >= 100 || widget.size.x >= 100 || widget.size.y >= 100)
        HyprlandAPI::addNotification(PHANDLE, "[hyprwidgets] Widget position and size should be a % value - scaling down.", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);

    if(widget.position.x >= 0) newBox.x = PMONITOR.m_size.x * std::clamp(widget.position.x, 1.f - widget.size.x, 99.f);
    if(widget.position.y >= 0) newBox.y = PMONITOR.m_size.y * std::clamp(widget.position.y, 1.f - widget.size.y, 99.f);
    if(widget.size.x     >  0) newBox.w = PMONITOR.m_size.x * std::clamp(widget.size.x, 1.f, 100.f);
    if(widget.size.y     >  0) newBox.h = PMONITOR.m_size.y * std::clamp(widget.size.y, 1.f, 100.f);

    layout->setTargetGeom(newBox, layout);
    widget.window->m_realSize->setValueAndWarp(newBox.size());
    widget.window->m_realPosition->setValueAndWarp(newBox.pos());
    widget.window->m_size     = newBox.size();
    widget.window->m_position = newBox.pos();
    widget.window->m_pinned   = true;
    widget.window->sendWindowSize(true);

    widget.window->m_ruleApplicator->m_tagKeeper.applyTag("+" + widget.tag, true);
    widget.window->m_ruleApplicator->propertiesChanged(Desktop::Rule::RULE_PROP_TAG);
    widget.window->updateDecorationValues();
    g_pInputManager->refocus();
    widget.window->m_hidden = true;

    widgets.push_back(widget);
    std::sort(widgets.begin(), widgets.end(), [](const Widget& a, const Widget& b) {
        return a.priority < b.priority;
    });

    Log::logger->log(Log::DEBUG, "[hyperwidgets] new widget added successfully");
}

int addWidget(lua_State* L) {
    if (!lua_istable(L, 1))
        return Config::Lua::Bindings::Internal::configError(L, "hyprwinwrap: expected a table { match?, tag?, x?, y?, w?, h?, z? }");

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

    Widget widget;

    widget.match = getStr("match", "active");
    widget.tag   = getStr("tag", "hyprwidget");

    Hyprutils::String::CVarList vars(widget.match, 0, ',');
    widget.window = g_pCompositor->getWindowByRegex(vars[0]);

    if (!widget.window){
        HyprlandAPI::addNotification(PHANDLE, "[hyprwinwrap] Could not match any window to '" + widget.match + "' match rule.", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        return 0;
    }

    widget.position.x = getInt("x", -1);
    widget.position.y = getInt("y", -1);
    widget.size.x     = getInt("w", -1);
    widget.size.y     = getInt("h", -1);
    widget.priority   = getInt("z", -1);
 
    configureWidget(widget); 

    return 0;
}

void onCloseWindow(PHLWINDOW pWindow) {
    std::erase_if(widgets, [pWindow](const auto& ref) {
        return ref.window.expired() || ref.window.lock() == pWindow;
    });
    //std::erase_if(bgRules, [pWindow](const auto& rule) {
    //    if (rule->name() != std::to_string(pWindow->getPID())) return false;
    //    Desktop::Rule::ruleEngine()->unregisterRule(rule);
    //    return true;
    //});

    Log::logger->log(Log::DEBUG, "[hyprwinwrap] closed window");
}

int removeWidget(lua_State* L) {
    if (!lua_istable(L, 1))
        return Config::Lua::Bindings::Internal::configError(L, "hyprwinwrap: expected a table { match }");

    std::string match;
    {
        Hyprutils::Utils::CScopeGuard x([L] { lua_pop(L, 1); });
        lua_getfield(L, 1, "match");
    
        if (!lua_isstring(L, -1))
            return Config::Lua::Bindings::Internal::configError(L, "hyprwinwrap: 'match' must be a class string");

        match = lua_tostring(L, -1);
    }

    Hyprutils::String::CVarList vars(match, 0, ',');
    PHLWINDOW pWindow = g_pCompositor->getWindowByRegex(vars[0]);

    for(auto& widget : widgets){
        const auto bgw = widget.window.lock();

        if (bgw != pWindow && match != "" && match != "all") continue;

        bgw->m_hidden = false;
        bgw->m_pinned = false;
        bgw->m_ruleApplicator->m_tagKeeper.applyTag("-" + widget.tag, true);
        bgw->m_ruleApplicator->propertiesChanged(Desktop::Rule::RULE_PROP_TAG);
        bgw->updateDecorationValues();

        if (bgw->m_isFloating) g_layoutManager->changeFloatingMode(bgw->layoutTarget());

        onCloseWindow(bgw);
    }

    Desktop::Rule::ruleEngine()->updateAllRules();

    return 0;
}

void onRenderStage(eRenderStage stage) {
    if (stage != RENDER_POST_WALLPAPER)
        return;

    for (auto& widget : widgets) {
        const auto bgw = widget.window.lock();

        if (bgw->m_monitor != g_pHyprRenderer->m_renderData.pMonitor)
            continue;

        if (bgw->m_suspended) bgw->setSuspended(false);

        // cant use setHidden cuz that sends suspended and shit too that would be laggy
        bgw->m_hidden = false;

        g_pHyprRenderer->renderWindow(bgw, g_pHyprRenderer->m_renderData.pMonitor.lock(), Time::steadyNow(), false, RENDER_PASS_ALL, false, true);

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
        HyprlandAPI::addNotification(PHANDLE, "[hyprwinwrap] Failure in initialization: Version mismatch (headers ver is not equal to running hyprland ver)",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hww] Version mismatch");
    }

    if (Config::mgr()->type() == Config::CONFIG_LUA) {
        HyprlandAPI::addLuaFunction(PHANDLE, "hyprwidgets", "add_widget", ::addWidget);
        HyprlandAPI::addLuaFunction(PHANDLE, "hyprwidgets", "remove_widget", ::removeWidget);
    } else {
        HyprlandAPI::addNotification(PHANDLE, "[hyprwinwrap] Failure in initialization: Legacy config not supported, please use lua.", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hww] Config type not supported, please use lua.");
    }

    static auto P2 = Event::bus()->m_events.window.close.listen([&](PHLWINDOW w) { onCloseWindow(w); });
    static auto P3 = Event::bus()->m_events.render.stage.listen([&](eRenderStage stage) { onRenderStage(stage); });
    static auto P4 = Event::bus()->m_events.config.reloaded.listen([&] { onConfigReload(); });

    auto fns = HyprlandAPI::findFunctionsByName(PHANDLE, "_ZN7Desktop4View11CSubsurface8onCommitEv");
    if (fns.size() < 1)
        throw std::runtime_error("hyprwinwrap: onCommit not found");
    subsurfaceHook = HyprlandAPI::createFunctionHook(PHANDLE, fns[0].address, (void*)&onCommitSubsurface);

    fns = HyprlandAPI::findFunctionsByName(PHANDLE, "_ZN7Desktop4View7CWindow12commitWindowEv");
    if (fns.size() < 1)
        throw std::runtime_error("hyprwinwrap: listener_commitWindow not found");
    commitHook = HyprlandAPI::createFunctionHook(PHANDLE, fns[0].address, (void*)&onCommit);

    bool hkResult = subsurfaceHook->hook();
    hkResult      = hkResult && commitHook->hook();

    if (!hkResult)
        throw std::runtime_error("hyprwinwrap: hooks failed");

    onConfigReload();

    HyprlandAPI::addNotification(PHANDLE, "[hyprwinwrap] Initialized successfully!", CHyprColor{0.2, 1.0, 0.2, 1.0}, 5000);

    return {"hyprwinwrap", "A clone of xwinwrap for Hyprland", "Vaxry", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    for(auto& widget : widgets){
        const auto bgw = widget.window.lock();

        bgw->m_hidden = false;
        bgw->m_pinned = false;
        bgw->m_ruleApplicator->m_tagKeeper.applyTag("-" + widget.tag, true);
        bgw->m_ruleApplicator->propertiesChanged(Desktop::Rule::RULE_PROP_TAG);
        bgw->updateDecorationValues();

        if (bgw->m_isFloating) g_layoutManager->changeFloatingMode(bgw->layoutTarget());

        onCloseWindow(bgw);
    }

    Desktop::Rule::ruleEngine()->updateAllRules();
}
