# Cross Overview Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A 3-finger pinch opens a finger-tracked, XMB-cross-shaped overview of live workspace thumbnails, navigable by swipes, arrows and clicks.

**Architecture:** `cross.lua` computes which workspaces sit where in the cross and owns selection/picking; `src/main.cpp` renders them as offscreen-framebuffer thumbnails in a custom render pass element and animates zoom and cross offset. They talk through new `hl.plugin.cross.overview_*` Lua functions plus one Lua callback (`on_pick`).

**Tech Stack:** C++ Hyprland 0.56.2 plugin API, Lua 5.5 config API, GLES via Hyprland's `CHyprOpenGLImpl`.

**Spec:** `docs/superpowers/specs/2026-10-20-cross-overview-design.md`

## Global Constraints

- Builds against the installed Hyprland 0.56.2 headers with the existing `Makefile` flags; no new pkg-config deps.
- Policy stays in Lua, pixels in C++ (the existing file headers say so; keep it true).
- Without the plugin loaded, the pinch does nothing and nothing else changes.
- Geometry constants (in `src/main.cpp`, next to `RUBBER_*`): `THUMB_FRAC = 0.2`, `SELECTED_SCALE = 1.3`, `GAP_X = 0.04`, `GAP_Y = 0.04`, `PINCH_RANGE = 0.5`, `OPEN_RATIO = 0.5`, `BACKDROP_DIM = 0.5`, `UNSELECTED_ALPHA = 0.75`, `BORDER_PX = 3` (colour = `general:col.active_border`, corners = `decoration:rounding`).
- Overview only on the focused monitor.
- Work on a branch `cross-overview`, not `master`.

## Review Focus

1. Pinching while a swipe is still settling (or swiping while the overview zooms) → the second gesture is ignored, nothing gets stuck. Pinned in Task 4.
2. Selected column whose slots are all empty, or a remembered slot whose workspace has been destroyed → its cell shows just the darkened wallpaper, no crash. Pinned in Task 2 (layout) and Task 3 (render of a missing workspace).
3. Workspace changed by keybind/IPC while open (Super+3) → overview closes instantly, no leftover pass element. Pinned in Task 6.
4. Fullscreen window in a thumbnail → thumbnail shows it; the bar reappears correctly after closing. Pinned in Task 3.
5. Pinch-out (or Enter/Escape) while the overview is closed → does nothing; Escape and arrows keep working normally outside the overview. Pinned in Task 5.

---

### Task 1: Spike the three unknowns

Throwaway; nothing from it ships except notes appended to the spec's "Risks" section.

**Files:** temporary edits to `~/.config/hypr/hyprland.lua` / a scratch plugin build; final notes in the spec.

- [ ] **Step 1:** Add a temporary `hl.gesture({ fingers = 3, direction = "pinchin", action = { start = f, update = f, finish = f } })` whose `f(e)` logs every field of `e` (`hl.log` or `print` to the Hyprland log). Pinch on the touchpad; record whether `e.scale` (or equivalent) is present and how it moves.
  Expected: a log line per update. If no scale field: the plugin registers its own pinch gesture (subclass `ITrackpadGesture`, add via `g_pTrackpadGestures`) and Lua gets `start`/`finish` notifications only; record that decision.
- [ ] **Step 2:** Check for submap support in the Lua API (`hl.submap` / `submap` key on `hl.bind`) in the Hyprland Lua docs/wiki for 0.56. Record yes/no; no → keys go through the plugin key hook (Task 5).
- [ ] **Step 3:** Fetch hyprexpo's source for the 0.56 tag (github.com/hyprwm/hyprland-plugins, `hyprexpo/overview.cpp`) and note the exact sequence it uses to render a workspace into a `CFramebuffer` (how it reaches the protected `renderWorkspace`, `beginRender`/`endRender` calls, how it inserts its pass element). Paste the call sequence into the spec.
- [ ] **Step 4:** Remove the temporary gesture; commit the spec notes.

```bash
git switch -c cross-overview
git add docs/superpowers/specs/2026-10-20-cross-overview-design.md docs/superpowers/plans/2026-10-20-cross-overview.md
git commit -m "docs: cross overview spec, plan and spike notes"
```

### Task 2: Cross layout in Lua (`crossCells`) with a test harness

**Files:**
- Modify: `cross.lua` (add `crossCells`, export it and `lastRow` on `M`)
- Create: `tests/stub_hl.lua`, `tests/cells_test.lua`
- Modify: `Makefile` (add `test` target: `lua5.5 tests/cells_test.lua`)

