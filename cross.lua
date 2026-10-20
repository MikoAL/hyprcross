-- cross.lua (symlinked to ~/.config/hypr/cross.lua)
-- PS3/PS4-style "cross" workspaces: columns run left/right along the main row,
-- and each column has slots stacked under it for windows that each want the
-- whole screen.
--
-- Workspace id = 10 * column + row, so column 1 is 10, 11, 12... Hyprland picks
-- the slide direction by comparing ids; this ordering keeps it right both ways.
--
--   Super + 1-0                      top of column 1-10
--   Super + Shift + 1-0              send window to top of column 1-10
--   Super + Ctrl + left/right        prev/next column, on the slot you last left it on
--   Super + Ctrl + up/down           slot above/below (stops one empty slot past the last)
--   Super + Ctrl + Shift + arrows    same moves, carrying the active window along
--   Super + scroll                   prev/next column
--   3-finger swipe                   drags the screen with your fingers (needs hyprcross.so);
--                                    pushing past an edge rubber-bands
--
-- Without the plugin loaded, swipes still switch on release and edges just stop.

local M = {}

local COLUMNS  = 10
local MAX_ROWS = 10 -- rows 0-9

local PLUGIN   = os.getenv("HOME") .. "/.local/lib/hyprland/hyprcross.so"
local BUMP_PX  = 36  -- how far the screen nudges when you hit an edge
local BUMP_MS  = 90  -- how long it stays out before springing back

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
    -- Going down from an empty slot would just open more empty ones.
    if dir > 0 and not carry and not occupied(ws(col, row)) then return nil end
    return ws(col, target)
end

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
    hl.bind("SUPER + " .. key,         function() go(col, 0) end)
    hl.bind("SUPER + SHIFT + " .. key, function() go(col, 0, true) end)
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

-- Swipes. The screen follows the fingers, so moving them left/up reveals the
-- column to the right / the slot below ("after"); right/down reveals "before".
local swipe = { axis = nil, moved = 0, tracking = false }

local function swipeStart(e)
    -- e.direction is where the fingers actually went (LEFT/RIGHT/UP/DOWN)
    swipe.axis     = (e.direction == "UP" or e.direction == "DOWN" or e.direction == "VERTICAL") and "y" or "x"
    swipe.moved    = e.delta[swipe.axis]
    swipe.tracking = false

    local p = plugin()
    if not p then return end
    local col, row = current()
    local before, after
    if swipe.axis == "x" then
        before, after = columnTarget(col - 1), columnTarget(col + 1)
    else
        before, after = slotTarget(col, row, -1), slotTarget(col, row, 1)
    end
    swipe.tracking = p.swipe_begin(swipe.axis, before, after)
    if swipe.tracking then p.swipe_update(swipe.moved, e.time_ms) end
end

local function swipeUpdate(e)
    local d = e.delta[swipe.axis]
    swipe.moved = swipe.moved + d
    if swipe.tracking then plugin().swipe_update(d, e.time_ms) end
end

-- Hyprland reads this one as "finish" (its error message also says "end", which is ignored).
local function swipeEnd(e)
    if swipe.tracking then
        plugin().swipe_end(e.cancelled)
    elseif not e.cancelled and math.abs(swipe.moved) > 50 then
        -- no plugin: switch once the fingers lift
        local dir = swipe.moved < 0 and 1 or -1
        if swipe.axis == "x" then horizontal(dir) else vertical(dir) end
    end
    swipe.tracking = false
end

for _, direction in ipairs({ "horizontal", "vertical" }) do
    hl.gesture({ fingers = 3, direction = direction, action = { start = swipeStart, update = swipeUpdate, finish = swipeEnd } })
end

return M
