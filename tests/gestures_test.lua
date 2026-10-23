-- Run with `make test`. Drives cross.lua's gesture handlers against a stubbed `hl`.
package.path = "./?.lua;./tests/?.lua;" .. package.path
local stub = require("stub_hl")
_G.hl = stub.hl
local cross = require("cross")

-- the handlers cross.lua registered, by direction, and its top-level binds
local gestures, binds = {}, {}
for _, call in ipairs(stub.calls) do
    if call[1] == "hl.gesture" then gestures[call[2].direction] = call[2].action end
    if call[1] == "hl.bind" and not binds[call[2]] then binds[call[2]] = call[3] end
end
local swipe, pinch, spread = gestures.horizontal, gestures.pinchout, gestures.pinchin

local function check(name, cond)
    if not cond then error("FAIL: " .. name, 2) end
end

local function called(name)
    for _, call in ipairs(stub.calls) do
        if call[1] == name then return true end
    end
    return false
end

-- The plugin, with these Lua functions answering instead of the stub's no-ops.
local p = stub.hl.plugin.cross
local function plugin(fns)
    for _, k in ipairs({ "swipe_begin", "overview_active", "overview_selected", "overview_begin", "overview_end" }) do
        p[k] = fns[k] or function() end
    end
end

local function drag(dx)
    swipe.start({ direction = dx < 0 and "LEFT" or "RIGHT", delta = { x = dx, y = 0 }, time_ms = 0 })
    swipe.update({ delta = { x = dx, y = 0 }, time_ms = 10 })
    swipe.finish({ cancelled = false })
end

-- a swipe the plugin turns down doesn't fall back to switching on release
do
    stub.reset(); cross.ov.open = false
    plugin({ swipe_begin = function() return false end })
    drag(-200)
    check("no switch when swipe_begin says no", not called("hl.dsp.focus"))
end

-- the overview went away on its own: swipes work again instead of being eaten
do
    stub.reset(); cross.ov.open = true
    local began = false
    plugin({ swipe_begin = function() began = true; return true end, overview_active = function() return false end })
    drag(-200)
    check("stale overview dropped", not cross.ov.open)
    check("swipe tracked", began)
    check("not fed to the overview", not called("hl.plugin.cross.overview_drag"))
end

-- ... and pinching can open it again
do
    stub.reset(); cross.ov.open = true
    local opened = false
    plugin({ overview_begin = function() opened = true; return true end, overview_active = function() return false end })
    pinch.start({ scale = 0.9, time_ms = 0 })
    check("pinch reopens after a stale overview", opened and cross.ov.pinching)
    cross.ov.pinching = false
end

-- spreading the fingers after it went away doesn't jump anywhere
do
    stub.reset(); cross.ov.open = true
    plugin({ overview_active = function() return false end })
    spread.finish({})
    check("no jump on a stale spread", not called("hl.dsp.focus"))
    check("stale spread leaves", not cross.ov.open)
end

-- while it's up, swipes move the highlight, start to finish
do
    stub.reset(); cross.ov.open = true
    plugin({ overview_active = function() return true end })
    drag(-200)
    check("overview drag", called("hl.plugin.cross.overview_drag"))
    check("overview release", called("hl.plugin.cross.overview_release"))
    check("no switch", not called("hl.dsp.focus"))
    cross.ov.open = false
end

-- the overview opens in OVERVIEW_MODE ("cross"); Tab flips it, and later pinches keep the flip
do
    stub.reset(); cross.ov.open, cross.ov.pinching = false, false
    local modes = {}
    plugin({ overview_begin = function(_, _, _, mode) table.insert(modes, mode); return true end,
             overview_active = function() return true end })
    pinch.start({ scale = 0.9, time_ms = 0 })
    check("opens in cross mode", modes[1] == "cross")
    cross.ov.pinching, cross.ov.open = false, true

    local tab
    for _, call in ipairs(stub.submaps) do
        if call[1] == "Tab" then tab = call[2] end
    end
    check("Tab bound in the overview", tab ~= nil)
    local flipped
    p.overview_mode = function(mode) flipped = mode end
    tab()
    check("Tab flips to free", flipped == "free")
    tab()
    check("Tab flips back to cross", flipped == "cross")
    tab()

    cross.ov.open = false
    pinch.start({ scale = 0.9, time_ms = 0 })
    check("the flip sticks for the next pinch", modes[2] == "free")
    cross.ov.pinching = false
end

-- Super + Tab opens the overview straight away, and closes it again from inside
do
    stub.reset(); stub.active = 21; cross.ov.open, cross.ov.pinching = false, false
    local begun, forced
    plugin({ overview_begin = function(_, selected) begun = selected; return true end,
             overview_end = function(cancelled, open) forced = not cancelled and open; return true end,
             overview_active = function() return true end })
    local open, close = binds["SUPER + Tab"], nil
    for _, call in ipairs(stub.submaps) do
        if call[1] == "SUPER + Tab" then close = call[2] end
    end
    open()
    check("Super + Tab opens on the current slot", begun == 21)
    check("without waiting for a pinch", forced == true)
    check("and is open", cross.ov.open)
    stub.reset()
    close()
    check("Super + Tab again closes it", called("hl.plugin.cross.overview_close") and not cross.ov.open)
end

-- an empty slot with a used one further down can still be swiped down from (1-1, gap at 1-2, 1-3)
do
    stub.reset(); stub.occupy(10, 11, 13); stub.active = 12; cross.ov.open = false
    local before, after = "unset", "unset"
    plugin({ swipe_begin = function(_, b, a) before, after = b, a; return true end })
    swipe.start({ direction = "UP", delta = { x = 0, y = -10 }, time_ms = 0 })
    swipe.finish({ cancelled = false })
    check("gap slot reveals the one below", after == 13)
    check("and the one above", before == 11)

    stub.active = 14
    swipe.start({ direction = "UP", delta = { x = 0, y = -10 }, time_ms = 0 })
    swipe.finish({ cancelled = false })
    check("nothing below the last empty slot", after == nil)
end

print("ok")
