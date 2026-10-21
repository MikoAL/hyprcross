// hyprcross: finger-tracking workspace swipes between arbitrary workspaces, with
// rubber-banding at the edges. Policy (which workspace is "next") lives in Lua;
// this plugin only moves pixels.
//
// Lua API (hl.plugin.cross.*):
//   swipe_begin(axis, before, after) -> bool   axis "x"|"y"; before/after are workspace
//                                               ids, or nil for an edge
//   swipe_update(delta, time_ms)               raw finger movement along the axis
//   swipe_end(cancelled)                       commit or spring back
//   bump(axis, px) / settle()                  nudge the active workspace, then release it
//   active() -> bool                           a swipe is in progress
//
// Swipe animation is modelled on Hyprland's CUnifiedWorkspaceSwipeGesture (0.56.2).

#include <hyprland/src/plugins/PluginAPI.hpp>
#include "shared.hpp"
#include "overview.hpp"
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/input/UnifiedWorkspaceSwipeGesture.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/config/ConfigValue.hpp>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include <cmath>
#include <deque>
#include <optional>

using namespace cross;

namespace {
    // Rubber band: how far past an edge the screen can be pulled (fraction of the
    // screen) and how stiff the pull is. Same shape as UIScrollView's: x*c/(x*c/d+1)-ish.
    constexpr double RUBBER_LIMIT = 0.10;
    constexpr double RUBBER_COEFF = 0.55;

    // Release: a flick faster than this (finger units per ms) decides the direction
    // on its own; otherwise the swipe commits past COMMIT_RATIO of the screen.
    constexpr double FLICK_SPEED  = 0.9;
    constexpr double COMMIT_RATIO = 0.5;
    constexpr int    VELOCITY_MS  = 80;

    struct SSwipe {
        PHLWORKSPACE  begin;
        PHLMONITORREF monitor;
        bool          vertical = false;
        WORKSPACEID   before   = WORKSPACE_INVALID; // revealed when content moves right/down
        WORKSPACEID   after    = WORKSPACE_INVALID; // revealed when content moves left/up
        double        delta    = 0;                 // accumulated finger movement
        std::deque<std::pair<uint32_t, double>> recent;
    };

    std::optional<SSwipe> g_swipe;
    PHLWORKSPACEREF       g_bumped;

    double swipeDistance() {
        static auto PSWIPEDIST = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_distance");
        return std::clamp<double>(*PSWIPEDIST, 1, UINT32_MAX);
    }

    double screenDistance(const SSwipe& s) {
        static auto PWORKSPACEGAP = CConfigValue<Config::INTEGER>("general:gaps_workspaces");
        const auto  MON           = s.monitor.lock();
        return (s.vertical ? MON->m_size.y : MON->m_size.x) + *PWORKSPACEGAP;
    }

    Vector2D along(const SSwipe& s, double v) {
        return s.vertical ? Vector2D{0.0, v} : Vector2D{v, 0.0};
    }

    double rubber(double frac) {
        const double A = std::abs(frac);
        return std::copysign((1.0 - 1.0 / (A * RUBBER_COEFF / RUBBER_LIMIT + 1.0)) * RUBBER_LIMIT, frac);
    }

    double velocity(const SSwipe& s) {
        if (s.recent.size() < 2)
            return 0;
        double sum = 0;
        for (auto const& [_, d] : s.recent)
            sum += d;
        const double DT = std::max<double>(s.recent.back().first - s.recent.front().first, 8.0);
        return sum / DT;
    }

    // The swipe can still be drawn: its workspace and monitor are both around.
    bool swipeAlive() {
        return g_swipe && live(g_swipe->begin) && !g_swipe->monitor.expired();
    }

    void render(SSwipe& s) {
        const auto   MON  = s.monitor.lock();
        const double DIST = screenDistance(s);
        double       frac = s.delta / swipeDistance();

        const bool   TOWARD_AFTER = frac < 0;
        const auto   ID           = TOWARD_AFTER ? s.after : s.before;
        const auto   NEIGHBOUR    = find(ID);
        hide(find(TOWARD_AFTER ? s.before : s.after));

        if (ID == WORKSPACE_INVALID)
            frac = rubber(frac);
        frac = std::clamp(frac, -1.0, 1.0);

        s.begin->m_forceRendering = true;
        s.begin->m_renderOffset->setValueAndWarp(along(s, frac * DIST));

        if (NEIGHBOUR) {
            NEIGHBOUR->m_forceRendering = true;
            NEIGHBOUR->m_alpha->setValueAndWarp(1.F);
            NEIGHBOUR->m_renderOffset->setValueAndWarp(along(s, frac * DIST + (TOWARD_AFTER ? DIST : -DIST)));
            NEIGHBOUR->updateWindowDecos();
        }

        s.begin->updateWindowDecos();
        g_pHyprRenderer->damageMonitor(MON);
    }

