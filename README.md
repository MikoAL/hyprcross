# hyprcross

PS3/PS4-style "cross" workspaces for Hyprland (Lua config, 0.56.2).

Columns run left/right; each column has slots stacked under it, so two windows
that both want the whole screen can live in the same column. Workspace id is
`10 * column + slot` (column 1 = 10, 11, 12...).

- `cross.lua`: the grid, keybinds, slot memory and gestures. Symlinked to
  `~/.config/hypr/cross.lua` and loaded with `require("cross")` from `hyprland.lua`.
- `src/`: the `hyprcross.so` plugin. `main.cpp` lets 3-finger swipes drag the
  screen with your fingers between any two workspaces, rubber-bands past an edge,
  and does the little bump when a keybind hits an edge. `overview.cpp` draws the
  pinch overview: live thumbnails of every column and slot on a grid, with a
  springy highlight. It
  only moves pixels; which workspace is where is decided in `cross.lua`.

## Keys

| Keys | Does |
|---|---|
| Super + Ctrl + left/right | prev/next column, on the slot you last left it on |
| Super + Ctrl + up/down | slot above/below (one empty slot past the last at most) |
| Super + Ctrl + Shift + arrows | the same, carrying the focused window |
| Super + 1-0 / Super + Shift + 1-0 | top of a column / send the window there |
| Super + scroll | prev/next column |
| 3-finger swipe | drag between columns and slots |
| 3-finger pinch in | overview: the screen shrinks into a grid of every column and slot |

While the overview is up:

| Input | Does |
|---|---|
| 3-finger swipe / 2-finger scroll | step the highlight a column or slot per stretch of finger travel |
| Arrow keys / mouse wheel | step the highlight (hold an arrow to keep going) |
| Enter / spread 3 fingers | go to the highlighted workspace |
| Click a thumbnail | go there |
| Escape | back to where you were |

Nothing else gets keyboard, scroll or click input while it's up.

## Build

```sh
make install   # -> ~/.local/lib/hyprland/hyprcross.so, loaded by cross.lua
make reload    # rebuild, install and swap it into the running Hyprland
```

The plugin only loads into the exact Hyprland it was built against. After a
Hyprland update, run `make reload`. Until you do, `cross.lua`
falls back to switching on release with no rubber band, and the pinch does nothing.

## Tuning

- `src/main.cpp`: `RUBBER_LIMIT` / `RUBBER_COEFF` (how far and how stiffly an edge
  stretches), `FLICK_SPEED` / `COMMIT_RATIO` (when a swipe commits on release).
- `src/overview.cpp`: `THUMB_FRAC` / `SELECTED_SCALE` / `GAP_X` / `GAP_Y` (cross
  layout), `BACKDROP_DIM` / `UNSELECTED_ALPHA` / `BORDER_PX` (look),
  `PINCH_RANGE` / `OPEN_RATIO` / `FLICK_SPEED` (when a pinch opens it),
  `SWIPE_DIST` / `SCROLL_DIST` (finger travel per step), `CAMERA_LAG_MS` (how far the view trails).
- `cross.lua`: `BUMP_PX` / `BUMP_MS` (the keyboard edge bump).
- The settle animation is the `workspaces` animation in `hyprland.lua` (the `wsSnap` spring);
  the overview's zoom and slide use it too.

`make test` checks the overview's grid layout in `cross.lua` against a stubbed `hl`.
