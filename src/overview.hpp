// The pinch overview: live thumbnails of the workspaces cross.lua lists, laid
// out on a grid with a springy highlight. Which workspaces and what a pick
// means is up to cross.lua; this only draws and animates.
#pragma once

extern "C" {
#include <lua.h>
}

namespace cross::overview {
    // Lua: hl.plugin.cross.overview_*
    int luaBegin(lua_State* L);    // (cells, selected, on_pick) -> bool
    int luaUpdate(lua_State* L);   // (scale, time_ms)
    int luaEnd(lua_State* L);      // (cancelled) -> bool open
    int luaDrag(lua_State* L);     // (dx, dy) fingers moved, touchpad units
    int luaRelease(lua_State* L);  // () fingers lifted
    int luaMove(lua_State* L);     // (dx, dy) to the neighbouring thumbnail
    int luaSelected(lua_State* L); // () -> id under the highlight
    int luaClose(lua_State* L);    // (id|nil)
    int luaActive(lua_State* L);   // () -> bool

    bool active();
    void init();
    void teardown();
}