    void finish(bool cancelled) {
        auto&        s    = *g_swipe;
        const auto   MON  = s.monitor.lock();
        const double DIST = screenDistance(s);
        const double FRAC = std::clamp(s.delta / swipeDistance(), -1.0, 1.0);
        const double VEL  = velocity(s);

        const bool   TOWARD_AFTER = FRAC < 0;
        const auto   ID           = TOWARD_AFTER ? s.after : s.before;
        const double SIGN         = TOWARD_AFTER ? -1.0 : 1.0;

        bool         commit = false;
        if (!cancelled && ID != WORKSPACE_INVALID && std::abs(s.delta) >= 2) {
            if (std::abs(VEL) >= FLICK_SPEED)
                commit = (VEL < 0) == TOWARD_AFTER;
            else
                commit = std::abs(FRAC) >= COMMIT_RATIO;
        }

        auto before = find(s.before);
        auto after  = find(s.after);

        if (!commit) {
            // spring back
            if (auto n = TOWARD_AFTER ? after : before)
                *n->m_renderOffset = along(s, -SIGN * DIST);
            *s.begin->m_renderOffset = Vector2D{};
            applyFullscreenAlpha(MON, s.begin);
        } else {
            const Vector2D MIDDLE = s.begin->m_renderOffset->value();
            auto           target = find(ID);
            const Vector2D START  = target ? target->m_renderOffset->value() : along(s, FRAC * DIST - SIGN * DIST);

            if (target)
                MON->changeWorkspace(ID);
            else {
                MON->changeWorkspace(State::workspaceState()->create(ID, MON->m_id));
                target = find(ID);
            }

            // changeWorkspace starts the usual in/out animation; restart both from where
            // the fingers left them so the motion stays continuous.
            if (target) {
                target->m_renderOffset->setValue(START);
                target->m_alpha->setValueAndWarp(1.F);
            }
            s.begin->m_renderOffset->setValue(MIDDLE);
            *s.begin->m_renderOffset = along(s, SIGN * DIST);
            s.begin->m_alpha->setValueAndWarp(1.F);

            g_pInputManager->unconstrainMouse();
            if (target)
                applyFullscreenAlpha(MON, target);
        }

        if (before)
            before->m_forceRendering = false;
        if (after)
            after->m_forceRendering = false;
        s.begin->m_forceRendering = false;

        g_pHyprRenderer->damageMonitor(MON);
        g_swipe.reset();
        g_pInputManager->refocus();
    }

    std::optional<WORKSPACEID> optId(lua_State* L, int idx) {
        if (lua_isnoneornil(L, idx))
            return WORKSPACE_INVALID;
        if (!lua_isinteger(L, idx))
            return std::nullopt;
        return lua_tointeger(L, idx);
    }

    bool parseAxis(lua_State* L, int idx, bool& vertical) {
        const char* axis = luaL_checkstring(L, idx);
        if (std::string_view{axis} == "x")
            vertical = false;
        else if (std::string_view{axis} == "y")
            vertical = true;
        else
            return false;
        return true;
    }

    int luaSwipeBegin(lua_State* L) {
        SSwipe s;
        if (!parseAxis(L, 1, s.vertical))
            return luaL_error(L, "cross.swipe_begin: axis must be \"x\" or \"y\"");
        const auto BEFORE = optId(L, 2), AFTER = optId(L, 3);
        if (!BEFORE || !AFTER)
            return luaL_error(L, "cross.swipe_begin: workspace ids must be integers or nil");

        // A swipe whose end never arrived would otherwise block every later one.
        if (swipeAlive())
            finish(true);
        g_swipe.reset();

        // the overview owns every workspace's offset and alpha while it's up, closing included
        const auto MON = Desktop::focusState()->monitor();
        if (!MON || !live(MON->m_activeWorkspace) || overview::active() || g_pSessionLockManager->isSessionLocked() ||
            g_pUnifiedWorkspaceSwipe->isGestureInProgress()) {
            lua_pushboolean(L, false);
            return 1;
        }

        if (const auto BUMPED = g_bumped.lock(); live(BUMPED))
            BUMPED->m_renderOffset->setValueAndWarp(Vector2D{});
        g_bumped.reset();

        s.begin   = MON->m_activeWorkspace;
        s.monitor = MON;
        s.before  = *BEFORE;
        s.after   = *AFTER;
        g_swipe   = std::move(s);

        if (Fullscreen::controller()->getFullscreenWindow(g_swipe->begin))
            setTopLayerAlpha(MON, 1.F);

        lua_pushboolean(L, true);
        return 1;
    }

