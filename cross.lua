-- cross.lua (symlinked to ~/.config/hypr/cross.lua)
-- PS3/PS4-style "cross" workspaces: columns run left/right along the main row,
-- and each column has slots stacked under it for windows that each want the
-- whole screen.
--
-- Workspace id = 10 * column + row, so column 1 is 10, 11, 12... Hyprland picks
-- the slide direction by comparing ids; this ordering keeps it right both ways.
--
--   Super + 1-0                      column 1-10, on the slot you last left it on
--   Super + Shift + 1-0              send window to column 1-10, same slot
--   Super + Ctrl + left/right        prev/next column, on the slot you last left it on
--   Super + Ctrl + up/down           slot above/below (stops one empty slot past the last used one)
--   Super + Ctrl + Shift + arrows    same moves, carrying the active window along
--   Super + scroll                   prev/next column
--   3-finger swipe                   drags the screen with your fingers (needs hyprcross.so);
--                                    pushing past an edge rubber-bands
--   3-finger pinch in / Super + Tab  overview: a grid of every column and slot (needs hyprcross.so);
--                                    swipes/2-finger scroll/arrows move, Enter/spread/click go, Escape backs out,
--                                    Tab flips between the cross and free modes (OVERVIEW_MODE),
--                                    Super + Tab again closes it
--
-- Without the plugin loaded, swipes still switch on release, edges just stop and
-- the pinch does nothing.

local M = {}

local COLUMNS  = 10
local MAX_ROWS = 10 -- rows 0-9

local PLUGIN   = os.getenv("HOME") .. "/.local/lib/hyprland/hyprcross.so"
local BUMP_PX  = 36  -- how far the screen nudges when you hit an edge
local BUMP_MS  = 90  -- how long it stays out before springing back

-- How the overview moves: "cross" keeps the highlight at the centre and slides
-- the grid under it, like the XMB; "free" moves the highlight over the grid and
-- the view follows. Tab flips it while the overview is up (until the next reload).
local OVERVIEW_MODE = "cross"

-- Hyprland loads registered plugins after the config has run, so look the
-- functions up when they are needed rather than now.
local havePlugin = io.open(PLUGIN) ~= nil
if havePlugin then hl.plugin.load(PLUGIN) end

local function plugin()
    local p = hl.plugin.cross
    return p and p.swipe_begin and p or nil
end

local function ws(col, row)
    return col * 10 + (row or 0)
end
M.ws = ws

-- Column and row of a workspace id. Old-style ids 1-9 count as the top of that column.
local function cell(id)
    if not id or id < 1 then return 1, 0 end
    if id < 10 then return id, 0 end
    return id // 10, id % 10
end

local function current()
    local w = hl.get_active_workspace()
    return cell(w and w.id)
end

local function occupied(id)
    local w = hl.get_workspace(id)
    return w ~= nil and w.windows > 0
end

-- Keep in sync with the workspaces animations in hyprland.lua.
local function slide(style)
    for _, leaf in ipairs({ "workspaces", "workspacesIn", "workspacesOut" }) do
        hl.animation({ leaf = leaf, enabled = true, speed = 1, spring = "wsSnap", style = style })
    end
end

local function go(col, row, carry)
    local here = current()
    local id = ws(col, row)
    -- The style is read when the switch starts, so it can go straight back to "slide".
    slide(col == here and "slidevert" or "slide")
    if carry then hl.dispatch(hl.dsp.window.move({ workspace = id })) end
    hl.dispatch(hl.dsp.focus({ workspace = id }))
    slide("slide")
end

-- Nudge the screen away from the edge you pushed against, then let it spring back.
local function bump(axis, dir)
    local p = plugin()
    if not p then return end
    p.bump(axis, -dir * BUMP_PX)
    hl.timer(function() p.settle() end, { timeout = BUMP_MS, type = "oneshot" })
end

-- Row each column was last left on, like the XMB remembering your place.
local lastRow = {}

hl.on("workspace.active", function()
    local w = hl.get_active_workspace()
    if not w or w.special then return end
    local col, row = cell(w.id)
    lastRow[col] = row
end)

-- Where moving one column over lands, or nil past the ends.
local function columnTarget(col)
    if col < 1 or col > COLUMNS then return nil end
    local row = lastRow[col] or 0
    if row > 0 and not occupied(ws(col, row)) then row = 0 end -- that slot has since emptied
    return ws(col, row)
end

-- The slot one step up/down, or nil when there's nowhere to go.
local function slotTarget(col, row, dir, carry)
    local target = row + dir
    if target < 0 or target >= MAX_ROWS then return nil end
    -- Going down from an empty slot would just open more empty ones, unless
    -- there's a used one further down (a gap left by closing windows).
    if dir > 0 and not carry then
        for r = row, MAX_ROWS - 1 do
            if occupied(ws(col, r)) then return ws(col, target) end
        end
        return nil
    end
    return ws(col, target)
