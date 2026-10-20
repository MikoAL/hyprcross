// The pinch overview. Every frame while it is up, each visible cell's workspace
// is rendered into its own framebuffer before the monitor renders, and the
// thumbnails are drawn over the (emptied) desktop with Hyprland's stock pass
// elements. One animated value, `progress`, takes it from the plain desktop (0)
// to the grid (1); the anchor thumbnail is the one that grows to fill the
// screen at 0 (where it opened from, or where it is closing to).
//
// Selection is a highlight frame of its own that springs from thumbnail to
// thumbnail, one step at a time like the arrow keys; finger travel (3-finger
// swipes, 2-finger scrolling) is counted out into those steps. Thumbnails swell
// as it nears them and the view trails behind it. Opening, it starts as the
// screen's edge and shrinks with the current workspace; closing, it grows back
// out with the picked one.
//
// Drawing hooks into render.stage (the last moment, so it covers everything,
// fullscreen windows included) rather than replacing renderWorkspace like
// hyprexpo does, so both plugins can be loaded at once.

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/pass/ClearPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/lua/ConfigManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/helpers/time/Time.hpp>

#include "shared.hpp"
#include "overview.hpp"

extern "C" {
#include <lauxlib.h>
}

#include <deque>
#include <optional>

#include <unordered_map>

namespace {
    // Layout, as fractions of the screen: thumbnail size, how much the one under
    // the highlight grows, and the gaps between thumbnails.
    constexpr double THUMB_FRAC     = 0.2;
    constexpr double SELECTED_SCALE = 1.3;
    constexpr double GAP_X          = 0.04;
    constexpr double GAP_Y          = 0.04;

    // How much the wallpaper darkens, and how dim the unselected thumbnails are.
    constexpr float BACKDROP_DIM     = 0.5F;
    constexpr float UNSELECTED_ALPHA = 0.75F;
    constexpr float EMPTY_ALPHA      = 0.08F; // tile for a slot with no workspace yet

    // Thumbnails are rendered at this fraction of the screen's resolution
    // (enough for the biggest one, THUMB_FRAC * SELECTED_SCALE); only the one
    // zooming to or from full screen gets full resolution, and only mid-zoom.
    constexpr double THUMB_RES = 0.35;

    // The highlighted and zooming thumbnails redraw every frame; the rest take
    // turns, each redrawing once every this many frames.
    constexpr int BACKGROUND_EVERY = 3;
    constexpr int   BORDER_PX        = 3;

    // Pinch: how far the fingers close (in libinput scale) for the full zoom, and
    // when a release opens it: a flick faster than FLICK_SPEED (progress per ms),
    // otherwise past OPEN_RATIO.
    constexpr double PINCH_RANGE = 0.5;
    constexpr double OPEN_RATIO  = 0.5;
    constexpr double FLICK_SPEED = 0.004;
    constexpr int    VELOCITY_MS = 80;

    // Moving the highlight: finger travel per step, the same for 3-finger swipes
    // and 2-finger scrolling (both arrive as raw touchpad movement; scroll_factor
    // isn't applied yet). Vertical steps take less: the pad is much shorter than
    // it is wide. And how long scrolling must pause to count as letting go.
    constexpr double STEP_DIST      = 110.0;
    constexpr double STEP_DIST_Y    = 75.0;

    // Like key repeat: within one drag, the second step takes this many steps'
    // worth of travel, so a small swipe gives exactly one step and only a long
    // one keeps going.
    constexpr double REPEAT_DELAY = 1.8;
    constexpr int    SCROLL_IDLE_MS = 60;

    // A drag steps along one axis only, picked by whichever way the fingers go
    // first; this much movement (fraction of a step) decides it.
    constexpr double AXIS_LOCK_DIST = 0.1;

    // The highlight frame is hidden while the overview is this close to shut,
    // so it doesn't linger on the screen edge.
    constexpr float FRAME_FADE = 0.15F;

