-- A fake `hl` for loading cross.lua outside Hyprland. Unknown fields are
-- callable no-ops (hl.dsp.window.move(...) etc.); `calls` records plugin calls,
-- `submaps` the { key, fn } binds made inside hl.define_submap.
local stub = { workspaces = {}, active = 10, calls = {}, submaps = {} }

local function proxy(name)
    return setmetatable({}, {
        __index = function(t, k) local p = proxy(name .. "." .. k); rawset(t, k, p); return p end,
        __call  = function(_, ...) table.insert(stub.calls, { name, ... }) end,
    })
end

local hl = proxy("hl")
hl.get_workspace        = function(id) return stub.workspaces[id] end
hl.get_active_workspace = function() return stub.workspaces[stub.active] or { id = stub.active, windows = 0 } end

local inSubmap = false
hl.define_submap = function(_, fn) inSubmap = true; fn(); inSubmap = false end
hl.bind = function(key, fn, ...)
    table.insert(stub.calls, { "hl.bind", key, fn, ... })
    if inSubmap then table.insert(stub.submaps, { key, fn }) end
end
stub.hl = hl

-- Mark workspaces as having windows: stub.occupy(30, 31, 32)
function stub.occupy(...)
    for _, id in ipairs({ ... }) do stub.workspaces[id] = { id = id, windows = 1 } end
end

function stub.reset()
    stub.workspaces, stub.active, stub.calls = {}, 10, {}
end

return stub
