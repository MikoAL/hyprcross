// Bits main.cpp (swipes) and overview.cpp both need.
#pragma once

#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>

inline HANDLE PHANDLE = nullptr;

namespace cross {
    // A live workspace by id, or nullptr. Inert ones (being destroyed) have
    // their animated values freed, so they count as missing.
    inline PHLWORKSPACE find(WORKSPACEID id) {
        if (id == WORKSPACE_INVALID)
            return nullptr;
        const auto WS = State::workspaceState()->query().id(id).run();
        if (!WS || WS->inert() || !WS->m_alpha || !WS->m_renderOffset)
            return nullptr;
        return WS;
    }

    // Layers above fullscreen windows (bars) are hidden on a fullscreen workspace;
    // show them while two workspaces are on screen, as Hyprland's own swipe does.
    inline void setTopLayerAlpha(const PHLMONITOR& mon, float a) {
        for (auto const& ref : mon->m_layerSurfaceLayers[2])
            if (const auto LS = ref.lock())
                *LS->alpha()[Desktop::View::LS_ALPHA_FADE] = a;
    }

    // Whether Hyprland hides the top layers on this workspace (a real fullscreen window).
    inline bool topLayersHidden(const PHLWORKSPACE& ws) {
        const auto FSWINDOW = Fullscreen::controller()->getFullscreenWindow(ws);
        const auto MODE     = FSWINDOW ? Fullscreen::controller()->getFullscreenModes(FSWINDOW).internal : Fullscreen::FSMODE_NONE;
        return MODE == Fullscreen::FSMODE_FULLSCREEN &&
            (!FSWINDOW || !Fullscreen::controller()->layoutManagedFS(FSWINDOW) ||
             (ws->m_space && ws->m_space->algorithm() && Fullscreen::controller()->hasFullscreen(ws, true)));
    }

    inline void applyFullscreenAlpha(const PHLMONITOR& mon, const PHLWORKSPACE& ws) {
        setTopLayerAlpha(mon, topLayersHidden(ws) ? 0.F : 1.F);
    }

    inline void hide(const PHLWORKSPACE& ws) {
        if (!ws)
            return;
        ws->m_forceRendering = false;
        ws->m_alpha->setValueAndWarp(0.F);
    }

    // main.cpp: a finger-tracked swipe is in progress
    bool swiping();
}
