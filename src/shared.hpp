// Bits main.cpp (swipes) and overview.cpp both need.
#pragma once

#include <hyprland/src/state/WorkspaceState.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>

inline HANDLE PHANDLE = nullptr;

// Hyprland 0.57 made workspace ids a variant (numbered or special) and dropped
// WORKSPACEID / WORKSPACE_INVALID. The cross grid only ever uses numbered
// workspaces, so keep them as plain integers here and convert at the edges.
using WORKSPACEID = int64_t;
inline constexpr WORKSPACEID WORKSPACE_INVALID = -1;

namespace cross {
    // The workspace's number, or WORKSPACE_INVALID for special/named ones.
    inline WORKSPACEID idOf(const PHLWORKSPACE& ws) {
        if (!ws)
            return WORKSPACE_INVALID;
        const auto ID = ws->id();
        if (const auto* N = std::get_if<Workspace::SWorkspaceNumberedID>(&ID))
            return N->value;
        return WORKSPACE_INVALID;
    }

    inline bool validId(WORKSPACEID id) {
        return id > 0 && id <= sc<WORKSPACEID>(UINT32_MAX);
    }

    // Creates the numbered workspace on a monitor (empty, like a fresh switch).
    inline PHLWORKSPACE create(WORKSPACEID id, const PHLMONITOR& mon) {
        if (!validId(id) || !mon)
            return nullptr;
        return State::workspaceState()->createNumbered(Workspace::SWorkspaceNumberedID(sc<Workspace::WorkspaceIDContainer>(id)), mon);
    }

    // A workspace being destroyed drops out of Hyprland's workspace state
    // (0.57 has no inert() flag) and may have its animated values freed.
    inline bool live(const PHLWORKSPACE& ws) {
        if (!ws || !ws->m_alpha || !ws->m_renderOffset)
            return false;
        const auto ID = ws->id();
        if (const auto* N = std::get_if<Workspace::SWorkspaceNumberedID>(&ID))
            return State::workspaceState()->query().numbered(*N).run() == ws;
        return true;
    }

    // A live workspace by id, or nullptr.
    inline PHLWORKSPACE find(WORKSPACEID id) {
        if (!validId(id))
            return nullptr;
        const auto WS = State::workspaceState()->query().numbered(Workspace::SWorkspaceNumberedID(sc<Workspace::WorkspaceIDContainer>(id))).run();
        return live(WS) ? WS : nullptr;
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
             (ws->space() && ws->space()->algorithm() && Fullscreen::controller()->hasFullscreen(ws, true)));
    }

    inline void applyFullscreenAlpha(const PHLMONITOR& mon, const PHLWORKSPACE& ws) {
        setTopLayerAlpha(mon, topLayersHidden(ws) ? 0.F : 1.F);
    }

    inline void hide(const PHLWORKSPACE& ws) {
        if (!live(ws))
            return;
        ws->m_forceRendering = false;
        ws->m_alpha->setValueAndWarp(0.F);
    }

    // main.cpp: a finger-tracked swipe is in progress
    bool swiping();
}