    // How far behind the highlight the view trails (time constant, ms).
    constexpr double CAMERA_LAG_MS = 70.0;

    // renderWorkspace is protected. hyprexpo gets at it with #define protected
    // public, but that can change class layouts (tail padding) and so misread
    // Hyprland's own objects; a derived class can hand out the member pointer instead.
    struct SRendererAccess : Render::IHyprRenderer {
        static constexpr auto RENDER_WORKSPACE = &SRendererAccess::renderWorkspace;
    };

    void renderWorkspace(const PHLMONITOR& mon, const PHLWORKSPACE& ws, const Time::steady_tp& now, const CBox& box) {
        (g_pHyprRenderer.get()->*SRendererAccess::RENDER_WORKSPACE)(mon, ws, now, box);
    }

    struct SCell {
        WORKSPACEID id = WORKSPACE_INVALID;
        double      x = 0, y = 0; // grid position
    };

    struct SOverview {
        PHLMONITORREF                                             monitor;
        PHLWORKSPACEREF                                           startedOn;
        std::vector<SCell>                                        cells;
        WORKSPACEID                                               target = WORKSPACE_INVALID; // thumbnail the highlight is on or heading to
        WORKSPACEID                                               anchor = WORKSPACE_INVALID;
        std::unordered_map<WORKSPACEID, SP<Render::IFramebuffer>> fbs;

        PHLANIMVAR<float>                                         progress;
        PHLANIMVAR<Vector2D>                                      highlight;  // grid position of the highlight frame
        Vector2D                                                  camera;     // grid position at the screen centre
        Time::steady_tp                                           cameraTime; // last camera step
        bool                                                      dragging = false;
        uint64_t                                                  frame    = 0;
        Vector2D                                                  travel; // finger travel not yet turned into steps
        int                                                       dragSteps = 0;
        enum : uint8_t { AXIS_NONE, AXIS_X, AXIS_Y }              dragAxis = AXIS_NONE;
        SP<CEventLoopTimer>                                       scrollTimer;
        std::deque<std::pair<uint32_t, double>>                   recent;
        bool                                                      tracking = false;
        bool                                                      closing  = false;
        bool                                                      finished = false; // restore the desktop on the next frame, then go
        bool                                                      restored = false;

        int                                                       pickRef = LUA_NOREF;

        CHyprSignalListener preListener, stageListener, buttonListener, moveListener, axisListener, workspaceListener, monitorListener;
    };

    std::optional<SOverview> g_ov;
    bool                     g_renderingThumbs = false;
    bool                     g_teardownQueued  = false;

    Vector2D                 lerp(const Vector2D& a, const Vector2D& b, double t) {
        return a + (b - a) * t;
    }

    CBox lerp(const CBox& a, const CBox& b, double t) {
        return {lerp(a.pos(), b.pos(), t), lerp(a.size(), b.size(), t)};
    }

    Vector2D posOf(const SCell& c) {
        return {c.x, c.y};
    }

    // 1 with the highlight right on the cell, falling to 0 a cell away.
    double nearness(const SOverview& o, const SCell& c) {
        const auto D = o.highlight->value() - posOf(c);
        return std::clamp(1.0 - std::sqrt(D.x * D.x + D.y * D.y), 0.0, 1.0);
    }

    // Logical, monitor-local box of something `scale` thumbnails big at grid `pos`.
    CBox gridBox(const SOverview& o, const PHLMONITOR& mon, const Vector2D& pos, double scale) {
        const Vector2D STEP{mon->m_size.x * (THUMB_FRAC + GAP_X), mon->m_size.y * (THUMB_FRAC + GAP_Y)};
        const Vector2D SIZE   = mon->m_size * THUMB_FRAC * scale;
        const Vector2D CENTRE = mon->m_size / 2.0 + (pos - o.camera) * STEP;
        return {CENTRE - SIZE / 2.0, SIZE};
    }