end

-- The overview's grid: every column across, each with its slots down to one
-- empty slot past the last used one (and the one you're on, even if it's further
-- down), so there's always somewhere to go down to. x/y are grid positions.
-- `home` marks the slot each column puts on the row in cross mode: the one you're
-- on, or the one Super+Ctrl+left/right would land on.
local function gridCells(col, row)
    local cells = {}
    for c = 1, COLUMNS do
        local home = c == col and row or columnTarget(c) % 10
        local last = math.max(home, c == col and row or 0)
        for r = 0, MAX_ROWS - 1 do
            if occupied(ws(c, r)) then last = math.max(last, math.min(r + 1, MAX_ROWS - 1)) end
        end
        for r = 0, last do
            table.insert(cells, { id = ws(c, r), x = c - 1, y = r, home = r == home })
        end
    end
    return cells
end
M.gridCells = gridCells

local function horizontal(dir, carry)
    local id = columnTarget(current() + dir)
    if not id then return bump("x", dir) end
    go(id // 10, id % 10, carry)
end

local function vertical(dir, carry)
    local col, row = current()
    local id = slotTarget(col, row, dir, carry)
    if not id then return bump("y", dir) end
    go(col, id % 10, carry)
end

for col = 1, COLUMNS do
    hl.workspace_rule({ workspace = tostring(ws(col)), persistent = true, default_name = tostring(col) })
    for row = 1, MAX_ROWS - 1 do
        hl.workspace_rule({ workspace = tostring(ws(col, row)), default_name = col .. "." .. row })
    end

    local key = col % 10
    hl.bind("SUPER + " .. key,         function() go(col, columnTarget(col) % 10) end)
    hl.bind("SUPER + SHIFT + " .. key, function() go(col, columnTarget(col) % 10, true) end)
end

hl.bind("SUPER + CTRL + left",  function() horizontal(-1) end)
hl.bind("SUPER + CTRL + right", function() horizontal(1) end)
hl.bind("SUPER + CTRL + up",    function() vertical(-1) end)
hl.bind("SUPER + CTRL + down",  function() vertical(1) end)

hl.bind("SUPER + CTRL + SHIFT + left",  function() horizontal(-1, true) end)
hl.bind("SUPER + CTRL + SHIFT + right", function() horizontal(1, true) end)
hl.bind("SUPER + CTRL + SHIFT + up",    function() vertical(-1, true) end)
hl.bind("SUPER + CTRL + SHIFT + down",  function() vertical(1, true) end)

hl.bind("SUPER + mouse_down", function() horizontal(1) end)
hl.bind("SUPER + mouse_up",   function() horizontal(-1) end)

-- Overview: pinching three fingers shrinks the screen into a grid of every
-- column and slot (needs hyprcross.so). While it's up, 3-finger swipes, 2-finger
-- scrolling and arrows move the highlight, Enter / spreading the fingers /
-- clicking a thumbnail goes there, Escape goes back, Tab flips the mode, and
-- nothing else gets input.
local ov = { open = false, pinching = false, mode = OVERVIEW_MODE }
M.ov = ov

local function ovLeave()
    ov.open, ov.pinching = false, false
    hl.dispatch(hl.dsp.submap("reset"))
end

-- The plugin drops the overview on its own when something else switches
-- workspaces, the screen locks or the monitor goes; catch up with that before
-- acting on ov.open. Returns the plugin, if loaded.
local function ovSync()
    local p = plugin()
    if ov.open and not (p and p.overview_active()) then ovLeave() end
    return p
end

local function ovPick(id)
    local p = plugin()
    if not p or not id or not (ov.open or ov.pinching) then return end
    -- before go(): the switch would otherwise look like someone else's and drop the overview
    p.overview_close(id)
    ovLeave()
    local col, row = cell(id)
    go(col, row)
end

local function ovCancel()
    local p = plugin()
    if p then p.overview_close() end
    ovLeave()
end

local function ovPickSelected()
    local p = ovSync()
    if p and ov.open then ovPick(p.overview_selected()) end
end

local function pinchStart(e)
    local p = ovSync()
    if not p or ov.open or ov.pinching then return end
    local col, row = current()
    ov.pinching = p.overview_begin(gridCells(col, row), ws(col, row), ovPick, ov.mode)
    if ov.pinching then p.overview_update(e.scale, e.time_ms) end
end

local function pinchUpdate(e)
    local p = plugin()
    if ov.pinching and p then p.overview_update(e.scale, e.time_ms) end
end

local function pinchEnd(e)
    if not ov.pinching then return end
    ov.pinching = false
    local p = plugin()
    ov.open = p ~= nil and p.overview_end(e.cancelled)
    if ov.open then hl.dispatch(hl.dsp.submap("cross_overview")) end
end

-- Super + Tab: the same overview without a pinch, zooming out on its own.
local function ovOpen()
    local p = ovSync()
    if not p or ov.open or ov.pinching then return end
    local col, row = current()
    if not p.overview_begin(gridCells(col, row), ws(col, row), ovPick, ov.mode) then return end
    ov.open = p.overview_end(false, true)
    if ov.open then hl.dispatch(hl.dsp.submap("cross_overview")) end
end
hl.bind("SUPER + Tab", ovOpen)

-- Hyprland names pinches by zoom: fingers coming together (scale < 1) is
-- "pinchout", spreading them is "pinchin".
hl.gesture({ fingers = 3, direction = "pinchout", action = { start = pinchStart, update = pinchUpdate, finish = pinchEnd } })
hl.gesture({ fingers = 3, direction = "pinchin", action = { finish = ovPickSelected } })

-- Keys only while the overview is up. If it went away on its own (a keybind
-- switched workspaces, the screen locked), the first key just drops the submap.
local function ovKey(fn)
    return function()
        ovSync()
        if not ov.open then return ovLeave() end
        fn()
    end
end

hl.define_submap("cross_overview", function()
    -- held arrows step once, then keep going at the keyboard's repeat rate
    local held = { repeating = true }
    hl.bind("left",   ovKey(function() plugin().overview_move(-1, 0) end), held)
    hl.bind("right",  ovKey(function() plugin().overview_move(1, 0) end), held)
    hl.bind("up",     ovKey(function() plugin().overview_move(0, -1) end), held)
    hl.bind("down",   ovKey(function() plugin().overview_move(0, 1) end), held)
    hl.bind("Return", ovKey(ovPickSelected))
    hl.bind("Escape", ovKey(ovCancel))
    hl.bind("SUPER + Tab", ovKey(ovCancel))
    hl.bind("Tab",    ovKey(function()
        ov.mode = ov.mode == "cross" and "free" or "cross"
        plugin().overview_mode(ov.mode)
    end))
    -- every other key goes nowhere while the overview is up
    hl.bind("catchall", ovKey(function() end))
end)

-- Follow the plugin dropping the overview so the arrows go back to normal.
-- (This can run before the plugin hears of the switch; ovSync catches up later.)
hl.on("workspace.active", function() ovSync() end)

-- Swipes. The screen follows the fingers, so moving them left/up reveals the
-- column to the right / the slot below ("after"); right/down reveals "before".
-- What a swipe does is settled when it starts: "overview" moves the highlight,
-- "track" drags the screen, "release" (no plugin) switches once the fingers
-- lift, and nil (the plugin said no) does nothing.
local swipe = { axis = nil, moved = 0, mode = nil }

local function swipeStart(e)
    -- e.direction is where the fingers actually went (LEFT/RIGHT/UP/DOWN)
    swipe.axis  = (e.direction == "UP" or e.direction == "DOWN" or e.direction == "VERTICAL") and "y" or "x"
    swipe.moved = e.delta[swipe.axis]
    swipe.mode  = nil

    local p = ovSync()
    if not p then swipe.mode = "release"; return end
    if ov.open then
        swipe.mode = "overview"
        return p.overview_drag(e.delta.x, e.delta.y)
    end
    if ov.pinching then return end
    local col, row = current()
    local before, after
    if swipe.axis == "x" then
        before, after = columnTarget(col - 1), columnTarget(col + 1)
    else
        before, after = slotTarget(col, row, -1), slotTarget(col, row, 1)
    end
    if p.swipe_begin(swipe.axis, before, after) then
        swipe.mode = "track"
        p.swipe_update(swipe.moved, e.time_ms)
    end
end

local function swipeUpdate(e)
    local p = plugin()
    if swipe.mode == "overview" then
        if p then p.overview_drag(e.delta.x, e.delta.y) end
        return
    end
    local d = e.delta[swipe.axis]
    swipe.moved = swipe.moved + d
    if swipe.mode == "track" and p then p.swipe_update(d, e.time_ms) end
end

-- Hyprland reads this one as "finish" (its error message also says "end", which is ignored).
local function swipeEnd(e)
    local p, mode = plugin(), swipe.mode
    swipe.mode = nil
    if mode == "overview" then
        if p then p.overview_release() end
    elseif mode == "track" then
        if p then p.swipe_end(e.cancelled) end
    elseif mode == "release" and not e.cancelled and math.abs(swipe.moved) > 50 then
        local dir = swipe.moved < 0 and 1 or -1
        if swipe.axis == "x" then horizontal(dir) else vertical(dir) end
    end
end

for _, direction in ipairs({ "horizontal", "vertical" }) do
    hl.gesture({ fingers = 3, direction = direction, action = { start = swipeStart, update = swipeUpdate, finish = swipeEnd } })
end

return M
