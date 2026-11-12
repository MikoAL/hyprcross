# hyprcross

PS3/PS4-style "cross" workspaces for Hyprland, with finger-tracking 3-finger
swipes and a pinch overview.

> Made with [Claude Code](https://claude.com/claude-code), Anthropic's AI coding
> assistant: every commit is co-authored by Claude. The plugin runs inside your
> compositor, so read the code before you trust it.

Columns run left/right; each column has slots stacked under it, so two windows
that both want the whole screen can live in the same column. Workspace id is
`10 * column + slot` (column 1 = 10, 11, 12...).

It comes in two parts:

- `cross.lua`: the grid, keybinds, slot memory and gestures. A module for
  Hyprland's Lua config, loaded with `require("cross")` from `hyprland.lua`.
- `hyprcross.so`: a plugin. It makes 3-finger swipes drag the screen with your
  fingers between any two workspaces, rubber-bands past an edge, does the little
  bump when a keybind hits an edge, and draws the pinch overview: live thumbnails
  of every column and slot, with a springy highlight. It only moves pixels;
  which workspace is where is decided in `cross.lua`.

`cross.lua` works without the plugin: swipes then switch on release with no
rubber band, and the pinch does nothing.

## Status

Early and lightly tested. It has only been used on one laptop: a single
built-in 2560x1600 display at 165 Hz with scale 1.6, and its touchpad, on Arch
Linux with Hyprland 0.57.2. Untested:

- **multiple monitors**: no idea how the swipes, the overview or the workspace
  grid behave with more than one display
- **other resolutions and scales**: no idea how they affect the overview's
  layout, the thumbnails or how far your fingers travel per step (the tuning
  values were picked by feel on that one screen)
- other GPUs, refresh rates and touchpads, and mouse-only setups
- installing through hyprpm, and other distros

Bug reports (and reports that it works) are welcome in the issues, and so are
pull requests, especially for setups unlike the one above.

## Requirements

- Hyprland **0.57** with a **Lua config** (`~/.config/hypr/hyprland.lua`).
  `hyprland.conf` (hyprlang) configs aren't supported. Tested on 0.57.2.
- A touchpad, for the swipes and pinches. Everything also has keys.
- To build the plugin: `make`, `pkg-config`, a C++26 compiler (the GCC that
  builds Hyprland), and the development files for Hyprland, Lua 5.5, pixman and
  libdrm. On Arch: `pacman -S --needed base-devel hyprland lua pixman libdrm`.

## Install

### 1. The plugin

Pick **one** of these two ways. If you use both, the plugin gets loaded twice.

**With hyprpm** (it rebuilds the plugin for you after Hyprland updates):

```sh
hyprpm update
hyprpm add https://github.com/MikoAL/hyprcross
hyprpm enable hyprcross
```

Then load hyprpm's plugins at startup, in `hyprland.lua`:

```lua
hl.on("hyprland.start", function() hl.exec_cmd("hyprpm reload") end)
```

After each Hyprland update, run `hyprpm update`.

**By hand:**

```sh
git clone https://github.com/MikoAL/hyprcross
cd hyprcross
make install   # -> ~/.local/lib/hyprland/hyprcross.so
```

`cross.lua` loads `~/.local/lib/hyprland/hyprcross.so` on its own when the file
exists. The plugin only loads into the exact Hyprland it was built against.
If `cross.lua` is a symlink into this checkout (see below), it runs
`scripts/update.sh` at login. That script refreshes hyprpm's headers, rebuilds
and loads the fresh plugin (log: `~/.local/state/hyprcross-update.log`). To do
the same yourself, run `make reload`.

### 2. cross.lua

Put `cross.lua` next to your `hyprland.lua`. A symlink into a clone keeps it up
to date with `git pull`:

```sh
ln -s ~/path/to/hyprcross/cross.lua ~/.config/hypr/cross.lua
```

(With hyprpm and no clone, download `cross.lua` from this repo instead.) Then, in `hyprland.lua`:

```lua
local cross = require("cross")
```

`cross.lua` sets up these binds itself, so remove any binds for the same keys
from your config, such as the default config's `SUPER + 1-0` workspace binds and
any 3-finger workspace gesture:

- `SUPER + 1-0`, `SUPER + SHIFT + 1-0`
- `SUPER + CTRL + arrows`, `SUPER + CTRL + SHIFT + arrows`
- `SUPER + Tab`, `SUPER + mouse wheel`
- 3-finger horizontal/vertical swipes (with and without `SUPER`) and 3-finger pinches

Because of the new workspace ids, rules or autostarts that name a workspace
should use `cross.ws(column, slot)`:

```lua
hl.exec_cmd("firefox", { workspace = cross.ws(2) .. " silent" })  -- column 2, top slot
```

To open or close the overview from your own binds or gestures, use
`cross.overviewOpen()` and `cross.overviewClose()`.

## Keys

| Keys | Does |
|---|---|
| Super + Ctrl + left/right | prev/next column, on the slot you last left it on |
| Super + Ctrl + up/down | slot above/below (one empty slot past the last at most) |
| Super + Ctrl + Shift + arrows | the same, carrying the focused window |
| Super + 1-0 / Super + Shift + 1-0 | a column, on the slot you last left it on / send the window there |
| Super + scroll | prev/next column |
| 3-finger swipe | drag between columns and slots |
| Super + 3-finger swipe | the same, carrying the focused window: it lifts (shrinks a little) and stays under your fingers while the workspaces slide behind it |
| 3-finger pinch in / Super + Tab | overview: the screen shrinks into a grid of every column and slot |

While the overview is up:

| Input | Does |
|---|---|
| 3-finger swipe / 2-finger scroll | step the highlight, like a held arrow key: one step, then more as you keep going |
| Arrow keys / mouse wheel | step the highlight (hold an arrow to keep going; Shift + wheel goes sideways) |
| Enter / spread 3 fingers | go to the highlighted workspace |
| Click a thumbnail | go there |
| Escape / Super + Tab | back to where you were |
| Tab | flip between cross and free mode |

It has two modes. In **cross** mode (the default) the highlight stays in the
middle of the screen and the grid slides under it, like the PS3's XMB, except
that every column shows all its slots. In **free** mode the highlight moves over
the grid and the view follows. `OVERVIEW_MODE` in `cross.lua` picks the one it
opens in; Tab flips it until the next config reload.

While your fingers are down the highlight leans toward where it's going (in
cross mode the grid does the leaning), and stiffly at the edge of the grid.
Nothing else gets keyboard, scroll or click input while it's up.