    CBox cellBox(const SOverview& o, const PHLMONITOR& mon, const SCell& c) {
        return gridBox(o, mon, posOf(c), 1.0 + (SELECTED_SCALE - 1.0) * nearness(o, c));
    }

    const SCell* cellFor(const SOverview& o, WORKSPACEID id) {
        for (auto const& c : o.cells)
            if (c.id == id)
                return &c;
        return nullptr;
    }

    void damage() {
        if (!g_ov)
            return;
        if (const auto MON = g_ov->monitor.lock())
            g_pHyprRenderer->damageMonitor(MON);
    }

    std::optional<std::vector<SCell>> parseCells(lua_State* L, int idx) {
        if (!lua_istable(L, idx))
            return std::nullopt;
        std::vector<SCell> cells;
        const auto         N = luaL_len(L, idx);
        for (lua_Integer i = 1; i <= N; ++i) {
            lua_geti(L, idx, i);
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                return std::nullopt;
            }
            SCell c;
            lua_getfield(L, -1, "id");
            lua_getfield(L, -2, "x");
            lua_getfield(L, -3, "y");
            if (!lua_isinteger(L, -3) || !lua_isnumber(L, -2) || !lua_isnumber(L, -1)) {
                lua_pop(L, 4);
                return std::nullopt;
            }
            c.id = lua_tointeger(L, -3);
            c.x  = lua_tonumber(L, -2);
            c.y  = lua_tonumber(L, -1);
            lua_pop(L, 4);
            cells.push_back(c);
        }
        return cells;
    }

    // The overview is done. Workspaces get put back from the next render.pre
    // (restore()), so the frame that drops the overlay already shows the real
    // desktop; the rest is freed later from the event loop, outside the signals
    // whose listeners it drops.
    void finish() {
        if (!g_ov || g_ov->finished)
            return;
        g_ov->closing  = true;
        g_ov->finished = true;
        damage();
    }

    void queueTeardown() {
        if (g_teardownQueued)
            return;
        g_teardownQueued = true;
        g_pEventLoopManager->doLater([] {
            g_teardownQueued = false;
            cross::overview::teardown();
        });
    }

    // Workspaces back to normal: the active one shown, the rest hidden.
    void restore(const PHLMONITOR& mon) {
        for (auto const& c : g_ov->cells) {
            const auto WS = cross::find(c.id);
            if (!WS)
                continue;
            if (WS == mon->m_activeWorkspace) {
                WS->m_forceRendering = false;
                WS->m_visible        = true;
                WS->m_alpha->setValueAndWarp(1.F);
                WS->m_renderOffset->setValueAndWarp(Vector2D{});
            } else
                cross::hide(WS);
        }
        if (mon->m_activeWorkspace)
            cross::applyFullscreenAlpha(mon, mon->m_activeWorkspace);
    }

    // Spring the highlight (and with it the view) onto a thumbnail.
    void snapTo(const SCell& c) {
        auto& o      = *g_ov;
        o.target     = c.id;
        *o.highlight = posOf(c);
        damage();
    }

    // Keys, mouse wheels and counted-out finger travel: one step to the
    // neighbouring thumbnail, if there is one.
    bool moveBy(int dx, int dy) {
        auto&      o   = *g_ov;
        const auto CUR = cellFor(o, o.target);
        if (!CUR)
            return false;
        const SCell* best = nullptr;
        for (auto const& c : o.cells) {
            if (dx && c.x == CUR->x + dx && (!best || std::abs(c.y - CUR->y) < std::abs(best->y - CUR->y)))
                best = &c;
            if (dy && c.x == CUR->x && c.y == CUR->y + dy)
                best = &c;
        }
        if (best)
            snapTo(*best);
        return best;
    }

    // Fingers moved `d` steps' worth. Whole steps move the highlight; the first
    // bit of travel picks the axis for the rest of the drag.
    void drag(const Vector2D& d) {
        auto& o = *g_ov;
        if (!o.dragging) {
            o.dragging = true;
            o.dragAxis  = SOverview::AXIS_NONE;
            o.travel    = {};
            o.dragSteps = 0;
        }
        o.travel = o.travel + d;

        if (o.dragAxis == SOverview::AXIS_NONE) {
            if (std::max(std::abs(o.travel.x), std::abs(o.travel.y)) < AXIS_LOCK_DIST)
                return;
            o.dragAxis = std::abs(o.travel.x) >= std::abs(o.travel.y) ? SOverview::AXIS_X : SOverview::AXIS_Y;
        }

        double& t = o.dragAxis == SOverview::AXIS_X ? o.travel.x : o.travel.y;
        while (true) {
            const double NEED = o.dragSteps == 1 ? REPEAT_DELAY : 1.0;
            if (std::abs(t) < NEED)
                break;
            const int DIR = t > 0 ? 1 : -1;
            if (!(o.dragAxis == SOverview::AXIS_X ? moveBy(DIR, 0) : moveBy(0, DIR))) {
                t = 0; // at the edge: don't bank travel, so turning back responds at once
                break;
            }
            t -= DIR * NEED;
            o.dragSteps++;
        }
    }

    void release() {
        if (g_ov)
            g_ov->dragging = false;
    }

    // Zoom back into `id` and go away.
    void startClosing(WORKSPACEID id) {
        auto& o   = *g_ov;
        o.closing = true;
        o.anchor  = id;
        if (const auto C = cellFor(o, id))
            *o.highlight = posOf(*C);
        if (o.progress->value() <= 0.001F) {
            finish();
            return;
        }
        *o.progress = 0.F;
        o.progress->setCallbackOnEnd([](auto) { finish(); });
    }

    void renderThumbs(const PHLMONITOR& mon) {
        auto&      o   = *g_ov;
        const auto NOW = Time::steadyNow();

        const auto ACTIVE  = mon->m_activeWorkspace;
        const auto SPECIAL = mon->m_activeSpecialWorkspace;
        mon->m_activeSpecialWorkspace.reset();

        g_renderingThumbs                        = true;
        g_pHyprRenderer->m_bBlockSurfaceFeedback = true;

        const CBox SCREEN{{}, mon->m_size};
        o.frame++;
        for (size_t i = 0; i < o.cells.size(); ++i) {
            auto const& c = o.cells[i];
            const auto WS = cross::find(c.id);
            if (!WS) {
                o.fbs.erase(c.id);
                continue;
            }
            if (c.id != o.anchor && !cellBox(o, mon, c).intersection(SCREEN).size().x)
                continue;

            auto&      fb       = o.fbs[c.id];
            const bool ZOOMING  = o.progress->value() < 0.999F;
            const bool PRIORITY = c.id == o.target || (c.id == o.anchor && ZOOMING);
            if (fb && fb->isAllocated() && !PRIORITY && (o.frame + i) % BACKGROUND_EVERY)
                continue;
            if (!fb)
                fb = g_pHyprRenderer->createFB("hyprcross overview");
            const bool     FULL_RES = c.id == o.anchor && o.progress->value() < 0.999F;
            const Vector2D SIZE     = FULL_RES ? mon->m_pixelSize : (mon->m_pixelSize * THUMB_RES).round();

            // set up like Hyprland's own window snapshots (makeSnapshotFB); shaders
            // read the framebuffer's image description
            if (fb->m_size != SIZE)
                fb->alloc(SIZE.x, SIZE.y, DRM_FORMAT_ABGR8888);
            fb->setImageDescription(mon->workBufferImageDescription());

            CRegion fakeDamage{CBox{{}, mon->m_transformedSize}};
            if (!g_pHyprRenderer->beginFullFakeRender(mon, fakeDamage, fb))
                continue;

            g_pHyprRenderer->draw(CClearPassElement::SClearData{CHyprColor{0, 0, 0, 1}});
            g_pHyprRenderer->startRenderPass();

            const bool VISIBLE = WS->m_visible;
            mon->m_activeWorkspace = WS;
            WS->m_visible          = true;
            WS->m_alpha->setValueAndWarp(1.F);

            renderWorkspace(mon, WS, NOW, CBox{{}, SIZE});
            g_pHyprRenderer->endRender();

            WS->m_alpha->setValueAndWarp(0.F);
            WS->m_visible = VISIBLE;
            g_pHyprRenderer->sendFrameEventsToWorkspace(mon, WS, NOW);
        }

        g_pHyprRenderer->m_bBlockSurfaceFeedback = false;
        g_renderingThumbs                        = false;

        mon->m_activeWorkspace        = ACTIVE;
        mon->m_activeSpecialWorkspace = SPECIAL;
    }

    void onPreRender(PHLMONITOR mon) {
        if (!g_ov || g_ov->monitor != mon)
            return;
        if (g_ov->finished) {
            if (!g_ov->restored) {
                restore(mon);
                g_ov->restored = true;
                queueTeardown();
            }
            return;
        }
        if (g_pSessionLockManager->isSessionLocked()) {
            finish();
            return;
        }

        auto& o = *g_ov;
        // Keep every listed workspace live (damage and frame callbacks) but out of
        // the normal pass: the thumbnails draw them instead.
        for (auto const& c : o.cells) {
            if (const auto WS = cross::find(c.id)) {
                WS->m_forceRendering = true;
                WS->m_alpha->setValueAndWarp(0.F);
                WS->m_renderOffset->setValueAndWarp(Vector2D{});
            }
        }

        // the view trails the highlight
        const auto   NOW  = Time::steadyNow();
        const double DT   = std::chrono::duration<double, std::milli>(NOW - o.cameraTime).count();
        o.cameraTime      = NOW;
        const auto   GAP  = o.highlight->value() - o.camera;
        o.camera          = o.camera + GAP * (1.0 - std::exp(-std::clamp(DT, 0.0, 100.0) / CAMERA_LAG_MS));
        if (std::abs(GAP.x) + std::abs(GAP.y) > 0.001)
            damage();

        renderThumbs(mon);

        mon->m_forceFullFrames = std::max(mon->m_forceFullFrames, 1);
    }

    void onStage(eRenderStage stage) {
        if (stage != RENDER_LAST_MOMENT || !g_ov || g_ov->finished || g_renderingThumbs)
            return;

        auto&      o   = *g_ov;
        const auto MON = o.monitor.lock();
        if (!MON || g_pHyprRenderer->renderData().pMonitor != MON)
            return;

        static auto     PROUNDING = CConfigValue<Config::INTEGER>("decoration:rounding");
        static auto     PBORDER   = CConfigValue<Config::IComplexConfigValue>("general:col.active_border");

        const float     P     = o.progress->value();
        const double    SCALE = MON->m_scale;
        const CBox      FULL{{}, MON->m_size};
        const CRegion   DAMAGE{0, 0, INT16_MAX, INT16_MAX};

        auto            toPixels = [&](CBox box) { return box.scale(SCALE).round(); };

        CRectPassElement::SRectData backdrop;
        backdrop.box   = toPixels(FULL);
        backdrop.color = CHyprColor{0, 0, 0, BACKDROP_DIM * P};
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(backdrop));

        auto drawCell = [&](const SCell& c) {
            const float RESTA = UNSELECTED_ALPHA + (1.F - UNSELECTED_ALPHA) * nearness(o, c);
            const auto  FB    = o.fbs.find(c.id);
            if (FB == o.fbs.end() || !FB->second->isAllocated()) {
                // a slot with no workspace yet: a faint tile, so it's clear it's there
                CRectPassElement::SRectData empty;
                empty.box   = toPixels(cellBox(o, MON, c));
                empty.color = CHyprColor{1, 1, 1, EMPTY_ALPHA * RESTA * P};
                empty.round = std::round(*PROUNDING * SCALE);
                g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(empty));
                return;
            }

            CBox        box   = cellBox(o, MON, c);
            float       a     = RESTA * P;
            double      round = *PROUNDING;
            if (c.id == o.anchor) {
                box   = lerp(FULL, box, P);
                a     = 1.F + (RESTA - 1.F) * P;
                round = round * P;
            }

            CTexPassElement::SRenderData tex;
            tex.tex    = FB->second->getTexture();
            tex.box    = toPixels(box);
            tex.a      = a;
            tex.damage = DAMAGE;
            tex.round  = std::round(round * SCALE);
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(std::move(tex)));
        };

        // nearest the highlight on top, the anchor over everything while it zooms
        std::vector<const SCell*> order;
        for (auto const& c : o.cells)
            if (c.id != o.anchor)
                order.push_back(&c);
        std::ranges::sort(order, {}, [&](const SCell* c) { return nearness(o, *c); });
        for (auto const* c : order)
            drawCell(*c);
        if (const auto ANCHOR = cellFor(o, o.anchor))
            drawCell(*ANCHOR);

        // the frame is the screen's edge at 0 and wraps the highlighted thumbnail at 1
        CBorderPassElement::SBorderData border;
        border.box = toPixels(lerp(FULL, gridBox(o, MON, o.highlight->value(), SELECTED_SCALE), P));
        if (const auto GRAD = dynamic_cast<Config::CGradientValueData*>(PBORDER.ptr()))
            border.grad1 = *GRAD;
        else
            border.grad1 = Config::CGradientValueData{CHyprColor{1, 1, 1, 1}};
        border.a          = std::clamp(P / FRAME_FADE, 0.F, 1.F);
        border.round      = std::round(*PROUNDING * P * SCALE);
        border.borderSize = std::round(BORDER_PX * SCALE);
        g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));
    }

    void callPick(WORKSPACEID id) {
        // mgr() is uniquely owned: lock()ing it terminates Hyprland
        const auto MGR = Config::Lua::mgr();
        if (!g_ov || g_ov->pickRef == LUA_NOREF || !MGR)
            return;
        MGR->callLuaFn(
            g_ov->pickRef,
            [id](lua_State* L) {
                lua_pushinteger(L, id);
                return 1;
            },
            Config::Lua::CConfigManager::LUA_TIMEOUT_EVENT_CALLBACK_MS, "hyprcross overview pick");
    }

    void onButton(const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) {
        if (!g_ov || g_ov->closing)
            return;
        info.cancelled = true;
        if (e.state != WL_POINTER_BUTTON_STATE_PRESSED || g_ov->tracking)
            return;

        const auto MON = g_ov->monitor.lock();
        if (!MON)
            return;
        const auto POS = g_pInputManager->getMouseCoordsInternal() - MON->m_position;
        for (auto const& c : g_ov->cells) {
            if (cellBox(*g_ov, MON, c).containsPoint(POS)) {
                callPick(c.id);
                return;
            }
        }
    }

    void listen(SOverview& o) {
        auto& ev            = Event::bus()->m_events;
        o.preListener       = ev.render.pre.listen([](PHLMONITOR mon) { onPreRender(mon); });
        o.stageListener     = ev.render.stage.listen([](eRenderStage stage) { onStage(stage); });
        o.buttonListener    = ev.input.mouse.button.listen([](const IPointer::SButtonEvent& e, Event::SCallbackInfo& info) { onButton(e, info); });
        // no hover: keep focus where it is while the cursor wanders over thumbnails
        o.moveListener      = ev.input.mouse.move.listen([](const Vector2D&, Event::SCallbackInfo& info) {
            if (g_ov && !g_ov->closing)
                info.cancelled = true;
        });
        // 2-finger scrolling drags the highlight; it settles once scrolling pauses.
        // A mouse wheel steps it instead. Nothing scrolls underneath.
        o.axisListener = ev.input.mouse.axis.listen([](const IPointer::SAxisEvent& e, Event::SCallbackInfo& info) {
            if (!g_ov || g_ov->closing)
                return;
            info.cancelled         = true;
            const bool HORIZONTAL = e.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL;
            if (e.source == WL_POINTER_AXIS_SOURCE_WHEEL) {
                if (e.deltaDiscrete)
                    moveBy(HORIZONTAL ? (e.deltaDiscrete > 0 ? 1 : -1) : 0, HORIZONTAL ? 0 : (e.deltaDiscrete > 0 ? 1 : -1));
                return;
            }
            if (e.delta == 0)
                return release();
            drag(HORIZONTAL ? Vector2D{e.delta / STEP_DIST, 0.0} : Vector2D{0.0, e.delta / STEP_DIST_Y});
            g_ov->scrollTimer->updateTimeout(std::chrono::milliseconds(SCROLL_IDLE_MS));
        });
        o.workspaceListener = ev.workspace.active.listen([](PHLWORKSPACE ws) {
            // something other than a pick switched workspaces under us
            if (g_ov && !g_ov->closing && ws && ws->m_monitor == g_ov->monitor && ws != g_ov->startedOn)
                finish();
        });
        o.monitorListener   = ev.monitor.removed.listen([](PHLMONITOR mon) {
            if (g_ov && g_ov->monitor == mon)
                finish();
        });
    }
}

