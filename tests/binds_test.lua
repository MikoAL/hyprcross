-- Run with `make test`. Presses cross.lua's keybinds against a stubbed `hl`.
package.path = "./?.lua;./tests/?.lua;" .. package.path
local stub = require("stub_hl")
_G.hl = stub.hl
require("cross")

local binds, onActive = {}, {}
for _, call in ipairs(stub.calls) do
    if call[1] == "hl.bind" then binds[call[2]] = call[3] end
    if call[1] == "hl.on" and call[2] == "workspace.active" then table.insert(onActive, call[3]) end
end

local function check(name, cond)
    if not cond then error("FAIL: " .. name, 2) end
end

-- Workspace the last dispatched focus went to.
local function focused()
    local id
    for _, call in ipairs(stub.calls) do
        if call[1] == "hl.dsp.focus" then id = call[2].workspace end
    end
    return id
end

local function visit(...)
    for _, id in ipairs({ ... }) do
        stub.active = id
        for _, fn in ipairs(onActive) do fn() end
    end
end

-- Super + N lands on the slot column N was last left on, like Super+Ctrl+left/right
do
    stub.reset(); stub.occupy(10, 30, 31, 32)
    visit(32, 10)
    binds["SUPER + 3"]()
    check("remembered slot", focused() == 32)
end

-- ...the top once that slot has emptied, or for a column never visited
do
    stub.reset(); stub.occupy(10, 30)
    visit(32, 10)
    binds["SUPER + 3"]()
    check("emptied slot falls back to the top", focused() == 30)
    binds["SUPER + 5"]()
    check("untouched column is its top", focused() == 50)
end

-- Super + 0 is column 10
do
    stub.reset(); stub.occupy(10, 100, 101)
    visit(101, 10)
    binds["SUPER + 0"]()
    check("column 10", focused() == 101)
end

-- Super + Shift + N carries the window to the same slot
do
    stub.reset(); stub.occupy(10, 40, 41)
    visit(41, 10)
    stub.calls = {}
    binds["SUPER + SHIFT + 4"]()
    local moved
    for _, call in ipairs(stub.calls) do
        if call[1] == "hl.dsp.window.move" then moved = call[2].workspace end
    end
    check("window sent to the remembered slot", moved == 41 and focused() == 41)
end

-- Super + Ctrl + down crosses an empty slot when there's a used one further down
do
    stub.reset(); stub.occupy(10, 11, 13); stub.active = 12
    binds["SUPER + CTRL + down"]()
    check("down through a gap", focused() == 13)
    stub.calls = {}; stub.active = 14
    binds["SUPER + CTRL + down"]()
    check("stops below the last used slot", focused() == nil)
end

print("ok")