    int luaSwipeUpdate(lua_State* L) {
        const double   D    = luaL_checknumber(L, 1);
        const uint32_t TIME = sc<uint32_t>(luaL_optinteger(L, 2, 0));
        if (!g_swipe)
            return 0;
        if (!swipeAlive()) {
            g_swipe.reset();
            return 0;
        }

        // Toward a neighbour, travel past a full screen has nothing more to show;
        // banking it would make turning back do nothing until it's unwound. Past
        // an edge it keeps counting: that's the rubber band.
        auto&        s   = *g_swipe;
        const double MAX = swipeDistance();
        s.delta += D;
        if (s.after != WORKSPACE_INVALID)
            s.delta = std::max(s.delta, -MAX);
        if (s.before != WORKSPACE_INVALID)
            s.delta = std::min(s.delta, MAX);
        s.recent.emplace_back(TIME, D);
        while (s.recent.size() > 2 && TIME - s.recent.front().first > VELOCITY_MS)
            s.recent.pop_front();

        render(s);
        return 0;
    }

    int luaSwipeEnd(lua_State* L) {
        const bool CANCELLED = lua_toboolean(L, 1);
        if (!g_swipe)
            return 0;
        if (!swipeAlive()) {
            g_swipe.reset();
            return 0;
        }
        finish(CANCELLED);
        return 0;
    }

    int luaBump(lua_State* L) {
        bool vertical = false;
        if (!parseAxis(L, 1, vertical))
            return luaL_error(L, "cross.bump: axis must be \"x\" or \"y\"");
        const double PX  = luaL_checknumber(L, 2);
        const auto   MON = Desktop::focusState()->monitor();
        if (g_swipe || overview::active() || !MON || !live(MON->m_activeWorkspace))
            return 0;

        const auto WS = MON->m_activeWorkspace;
        *WS->m_renderOffset = vertical ? Vector2D{0.0, PX} : Vector2D{PX, 0.0};
        g_bumped        = WS;
        return 0;
    }

    int luaSettle(lua_State* L) {
        if (const auto WS = g_bumped.lock(); live(WS) && !g_swipe)
            *WS->m_renderOffset = Vector2D{};
        g_bumped.reset();
        return 0;
    }

    int luaActive(lua_State* L) {
        lua_pushboolean(L, g_swipe.has_value());
        return 1;
    }
}

bool cross::swiping() {
    return g_swipe.has_value();
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();
    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprcross] Built for a different Hyprland, rebuild it (make install)", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprcross] version mismatch");
    }

    bool ok = true;
    ok &= HyprlandAPI::addLuaFunction(PHANDLE, "cross", "swipe_begin", luaSwipeBegin);
    ok &= HyprlandAPI::addLuaFunction(PHANDLE, "cross", "swipe_update", luaSwipeUpdate);
    ok &= HyprlandAPI::addLuaFunction(PHANDLE, "cross", "swipe_end", luaSwipeEnd);
    ok &= HyprlandAPI::addLuaFunction(PHANDLE, "cross", "bump", luaBump);
    ok &= HyprlandAPI::addLuaFunction(PHANDLE, "cross", "settle", luaSettle);
    ok &= HyprlandAPI::addLuaFunction(PHANDLE, "cross", "active", luaActive);
    ok &= HyprlandAPI::addLuaFunction(PHANDLE, "cross", "overview_begin", overview::luaBegin);
    ok &= HyprlandAPI::addLuaFunction(PHANDLE, "cross", "overview_update", overview::luaUpdate);
    ok &= HyprlandAPI::addLuaFunction(PHANDLE, "cross", "overview_end", overview::luaEnd);
    ok &= HyprlandAPI::addLuaFunction(PHANDLE, "cross", "overview_drag", overview::luaDrag);
    ok &= HyprlandAPI::addLuaFunction(PHANDLE, "cross", "overview_release", overview::luaRelease);
    ok &= HyprlandAPI::addLuaFunction(PHANDLE, "cross", "overview_move", overview::luaMove);
    ok &= HyprlandAPI::addLuaFunction(PHANDLE, "cross", "overview_mode", overview::luaMode);
    ok &= HyprlandAPI::addLuaFunction(PHANDLE, "cross", "overview_selected", overview::luaSelected);
    ok &= HyprlandAPI::addLuaFunction(PHANDLE, "cross", "overview_close", overview::luaClose);
    ok &= HyprlandAPI::addLuaFunction(PHANDLE, "cross", "overview_active", overview::luaActive);
    if (!ok)
        throw std::runtime_error("[hyprcross] couldn't register Lua functions (is the config Lua?)");

    overview::init();

    return {"hyprcross", "Finger-tracking swipes and a pinch overview for PS-style cross workspaces", "miko", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    overview::exit();
    if (swipeAlive())
        finish(true);
    g_swipe.reset();
    if (const auto WS = g_bumped.lock(); live(WS))
        *WS->m_renderOffset = Vector2D{};
    g_bumped.reset();
}