namespace cross::overview {
    bool active() {
        return g_ov.has_value();
    }

    void teardown() {
        if (!g_ov)
            return;

        auto&      o   = *g_ov;
        const auto MON = o.monitor.lock();

        // Normally restore() already ran from render.pre. If not (plugin unload,
        // monitor gone), just stop forcing them to render; no warping here.
        if (!o.restored) {
            for (auto const& c : o.cells)
                if (const auto WS = cross::find(c.id))
                    WS->m_forceRendering = false;
            if (MON && MON->m_activeWorkspace)
                *MON->m_activeWorkspace->m_alpha = 1.F;
        }

        if (o.scrollTimer) {
            o.scrollTimer->cancel();
            g_pEventLoopManager->removeTimer(o.scrollTimer);
        }

        // glBackend() is uniquely owned: lock()ing it terminates Hyprland
        if (const auto GL = g_pHyprRenderer->glBackend())
            GL->makeEGLCurrent();
        o.fbs.clear();

        if (MON)
            g_pHyprRenderer->damageMonitor(MON);

        g_ov.reset();
        g_pInputManager->refocus();
    }

    int luaBegin(lua_State* L) {
        const auto CELLS = parseCells(L, 1);
        if (!CELLS)
            return luaL_error(L, "cross.overview_begin: cells must be a list of {id, x, y}");
        const auto SELECTED = luaL_checkinteger(L, 2);
        if (!lua_isnoneornil(L, 3))
            luaL_checktype(L, 3, LUA_TFUNCTION);

        const auto MON = Desktop::focusState()->monitor();
        if (g_ov || cross::swiping() || !MON || !MON->m_activeWorkspace || g_pSessionLockManager->isSessionLocked()) {
            lua_pushboolean(L, false);
            return 1;
        }

        g_ov.emplace();
        auto& o     = *g_ov;
        o.monitor   = MON;
        o.startedOn = MON->m_activeWorkspace;
        o.cells     = *CELLS;
        o.target    = SELECTED;
        o.anchor    = MON->m_activeWorkspace->m_id;

        if (!lua_isnoneornil(L, 3)) {
            lua_pushvalue(L, 3);
            o.pickRef = luaL_ref(L, LUA_REGISTRYINDEX);
        }

        const auto CONFIG = Config::animationTree()->getAnimationPropertyConfig("workspaces");
        Animation::mgr()->createAnimation(0.F, o.progress, CONFIG, AVARDAMAGE_NONE);
        const auto START  = cellFor(o, SELECTED) ? posOf(*cellFor(o, SELECTED)) : Vector2D{};
        Animation::mgr()->createAnimation(START, o.highlight, CONFIG, AVARDAMAGE_NONE);
        o.progress->setUpdateCallback([](auto) { damage(); });
        o.highlight->setUpdateCallback([](auto) { damage(); });
        o.camera     = START;
        o.cameraTime = Time::steadyNow();

        o.scrollTimer = makeShared<CEventLoopTimer>(std::nullopt, [](SP<CEventLoopTimer>, void*) {
            if (g_ov && !g_ov->closing)
                release();
        }, nullptr);
        g_pEventLoopManager->addTimer(o.scrollTimer);

        // the bar fades out on its own animation; warping layer alphas from a
        // plugin aborts Hyprland
        cross::setTopLayerAlpha(MON, 0.F);

        listen(o);
        damage();

        lua_pushboolean(L, true);
        return 1;
    }

