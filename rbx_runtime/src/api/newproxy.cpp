// newproxy.cpp — the newproxy([bool]) global.
//
// Roblox-specific (upstream Luau removed newproxy). Profile-driven:
// newproxy=error makes any call error; otherwise it yields a bare userdata,
// optionally with an empty table as its metatable when called with `true`
// (challenge programs probe newproxy(true) — the +52 calibration term).
#include "api/api.h"

#include "lua.h"
#include "lualib.h"

namespace rbxch {
namespace api {

namespace {

int lua_newproxy_shim(lua_State* L) {
    const Options& o = opts(L);
    if (o.newproxy == "error") {
        luaL_error(L, "newproxy unavailable");
        return 0;
    }
    int b = luaL_optboolean(L, 1, 0);
    (void)lua_newuserdata(L, sizeof(void*));
    if (b) {
        lua_newtable(L);
        lua_setmetatable(L, -2);
    }
    return 1;
}

} // namespace

void register_newproxy(lua_State* L, const Options&) {
    lua_pushcfunction(L, lua_newproxy_shim, "newproxy");
    lua_setglobal(L, "newproxy");
}

} // namespace api
} // namespace rbxch