## Tuning

- `cross.lua`: `COLUMNS` / `MAX_ROWS` (grid size), `SPRING` (the spring every
  workspace switch and the overview settle with; `cross.lua` sets the
  `workspaces` animations to it), `BUMP_PX` / `BUMP_MS` (the keyboard edge bump),
  `OVERVIEW_MODE` (`"cross"` or `"free"`), `AUTO_REBUILD` (the login rebuild).
- `src/main.cpp`: `LIFT_SCALE` (how small a carried window gets), `RUBBER_LIMIT` /
  `RUBBER_COEFF` (how far and how stiffly an edge stretches), `FLICK_SPEED` /
  `COMMIT_RATIO` (when a swipe commits on release).
- `src/overview.cpp`: `THUMB_FRAC` / `SELECTED_SCALE` / `GAP_X` / `GAP_Y` (cross
  layout), `BACKDROP_DIM` / `UNSELECTED_ALPHA` / `BORDER_PX` (look),
  `THUMB_RES` / `FOCUS_HZ` / `BACKGROUND_HZ` (thumbnail resolution and refresh
  rates: the frame-rate knobs), `PINCH_RANGE` / `OPEN_RATIO` / `FLICK_SPEED`
  (when a pinch opens it), `STEP_DIST` / `STEP_DIST_Y` (finger travel per
  sideways / up-down step, swipes and scrolling alike), `REPEAT_DELAY` (extra
  travel before a drag's second step), `LEAN_MAX` / `EDGE_RESIST` (the lean),
  `CAMERA_LAG_MS` (how far the view trails in free mode).

Changes in `src/` need a rebuild: `make reload` swaps the new build into the
running Hyprland.

## Uninstall

Remove the `require("cross")` line (and any `cross.*` calls) from `hyprland.lua`,
delete `~/.config/hypr/cross.lua`, then `hyprpm remove hyprcross` or
`rm ~/.local/lib/hyprland/hyprcross.so`.

## Development

```sh
make           # build hyprcross.so here
make reload    # build, install and swap it into the running Hyprland
make test      # Lua tests: the overview's grid layout and the gesture/bind routing in cross.lua, against a stubbed `hl`
```

A plugin crash takes Hyprland down with it, so try plugin changes in a nested
Hyprland first.

## License

[MIT](LICENSE)