    int luaUpdate(lua_State* L) {
        const double   SCALE = luaL_checknumber(L, 1);
        const uint32_t TIME  = sc<uint32_t>(luaL_optinteger(L, 2, 0));
        if (!g_ov || g_ov->closing)
            return 0;

        auto&        o    = *g_ov;
        const double P    = std::clamp((1.0 - SCALE) / PINCH_RANGE, 0.0, 1.0);
        const double PREV = o.progress->value();
        o.tracking        = true;
        o.recent.emplace_back(TIME, P - PREV);
        while (o.recent.size() > 2 && TIME - o.recent.front().first > VELOCITY_MS)
            o.recent.pop_front();

        o.progress->setValueAndWarp(P);
        damage();
        return 0;
    }

    int luaEnd(lua_State* L) {
        const bool CANCELLED = lua_toboolean(L, 1);
        if (!g_ov || g_ov->closing) {
            lua_pushboolean(L, g_ov && !g_ov->closing);
            return 1;
        }

        auto& o    = *g_ov;
        o.tracking = false;

        double vel = 0;
        if (o.recent.size() >= 2) {
            double sum = 0;
            for (auto const& [_, d] : o.recent)
                sum += d;
            vel = sum / std::max<double>(o.recent.back().first - o.recent.front().first, 8.0);
        }
        o.recent.clear();

        bool open = false;
        if (!CANCELLED) {
            if (std::abs(vel) >= FLICK_SPEED)
                open = vel > 0;
            else
                open = o.progress->value() >= OPEN_RATIO;
        }

        if (open)
            *o.progress = 1.F;
        else
            startClosing(o.startedOn ? o.startedOn->m_id : o.anchor);

        lua_pushboolean(L, open);
        return 1;
    }

