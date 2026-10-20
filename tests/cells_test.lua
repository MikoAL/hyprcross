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

-- the selected column expands into its slots, one empty slot past the last
do
    stub.reset(); stub.occupy(30, 31, 32)
    local cells = cross.crossCells(3, 1)
    local byId = cellsById(cells)
    for r, y in pairs({ [0] = -1, [1] = 0, [2] = 1, [3] = 2 }) do
        local c = byId[30 + r]
        check("slot 3." .. r .. " present", c ~= nil)
        check("slot 3." .. r .. " position", c.x == 0 and c.y == y)
    end
    check("column 1 cell", byId[10] and byId[10].x == -2 and byId[10].y == 0)
    check("column 10 cell", byId[100] and byId[100].x == 7 and byId[100].y == 0)
    check("13 cells", #cells == 13)
end

-- other columns show the slot they were last left on, if it still has windows
do
    stub.reset(); stub.occupy(52)
    cross.lastRow[5] = 2
    check("remembered slot", cellsById(cross.crossCells(3, 0))[52] ~= nil)
    stub.reset()
    check("emptied slot falls back to top", cellsById(cross.crossCells(3, 0))[50] ~= nil)
    cross.lastRow[5] = nil
end

-- an empty column is just its top slot
do
    stub.reset()
    local col4 = {}
    for _, c in ipairs(cross.crossCells(4, 0)) do if c.x == 0 then table.insert(col4, c) end end
    check("empty column has one cell", #col4 == 1 and col4[1].id == 40 and col4[1].y == 0)
end

-- slots stop at MAX_ROWS
do
    stub.reset()
    for r = 0, 9 do stub.occupy(30 + r) end
    local byId = cellsById(cross.crossCells(3, 0))
    check("last slot 39", byId[39] ~= nil and byId[39].y == 9)
    check("nothing past 39", byId[40] == nil or byId[40].x ~= 0)
end

-- stepping the overview selection
do
    stub.reset(); stub.occupy(30, 31, 40)
    local selected
    stub.hl.plugin.cross = {
        swipe_begin     = function() end,
        overview_active = function() return true end,
        overview_select = function(cells, id) selected = { cells = cells, id = id } end,
    }
    cross.ov.open, cross.ov.col, cross.ov.row = true, 3, 1
    cross.ovStep("x", 1)
    check("right selects column 4", selected and selected.id == 40 and cross.ov.col == 4 and cross.ov.row == 0)
    local byId = cellsById(selected.cells)
    check("cross re-centred on 4", byId[40].x == 0 and byId[40].y == 0 and byId[30].x == -1)

    selected = nil
    cross.ov.col, cross.ov.row = 10, 0
    cross.ovStep("x", 1)
    check("no column past 10", selected == nil and cross.ov.col == 10)

    cross.ov.col, cross.ov.row = 3, 1
    cross.ovStep("y", 1)
    check("down to the empty slot after the last", selected and selected.id == 32)
    selected = nil
    cross.ov.row = 2
    cross.ovStep("y", 1)
    check("not past the empty slot", selected == nil and cross.ov.row == 2)
    cross.ov.open = false
end

print("ok")
