// The pinch overview. Every frame while it is up, each visible cell's workspace
// is rendered into its own framebuffer before the monitor renders, and the
// thumbnails are drawn over the (emptied) desktop with Hyprland's stock pass
// elements. One animated value, `progress`, takes it from the plain desktop (0)
// to the cross (1); the anchor thumbnail is the one that grows to fill the
// screen at 0 (where it opened from, or where it is closing to).
//
// Drawing hooks into render.stage rather than replacing renderWorkspace like
// hyprexpo does, so both plugins can be loaded at once.

// renderWorkspace is protected; this is how hyprexpo reaches it too.
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#undef protected

#include <hyprland/src/plugins/PluginAPI.hpp>
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
    // Layout, as fractions of the screen: thumbnail size, the selected one's
    // extra scale, and the gaps between thumbnails.
    constexpr double THUMB_FRAC     = 0.2;
    constexpr double SELECTED_SCALE = 1.3;
    constexpr double GAP_X          = 0.04;
    constexpr double GAP_Y          = 0.04;

    // How much the wallpaper darkens, and how dim the unselected thumbnails are.
    constexpr float BACKDROP_DIM     = 0.5F;
    constexpr float UNSELECTED_ALPHA = 0.75F;
    constexpr int   BORDER_PX        = 3;

    // Pinch: how far the fingers close (in libinput scale) for the full zoom, and
    // when a release opens it: a flick faster than FLICK_SPEED (progress per ms),
    // otherwise past OPEN_RATIO.
    constexpr double PINCH_RANGE = 0.5;
    constexpr double OPEN_RATIO  = 0.5;
    constexpr double FLICK_SPEED = 0.004;
    constexpr int    VELOCITY_MS = 80;

    struct SCell {
        WORKSPACEID id = WORKSPACE_INVALID;
        double      x = 0, y = 0; // grid steps from the selection
    };

    struct SOverview {
        PHLMONITORREF                                             monitor;
        PHLWORKSPACEREF                                           startedOn;
        std::vector<SCell>                                        cells;
        WORKSPACEID                                               selected = WORKSPACE_INVALID;
        WORKSPACEID                                               anchor   = WORKSPACE_INVALID;
        std::unordered_map<WORKSPACEID, SP<Render::IFramebuffer>> fbs;

        PHLANIMVAR<float>                                         progress;
        PHLANIMVAR<Vector2D>                                      offset; // grid position at the screen centre
        std::deque<std::pair<uint32_t, double>>                   recent;
        bool                                                      tracking = false;
        bool                                                      closing  = false;
        float                                                     topAlpha = 1.F; // what the bar goes back to

        int                                                       pickRef = LUA_NOREF;

        CHyprSignalListener                                       preListener, stageListener, buttonListener, moveListener, workspaceListener, monitorListener;
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

    // Logical, monitor-local box of a cell.
    CBox cellBox(const SOverview& o, const PHLMONITOR& mon, const SCell& c) {
        const Vector2D STEP{mon->m_size.x * (THUMB_FRAC + GAP_X), mon->m_size.y * (THUMB_FRAC + GAP_Y)};
        Vector2D       size = mon->m_size * THUMB_FRAC;
        if (c.id == o.selected)
            size = size * SELECTED_SCALE;
        const Vector2D CENTRE = mon->m_size / 2.0 + (Vector2D{c.x, c.y} - o.offset->value()) * STEP;
        return {CENTRE - size / 2.0, size};
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

    // The column a workspace id belongs to (cross.lua's ids are 10 * column + slot).
    WORKSPACEID column(WORKSPACEID id) {
        return id < 10 ? id : id / 10;
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

    void queueTeardown() {
        // Not from inside the animation callback that asks for it: that would
        // destroy the animated variable mid-callback.
        if (g_teardownQueued)
            return;
        g_teardownQueued = true;
        g_pEventLoopManager->doLater([] {
            g_teardownQueued = false;
            cross::overview::teardown();
        });
    }

    // Zoom back into `id` and go away.
    void startClosing(WORKSPACEID id) {
        auto& o   = *g_ov;
        o.closing = true;
        o.anchor  = id;
        if (o.progress->value() <= 0.001F) {
            queueTeardown();
            return;
        }
        *o.progress = 0.F;
        o.progress->setCallbackOnEnd([](auto) { queueTeardown(); });
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
        for (auto const& c : o.cells) {
            const auto WS = cross::find(c.id);
            if (!WS) {
                o.fbs.erase(c.id);
                continue;
            }
            if (c.id != o.anchor && !cellBox(o, mon, c).intersection(SCREEN).size().x)
                continue;

            auto& fb = o.fbs[c.id];
            if (!fb)
                fb = g_pHyprRenderer->createFB("hyprcross overview");
            if (fb->m_size != mon->m_pixelSize)
                fb->alloc(mon->m_pixelSize.x, mon->m_pixelSize.y, mon->m_output->state->state().drmFormat);

            CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
            if (!g_pHyprRenderer->beginFullFakeRender(mon, fakeDamage, fb))
                continue;

            g_pHyprRenderer->m_renderPass.add(makeUnique<CClearPassElement>(CClearPassElement::SClearData{CHyprColor{0, 0, 0, 1}}));

            const bool VISIBLE = WS->m_visible;
            mon->m_activeWorkspace = WS;
            WS->m_visible          = true;
            WS->m_alpha->setValueAndWarp(1.F);

            g_pHyprRenderer->renderWorkspace(mon, WS, NOW, CBox{{}, mon->m_pixelSize});
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
        if (g_pSessionLockManager->isSessionLocked()) {
            cross::overview::teardown();
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

        renderThumbs(mon);

        cross::setTopLayerAlpha(mon, o.topAlpha * (1.F - o.progress->value()), true);
        mon->m_forceFullFrames = std::max(mon->m_forceFullFrames, 1);
    }

    void onStage(eRenderStage stage) {
        if (stage != RENDER_POST_WINDOWS || !g_ov || g_renderingThumbs)
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
            const auto FB = o.fbs.find(c.id);
            if (FB == o.fbs.end() || !FB->second->isAllocated())
                return;

            const bool  SELECTED = c.id == o.selected;
            const float RESTA    = SELECTED ? 1.F : UNSELECTED_ALPHA;
            CBox        box      = cellBox(o, MON, c);
            float       a        = RESTA * P;
            double      round    = *PROUNDING;
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

            if (SELECTED) {
                CBorderPassElement::SBorderData border;
                border.box = toPixels(box);
                if (const auto GRAD = dynamic_cast<Config::CGradientValueData*>(PBORDER.ptr()))
                    border.grad1 = *GRAD;
                else
                    border.grad1 = Config::CGradientValueData{CHyprColor{1, 1, 1, 1}};
                border.a          = P;
                border.round      = std::round(round * SCALE);
                border.borderSize = std::round(BORDER_PX * SCALE);
                g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));
            }
        };

        // the selection over its neighbours, the anchor over everything while it zooms
        for (auto const& c : o.cells)
            if (c.id != o.selected && c.id != o.anchor)
                drawCell(c);
        for (auto const& c : o.cells)
            if (c.id == o.selected && c.id != o.anchor)
                drawCell(c);
        if (const auto ANCHOR = cellFor(o, o.anchor))
            drawCell(*ANCHOR);
    }

    void callPick(WORKSPACEID id) {
        const auto MGR = Config::Lua::mgr().lock();
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
        o.workspaceListener = ev.workspace.active.listen([](PHLWORKSPACE ws) {
            // something other than a pick switched workspaces under us
            if (g_ov && !g_ov->closing && ws && ws->m_monitor == g_ov->monitor && ws != g_ov->startedOn)
                cross::overview::teardown();
        });
        o.monitorListener   = ev.monitor.removed.listen([](PHLMONITOR mon) {
            if (g_ov && g_ov->monitor == mon)
                cross::overview::teardown();
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

        for (auto const& c : o.cells) {
            const auto WS = cross::find(c.id);
            if (!WS)
                continue;
            if (MON && WS == MON->m_activeWorkspace) {
                WS->m_forceRendering = false;
                WS->m_visible        = true;
                WS->m_alpha->setValueAndWarp(1.F);
                WS->m_renderOffset->setValueAndWarp(Vector2D{});
            } else
                cross::hide(WS);
        }

        if (const auto GL = g_pHyprRenderer->glBackend().lock())
            GL->makeEGLCurrent();
        o.fbs.clear();

        if (MON) {
            if (MON->m_activeWorkspace)
                cross::setTopLayerAlpha(MON, cross::topLayersHidden(MON->m_activeWorkspace) ? 0.F : 1.F, true);
            g_pHyprRenderer->damageMonitor(MON);
        }

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
        o.selected  = SELECTED;
        o.anchor    = MON->m_activeWorkspace->m_id;
        o.topAlpha  = cross::topLayersHidden(MON->m_activeWorkspace) ? 0.F : 1.F;

        if (!lua_isnoneornil(L, 3)) {
            lua_pushvalue(L, 3);
            o.pickRef = luaL_ref(L, LUA_REGISTRYINDEX);
        }

        const auto CONFIG = Config::animationTree()->getAnimationPropertyConfig("workspaces");
        Animation::mgr()->createAnimation(0.F, o.progress, CONFIG, AVARDAMAGE_NONE);
        Animation::mgr()->createAnimation(Vector2D{}, o.offset, CONFIG, AVARDAMAGE_NONE);
        o.progress->setUpdateCallback([](auto) { damage(); });
        o.offset->setUpdateCallback([](auto) { damage(); });

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

    int luaSelect(lua_State* L) {
        const auto CELLS = parseCells(L, 1);
        if (!CELLS)
            return luaL_error(L, "cross.overview_select: cells must be a list of {id, x, y}");
        const auto SELECTED = luaL_checkinteger(L, 2);
        if (!g_ov || g_ov->closing)
            return 0;

        auto& o = *g_ov;

        // Start the cross where the old selection was, then slide it to the new one.
        Vector2D from;
        bool     found = false;
        for (auto const& c : *CELLS) {
            if (c.id == o.selected) {
                from  = {c.x, c.y};
                found = true;
                break;
            }
        }
        if (!found) {
            for (auto const& c : *CELLS) {
                if (column(c.id) == column(o.selected) && c.y == 0) {
                    from = {c.x, 0.0};
                    break;
                }
            }
        }

        o.cells    = *CELLS;
        o.selected = SELECTED;
        o.offset->setValueAndWarp(from + o.offset->value());
        *o.offset = Vector2D{};
        damage();
        return 0;
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