    int luaDrag(lua_State* L) {
        const double DX = luaL_checknumber(L, 1);
        const double DY = luaL_checknumber(L, 2);
        if (!g_ov || g_ov->closing)
            return 0;
        // the grid follows the fingers, so the highlight goes the other way
        drag(Vector2D{-DX / STEP_DIST, -DY / STEP_DIST_Y});
        return 0;
    }

    int luaRelease(lua_State* L) {
        if (g_ov && !g_ov->closing)
            release();
        return 0;
    }

    int luaMove(lua_State* L) {
        const auto DX = luaL_checkinteger(L, 1);
        const auto DY = luaL_checkinteger(L, 2);
        if (g_ov && !g_ov->closing)
            moveBy(sc<int>(DX), sc<int>(DY));
        return 0;
    }

    int luaSelected(lua_State* L) {
        if (!g_ov)
            return 0;
        lua_pushinteger(L, g_ov->target);
        return 1;
    }

    int luaClose(lua_State* L) {
        if (!g_ov || g_ov->closing)
            return 0;
        WORKSPACEID id = g_ov->startedOn ? g_ov->startedOn->m_id : g_ov->anchor;
        if (lua_isinteger(L, 1))
            id = lua_tointeger(L, 1);
        g_ov->tracking = false;
        startClosing(id);
        return 0;
    }

    int luaActive(lua_State* L) {
        lua_pushboolean(L, g_ov && !g_ov->closing);
        return 1;
    }
}
