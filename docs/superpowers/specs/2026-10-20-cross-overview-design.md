# Cross overview (3-finger pinch) — design

Status: draft, awaiting review (2026-10-20)

## Goal

A hyprexpo-style overview of workspaces, laid out like the PS3 XMB cross instead
of a grid, opened by pinching three fingers. It is for seeing where things are
and jumping to any column/slot quickly.

## What you'll see and do

**Opening.** Pinch three fingers together. The screen shrinks with your fingers
into its place in the cross; let go past halfway (or pinch quickly) and it
settles into the overview, otherwise it springs back to full screen.

**The cross.**

```
                 [3.0]
   [1]   [2]   ( 3.1 )   [4]   [5]  ...
                 [3.2]
                 [3.3]   <- one empty slot after the last used one
```

- A horizontal row of columns 1–10, each a live mini picture of the slot that
  column was last left on (the same slot Super+Ctrl+left/right takes you to).
- The selected column also shows its slots as a vertical line through it: slots
  before the selected one above, slots after it below, the selected one at the
  crossing point, like the XMB.
- The selected workspace is drawn larger with a highlight border; the rest are a
  little dimmer. Behind everything is your wallpaper, darkened.
- Moving the selection slides the whole cross so the selection stays in the
  middle of the screen, with the same springy motion as workspace switches.
- Thumbnails are live: video keeps playing, terminals keep updating.

**Moving around while it's open.**

| Input | Does |
|---|---|
| 3-finger swipe left/right | select the next/previous column (one step per swipe) |
| 3-finger swipe up/down | select the slot below/above in that column |
| Arrow keys | same as swipes |
| Enter, or 3-finger pinch out | go to the selected workspace |
| Click/tap a thumbnail | go straight to that workspace |
| Escape | close and stay where you were |

Going to a workspace zooms that thumbnail back up to full screen.

