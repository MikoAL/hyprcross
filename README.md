# hyprcross

PS3/PS4-style "cross" workspaces for Hyprland (Lua config, 0.56.2).

Columns run left/right; each column has slots stacked under it, so two windows
that both want the whole screen can live in the same column. Workspace id is
`10 * column + slot` (column 1 = 10, 11, 12...).

- `cross.lua`: the grid, keybinds, slot memory and gestures. Symlinked to
  `~/.config/hypr/cross.lua` and loaded with `require("cross")` from `hyprland.lua`.
- `src/main.cpp`: the `hyprcross.so` plugin. It lets 3-finger swipes drag the
  screen with your fingers between any two workspaces, rubber-bands past an edge,
  and does the little bump when a keybind hits an edge. It only moves pixels; which
  workspace is next is decided in `cross.lua`.

## Keys

| Keys | Does |
|---|---|
| Super + Ctrl + left/right | prev/next column, on the slot you last left it on |
| Super + Ctrl + up/down | slot above/below (one empty slot past the last at most) |
| Super + Ctrl + Shift + arrows | the same, carrying the focused window |
| Super + 1-0 / Super + Shift + 1-0 | top of a column / send the window there |
| Super + scroll | prev/next column |
| 3-finger swipe | drag between columns and slots |

## Build

```sh
make install   # -> ~/.local/lib/hyprcross/hyprcross.so, picked up on the next `hyprctl reload`
```

The plugin only loads into the exact Hyprland it was built against. After a
Hyprland update, run `make install` again and reload. Until you do, `cross.lua`
falls back to switching on release with no rubber band.

## Tuning

- `src/main.cpp`: `RUBBER_LIMIT` / `RUBBER_COEFF` (how far and how stiffly an edge
  stretches), `FLICK_SPEED` / `COMMIT_RATIO` (when a swipe commits on release).
- `cross.lua`: `BUMP_PX` / `BUMP_MS` (the keyboard edge bump).
- The settle animation is the `workspaces` animation in `hyprland.lua` (the `wsSnap` spring).