**Interfaces:**
- Produces: `M.crossCells(col, row) -> { {id=int, x=int, y=int}, ... }`. One cell per column 1–10 at `(c - col, 0)` using `columnTarget(c)`, except the selected column which contributes rows `0 .. lastOccupied+1` (capped at `MAX_ROWS-1`; at least `row`) at `(0, r - row)`. Order: main row left→right, then the selected column top→bottom.
- Produces: `tests/stub_hl.lua` returns a fake `hl` with recording no-ops for `bind`, `gesture`, `on`, `workspace_rule`, `timer`, `animation`, `dispatch`, `dsp`, `plugin`, and a settable table of workspaces driving `get_workspace(id)` / `get_active_workspace()`.

- [ ] **Step 1: Write the failing tests** in `tests/cells_test.lua` (plain `assert`, loads `cross.lua` via `package.path` with `_G.hl = require("stub_hl")`):
  - `selected column 3, row 1, slots 30,31,32 occupied` → cells for column 3 are ids `30,31,32,33` at y `-1,0,1,2`; column 1 cell is `{id=10,x=-2,y=0}`; column 10 is `{id=100,x=7,y=0}`; total cells = 9 + 4 = 13.
  - `lastRow[5] = 2` and `52` occupied → column 5's cell id is `52`; `lastRow[5] = 2` and `52` empty → `50`.
  - all of column 4 empty, selected `(4, 0)` → column 4 contributes exactly `{40 at y=0, 41 at y=1}`.
  - `MAX_ROWS` cap: slots 30..39 occupied → column 3 stops at `39`.
- [ ] **Step 2:** Run `make test`. Expected: FAIL (`crossCells` is nil).
- [ ] **Step 3:** Implement `crossCells` in `cross.lua` reusing `ws`, `occupied`, `columnTarget`; make the top-level `io.open(PLUGIN)` tolerate the stub (it already does: no file → `havePlugin = false`).
- [ ] **Step 4:** Run `make test`. Expected: `ok` line, exit 0.
- [ ] **Step 5:** Commit `cross.lua tests/ Makefile` — "cross.lua: compute the overview cross layout".

### Task 3: Plugin renders the overview (static, no zoom yet)

**Files:**
- Create: `src/overview.hpp`, `src/overview.cpp` (overview state, pass element, thumbnail framebuffers, geometry)
- Modify: `src/main.cpp` (register Lua functions, cleanup in `PLUGIN_EXIT`), `Makefile` (compile both `.cpp` files)

**Interfaces:**
- Consumes: cell tables from Task 2.
- Produces (Lua, on `hl.plugin.cross`):
  - `overview_begin(cells, selected:int, on_pick:function) -> bool` — false if locked, a swipe is active (`g_swipe`), or no focused monitor. Stores a registry ref to `on_pick`.
  - `overview_open()` — sets progress straight to 1 (used for testing now; zoom comes in Task 4).
  - `overview_close(id:int|nil)` — tears down immediately for now (animated in Task 4).
  - `overview_active() -> bool`.
- Produces (C++): `struct SCell { WORKSPACEID id; int x, y; }`; `CBox cellBox(const SOverview&, const SCell&)` implementing the geometry from Global Constraints; `void overviewTeardown()`.

- [ ] **Step 1:** Implement following the call sequence recorded in Task 1 Step 3: per visible cell one `CFramebuffer` sized to the cell box × monitor scale, rendered each frame with `renderWorkspace`; a pass element that draws the background layer darkened by `BACKDROP_DIM`, then each thumbnail (`UNSELECTED_ALPHA` unless selected; selected drawn `SELECTED_SCALE` larger with a `BORDER_PX` border in `col.active_border` and `decoration:rounding` corners). Missing workspace (`find(id) == nullptr`) → no framebuffer, cell draws nothing over the backdrop.
- [ ] **Step 2:** `make reload`, then:
  `hyprctl eval 'local p=hl.plugin.cross; p.overview_begin(require("cross").crossCells(3,0), 30, function() end); p.overview_open()'`
  then `grim /tmp/ov.png`. Expected: cross centred on 30, live thumbnails, darkened wallpaper. Repeat with a fullscreen window on one workspace and with an empty column selected.
- [ ] **Step 3:** `hyprctl eval 'hl.plugin.cross.overview_close()'`. Expected: normal desktop, bar visible, `overview_active()` false.
- [ ] **Step 4:** Commit — "plugin: draw the cross overview".

### Task 4: Finger-tracked pinch zoom

**Files:** Modify `src/overview.cpp`, `src/main.cpp`, `cross.lua`.

