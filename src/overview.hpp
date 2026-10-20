// The pinch overview: live thumbnails of the workspaces cross.lua lists, laid
// out as an XMB cross around the selection. Which workspaces and what a pick
// means is up to cross.lua; this only draws and animates.
#pragma once

extern "C" {
#include <lua.h>
}

namespace cross::overview {
    // Lua: hl.plugin.cross.overview_*
    int luaBegin(lua_State* L);  // (cells, selected, on_pick) -> bool
    int luaUpdate(lua_State* L); // (scale, time_ms)
    int luaEnd(lua_State* L);    // (cancelled) -> bool open
    int luaSelect(lua_State* L); // (cells, selected)
    int luaClose(lua_State* L);  // (id|nil)
    int luaActive(lua_State* L); // () -> bool

    bool active();
    void init();
    void teardown();
}
