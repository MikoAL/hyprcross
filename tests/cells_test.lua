-- Run with `make test`. Loads cross.lua against a stubbed `hl`.
package.path = "./?.lua;./tests/?.lua;" .. package.path
local stub = require("stub_hl")
_G.hl = stub.hl
local cross = require("cross")

local function cellsById(cells)
    local byId = {}
    for _, c in ipairs(cells) do byId[c.id] = c end
    return byId
end

local function check(name, cond)
    if not cond then error("FAIL: " .. name, 2) end
end

-- every column, each down to its last used slot
do
    stub.reset(); stub.occupy(10, 30, 31, 32, 52)
    local cells = cross.gridCells(1, 0)
    local byId = cellsById(cells)
    for r = 0, 2 do
        check("slot 3." .. r, byId[30 + r] and byId[30 + r].x == 2 and byId[30 + r].y == r)
    end
    check("no empty slot after the last", byId[33] == nil)
    check("gap slot kept", byId[51] and byId[51].x == 4 and byId[51].y == 1)
    check("empty column is its top slot", byId[70] and byId[71] == nil)
    check("column 10", byId[100] and byId[100].x == 9 and byId[100].y == 0)
    check("count", #cells == 10 + 2 + 2)
end

-- the slot you're on shows even when empty
do
    stub.reset()
    local byId = cellsById(cross.gridCells(4, 2))
    check("current empty slot", byId[42] and byId[41] and byId[42].y == 2)
end

print("ok")