**Interfaces:**
- Produces: `overview_update(scale:number, time_ms:int)`, `overview_end(cancelled:bool)`; `overview_close(id)` now animates. Remove `overview_open` (test-only).
- Progress: `clamp((1 - scale) / PINCH_RANGE, 0, 1)`; on end, commit open if velocity of progress ≥ `0.004`/ms toward open, else if progress ≥ `OPEN_RATIO`; otherwise spring to 0 and tear down. Use a `CAnimatedVariable<float>` with the `workspaces` animation config so the settle uses the user's `wsSnap` spring.
- Active workspace box lerps monitor box → its cell box by progress; other cells and backdrop alpha scale with progress. Closing on `id` lerps that cell → monitor box; `cross.lua` calls `go()` before `overview_close(id)` so the target is active underneath.

- [ ] **Step 1:** Wire `cross.lua`: `pinchin` gesture → `overview_begin(crossCells(current()), activeId, pick)` on start, `overview_update(e.scale, e.time_ms)` on update, `overview_end(e.cancelled)` on finish (or the plugin-owned gesture from Task 1 if Lua has no scale). Keep state in `ov = { open, col, row }`.
- [ ] **Step 2:** `make reload`; pinch slowly and stop halfway, release → springs back; pinch past half → opens; quick short pinch → opens. Record a screen video (`wf-recorder`) of one slow pinch for review.
- [ ] **Step 3:** Review Focus 1: start a 3-finger swipe, release, and pinch immediately while it settles → pinch ignored or opens cleanly, no stuck offsets; swipe while the overview zooms → no workspace drag.
- [ ] **Step 4:** Commit — "Pinch zooms into the cross overview".

### Task 5: Moving the selection (swipes, arrows, Enter, Escape, pinch-out)

**Files:** Modify `cross.lua`, `src/overview.cpp`, `src/main.cpp`.

**Interfaces:**
- Produces: `overview_select(cells, selected:int)` — replaces cells, springs the cross offset so `selected` is at the centre.
- `cross.lua`: while `ov.open`, `swipeStart` skips `swipe_begin`; `swipeEnd` steps the selection when `|moved| > 50` using `columnTarget` / `slotTarget` (same direction convention as now: fingers left → next column). `pinchout` while open → pick. Arrows/Return/Escape via a submap if Task 1 found one; otherwise the plugin's key hook (Task 6) calls `on_pick`-style callbacks — define `on_key(name)` in that case and pass it as a 4th arg to `overview_begin`.
- Pick = `go(col,row)` then `overview_close(id)`; Escape = `overview_close(nil)` (zooms back into the original workspace).

- [ ] **Step 1:** Extend `tests/cells_test.lua`: a `step(ov, "x", 1)` helper test — from `(3,1)`, stepping right selects column 4's remembered slot and `crossCells(4, r)` is what gets passed (assert via the stub's recorded `overview_select` call); stepping right from column 10 leaves the selection unchanged.
- [ ] **Step 2:** `make test` → FAIL; implement; `make test` → PASS.
- [ ] **Step 3:** `make reload`; open, swipe/arrow through a few cells, Enter → lands on it with slot memory updated; Escape → back where you were. Review Focus 5: with the overview closed, pinch-out, Escape and arrows behave exactly as before.
- [ ] **Step 4:** Commit — "Navigate the cross overview".

### Task 6: Clicks, closing on outside events, cleanup, docs

**Files:** Modify `src/overview.cpp`, `src/main.cpp`, `README.md`.

- [ ] **Step 1:** Mouse button hook (via `HyprlandAPI::registerCallbackDynamic` for `mouseButton`, cancelling the event while open): hit-test cell boxes, call `on_pick(id)`; outside any cell → ignore. Key hook only if Task 1 found no submap.
- [ ] **Step 2:** Close instantly (no animation) on: session lock, `workspace` event for the overview's monitor not caused by our own pick, monitor removed. `PLUGIN_EXIT` calls `overviewTeardown()` and unregisters hooks.
- [ ] **Step 3:** Verify: click a thumbnail → goes there; open then Super+5 → overview gone, on column 5 (Review Focus 3); open then `make reload` → clean desktop; lock screen while open → no leftover after unlock.
- [ ] **Step 4:** README: add "3-finger pinch in — cross overview" and the in-overview controls to the Keys table; add the overview constants under Tuning; mention the plugin now has overview functions in the `src/main.cpp` bullet.
- [ ] **Step 5:** Commit — "Cross overview: clicks, outside events, docs".
