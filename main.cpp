#include <hyprland/src/desktop/rule/Rule.hpp>
#define WLR_USE_UNSTABLE

#include <unistd.h>
#include <vector>

#include <hyprland/src/includes.hpp>
#include <any>
#include <sstream>

#define private public
#define protected public
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
#include <hyprland/src/layout/space/Space.hpp>
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
    std::string match;
    std::string tag;
    Vector2D position;
    Vector2D size;
};

std::vector<Widget> widgets;

void configureWidget(Widget widget){
    const auto PMONITOR = widget.window->m_monitor.lock();
    if (!PMONITOR)
        return;

    if (!widget.window->m_isFloating)
        g_layoutManager->changeFloatingMode(widget.window->layoutTarget());

    float sx = widget.size.x;
    float sy = widget.size.y;
    float px = widget.position.x;
    float py = widget.position.y;

    sx = std::clamp(sx, 1.f, 100.f);
    sy = std::clamp(sy, 1.f, 100.f);
    px = std::clamp(px, 0.f, 100.f);
    py = std::clamp(py, 0.f, 100.f);

    if (px + sx > 100.f) {
        Log::logger->log(Log::WARN, "[hyprwinwrap] size_x (%d) + pos_x (%d) > 100, adjusting size_x to %d", sx, px, 100.f - px);
        sx = 100.f - px;
    }
    if (py + sy > 100.f) {
        Log::logger->log(Log::WARN, "[hyprwinwrap] size_y (%d) + pos_y (%d) > 100, adjusting size_y to %d", sy, py, 100.f - py);
        sy = 100.f - py;
    }

    const Vector2D monitorSize = PMONITOR->m_size;
    const Vector2D monitorPos  = PMONITOR->m_position;

    const Vector2D newSize = {static_cast<int>(monitorSize.x * (sx / 100.f)), static_cast<int>(monitorSize.y * (sy / 100.f))};

    const Vector2D newPos = {static_cast<int>(monitorPos.x + (monitorSize.x * (px / 100.f))), static_cast<int>(monitorPos.y + (monitorSize.y * (py / 100.f)))};

    const CBox b(newPos.x, newPos.y, newSize.x, newSize.y);

    pWindow->layoutTarget()->space()->setTargetGeom(b, pWindow->layoutTarget());
    pWindow->m_realSize->setValueAndWarp(newSize);
    pWindow->m_realPosition->setValueAndWarp(newPos);
    pWindow->m_size     = newSize;
    pWindow->m_position = newPos;
    pWindow->m_pinned   = true;
    pWindow->sendWindowSize(true);

    widgets.push_back(widget);
    pWindow->m_hidden = true;

    g_pInputManager->refocus();
    Log::logger->log(Log::DEBUG, "[hyprwinwrap] new window moved to bg");
}

int addWidget(lua_State* L) {
    if (!lua_istable(L, 1))
        return Config::Lua::Bindings::Internal::configError(L, "hyprwinwrap: expected a table { match, tag, x, y, w, h, z }");

    auto getInt = [&](const std::string& name) -> lua_Integer{
        Hyprutils::Utils::CScopeGuard x([L] { lua_pop(L, 1); });
        lua_getfield(L, 1, name.c_str());
        
        if (!lua_isinteger(L, -1))
            return Config::Lua::Bindings::Internal::configError(L, "hyprwinwrap: '" + name + "' must be an integer");

        return lua_tointeger(L, -1);
    }
    auto getStr = [&](const std::string& name) -> const char* {
        Hyprutils::Utils::CScopeGuard x([L] { lua_pop(L, 1); });
        lua_getfield(L, 1, name.c_str());
        
        if (!lua_isstring(L, -1))
            return Config::Lua::Bindings::Internal::configError(L, "hyprwinwrap: '" + name + "' must be a class string");

        return lua_tostring(L, -1);
    }

    WWidget widget;

    widget.match      = getStr("match");
    widget.tag        = getStr("tag");
    widget.position.x = getInt("x");
    widget.position.y = getInt("y");
    widget.size.x     = getInt("w");
    widget.size.z     = getInt("h");
    widget.priority   = getInt("z");

    Hyprutils::String::CVarList vars(window, 0, ',');
    PHLWINDOW pWindow = g_pCompositor->getWindowByRegex(vars[0]);

    if (!pWindow){
        HyprlandAPI::addNotification(PHANDLE, "[hyprwinwrap] Could not match any window to '" + widget.match + "' match rule.", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprwinwrap] Could not match any window.");
    }

    pWindow->m_ruleApplicator->m_tagKeeper.applyTag("+" + widget.tag, true);
    pWindow->m_ruleApplicator->propertiesChanged(Desktop::Rule::RULE_PROP_TAG);
    pWindow->updateDecorationValues();
    
    configureWidget(widget);
    std::sort(widgets.begin(), widgets.end(), [](const Widget& a, const Widget& b) {
        return a.prioritstd::sort(vec.begin(), vec.end(),
    [](const Obj& a, const Obj& b) {
        return a.x < b.x;
    });y < b.priority;
    });

    return 0;
}

void onCloseWindow(const Widget& widget) {
    std::erase_if(widgets, [widget](const auto& ref) {
        return ref.window.expired() || ref.window.lock() == widget.window;
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

    Hyprutils::Utils::CScopeGuard x([L] { lua_pop(L, 1); });
    lua_getfield(L, 1, "match");
    
    if (!lua_isstring(L, -1))
        return Config::Lua::Bindings::Internal::configError(L, "hyprwinwrap: 'match' must be a class string");

    std::string match(lua_tostring(L, -1));
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
        return ref.lock() == PWINDOW;
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

    HyprlandAPI::addNotification(PHANDLE, "[hyprwinwrap] Initialized successfully!", CHyprColor{0.2, 1.0, 0.2, 1.0}, 5000);

    return {"hyprwinwrap", "A clone of xwinwrap for Hyprland", "Vaxry", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    clearWindowRules();
}