Swipes keep their direction convention from normal use ("the content follows the
fingers"): fingers moving left selects the column to the right.

**Not included (for now):** dragging windows between workspaces, text labels
under thumbnails, multiple monitors at once (it opens on the focused monitor).

**Without the plugin loaded** (e.g. right after a Hyprland update, before
`make reload`), the pinch does nothing.

## Technical design

Same split as the swipes: `cross.lua` owns policy (which workspaces are in the
cross, what a selection step means, what picking does); the plugin only draws and
animates.

### cross.lua

- `hl.gesture({ fingers = 3, direction = "pinchin", ... })` with start/update/finish
  callbacks, and a `pinchout` gesture that confirms while open.
- `crossCells(col, row)` builds the cell list for a selection:
  - main row: for each column 1–10, `columnTarget(c)` (reuses `lastRow`), at
    grid position `(c - col, 0)`;
  - the selected column: rows 0..last occupied + 1 (capped at `MAX_ROWS - 1`), at
    `(0, r - row)`; the selected column's main-row cell is replaced by these.
- Overview state `ov = { open, col, row }`. While `ov.open`, the existing swipe
  callbacks step the selection on finish (same 50-unit threshold as the no-plugin
  fallback) instead of calling `swipe_begin`; stepping uses `columnTarget` /
  `slotTarget` so the rules match normal navigation (no going past the ends).
- Keys: a submap `cross_overview` holding arrows / Return / Escape, entered on
  open and left on close. If the Lua API has no submap support, the plugin
  swallows those keys instead (see below) and calls back into Lua.
- Picking: `p.overview_close(id)` then `go(col, row)`.

### Plugin (src/main.cpp)

New Lua functions on `hl.plugin.cross`:

| Function | Does |
|---|---|
| `overview_begin(cells, selected, on_pick)` | `cells` = list of `{id, x, y}` grid positions; starts the overview at zoom 0; returns false if it can't (locked, swipe running, no monitor) |
| `overview_update(scale, time_ms)` | pinch scale from the gesture → zoom progress |
| `overview_end(cancelled)` | spring to open (1) or closed (0), same flick/threshold logic as swipes |
| `overview_select(cells, selected)` | new cells/selection; springs the cross offset so `selected` is centred |
| `overview_close(id)` | zoom `id`'s thumbnail back to full screen, then tear down |
| `overview_active() -> bool` | overview is up |

`on_pick(id)` is a Lua function the plugin calls (via a registry ref) when a
thumbnail is clicked or a swallowed key is pressed.

**Rendering.** Modelled on hyprexpo:
- One `CFramebuffer` per visible cell, sized to the thumbnail's on-screen pixels.
  Each frame, render the workspace into it with `CHyprRenderer::renderWorkspace`
  (protected; reached the way hyprexpo does).
- A custom `IPassElement` replaces the monitor's normal content while the
  overview is up: darkened background layer, then thumbnails as textures with
  rounded corners, selection border and alpha.
- Only cells intersecting the monitor are rendered; off-screen cells keep no
  framebuffer.
- The whole monitor is damaged every frame while open/animating.

**Geometry.** Monitor size `W×H`. Thumbnail `0.2W × 0.2H`, selected `×1.3`;
spacing `0.04W` horizontally, `0.04H` vertically. Grid position `(x, y)` maps to
the centre `(W/2 + x·step_x, H/2 + y·step_y)` plus the animated cross offset.
These live as tunable constants next to the swipe ones.

**Zoom.** One animated value `progress ∈ [0,1]`.
- Opening: `progress = clamp((1 − scale) / PINCH_RANGE, 0, 1)` with
  `PINCH_RANGE = 0.5`; release commits on `progress ≥ 0.5` or flick, using the
  same velocity tracking as swipes.
- The active workspace's box lerps from the full monitor box to its cell box;
  every other thumbnail and the backdrop fade/scale in with `progress`.
- Closing on pick is the same lerp in reverse toward the picked cell; the
  workspace switch (`go`) happens at the start so the real workspace is
  already active underneath when the pass element is removed.

**Input while open.**
- Mouse/touchpad button: hook the mouse button event, cancel it, hit-test the
  cells, call `on_pick(id)`.
- Keys (only if no Lua submap): hook key press, cancel arrows/Return/Escape and
  forward them to Lua.
- Pointer motion does nothing (no hover selection).

**Edge cases.**
- Session lock, the active workspace changing by other means (keybind, IPC), or
  the monitor disappearing → close immediately without animation.
- A swipe already running → `overview_begin` returns false.
- Windows opening/closing while open: thumbnails stay live; the cell layout is
  only recomputed on the next selection step.
- `PLUGIN_EXIT` while open: drop the pass element, free framebuffers, unhook.

### Risks to check first

1. Whether the Lua gesture wrapper passes the pinch `scale` to Lua callbacks.
   If not, the plugin registers its own pinch gesture handler for the pinch and
   Lua only receives open/close notifications.
2. `renderWorkspace` into an offscreen framebuffer on 0.56.2 (hyprexpo is the
   reference; check its current source for the exact call sequence).
3. Whether the Lua config API has submaps.

### Spike findings (2026-10-20)

1. Pinch scale reaches Lua: Hyprland's `CLuaFunctionGesture` pushes `scale`,
   `delta`, `rotation`, `fingers` and `time_ms` for pinch start/update, and
   `cancelled` on end. No plugin-side gesture needed.
2. Submaps exist: `hl.define_submap(name, fn)` with `hl.bind` calls inside, and
   `hl.dsp.submap(name)` / `hl.dsp.submap("reset")` to enter/leave. Keys stay in Lua.
3. Rendering on 0.56.2: the renderer is now `IHyprRenderer` with GL/VK
   backends. Thumbnails: `g_pHyprRenderer->createFB()`, `fb->alloc(...)`,
   `beginFullFakeRender(mon, damage, fb)`, protected `renderWorkspace(...)`,
   `endRender()`; `sendFrameEventsToWorkspace` keeps clients on hidden
   workspaces drawing. Drawing uses the stock pass elements (`CRectPassElement`,
   `CTexPassElement`, `CBorderPassElement`) added from the
   `Event::bus()->m_events.render.stage` callback at `RENDER_POST_WINDOWS`,
   instead of hooking `renderWorkspace` the way hyprexpo does, so it can't clash
   with the hyprexpo the user already has loaded. `m_forceFullFrames` keeps the
   whole screen redrawn while open.

## Testing

There is no automated test suite; the plugin is checked by building
(`make reload`) and using it in the running Hyprland, with `grim` screenshots of
each state (closed, mid-pinch, open, after a selection step, after picking).
The cell-list logic in `cross.lua` is plain Lua and gets a small script that
runs it against a stubbed `hl` table to check the cross shape for a few slot
layouts.

## Docs

README: add the pinch and the overview keys to the Keys table and a line under
Tuning for the overview constants.
