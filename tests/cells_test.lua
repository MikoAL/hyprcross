-- Run with `make test`. Loads cross.lua against a stubbed `hl`.
package.path = "./?.lua;./tests/?.lua;" .. package.path
local stub = require("stub_hl")
_G.hl = stub.hl
local cross = require("cross")

-- cross.lua's workspace.active handler (it tracks the slot each column was left on)
local onActive
for _, call in ipairs(stub.calls) do
    if call[1] == "hl.on" and call[2] == "workspace.active" then onActive = call[3]; break end
end

local function cellsById(cells)
    local byId = {}
    for _, c in ipairs(cells) do byId[c.id] = c end
    return byId
end

local function check(name, cond)
    if not cond then error("FAIL: " .. name, 2) end
end

-- every column, each down to one empty slot past its last used one
do
    stub.reset(); stub.occupy(10, 30, 31, 32, 52)
    local cells = cross.gridCells(1, 0)
    local byId = cellsById(cells)
    for r = 0, 3 do
        check("slot 3." .. r, byId[30 + r] and byId[30 + r].x == 2 and byId[30 + r].y == r)
    end
    check("only one empty slot after the last", byId[34] == nil)
    check("gap slot kept", byId[51] and byId[51].x == 4 and byId[51].y == 1)
    check("empty column is just its top slot", byId[70] and byId[71] == nil)
    check("used top slot gets one below", byId[11] and byId[12] == nil)
    check("column 10", byId[100] and byId[100].x == 9 and byId[100].y == 0)
    check("empty slot under a gap column", byId[53] and byId[54] == nil)
    -- column 1: 10, 11; 3: 30-33; 5: 50-53; the other seven: their top slot
    check("count", #cells == 2 + 4 + 4 + 7)
end

-- slots stop at MAX_ROWS
do
    stub.reset()
    for r = 0, 9 do stub.occupy(30 + r) end
    local byId = cellsById(cross.gridCells(1, 0))
    check("last slot 39", byId[39] and byId[39].y == 9)
end

-- the slot you're on shows even when empty
do
    stub.reset()
    local byId = cellsById(cross.gridCells(4, 2))
    check("current empty slot", byId[42] and byId[41] and byId[42].y == 2)
end

-- each column's home (the slot on the overview's row) is the one it was last left on,
-- the current slot for the column you're in, and the top once that slot has emptied
do
    stub.reset(); stub.occupy(10, 30, 31, 32, 50)
    for _, id in ipairs({ 31, 52, 10 }) do
        stub.active = id
        onActive()
    end
    local homes = {}
    for _, c in ipairs(cross.gridCells(2, 0)) do
        if c.home then
            check("one home per column", homes[c.x] == nil)
            homes[c.x] = c.id
        end
    end
    check("remembered slot", homes[2] == 31)
    check("current column's slot", homes[1] == 20)
    check("emptied slot falls back to the top", homes[4] == 50)
    check("untouched column is its top", homes[0] == 10)
    check("every column has one", homes[9] == 100)
end

print("ok")
