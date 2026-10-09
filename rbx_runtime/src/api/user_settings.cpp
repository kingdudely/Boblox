// user_settings.cpp — the UserSettings() global.
//
// Profile-driven (see Options): usersettings=error registers the global as
// nil (calling it errors like production); otherwise it returns a table whose
// metatable __tostring yields opts.us_string — the calibration string the
// challenge scoring hashes (tostring(UserSettings()) byte-sum, FINDINGS.md).
#include "api/api.h"

#include "lua.h"
#include "lualib.h"

namespace rbxch {
namespace api {

namespace {

int us_tostring_cont(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    return 1;
}

int lua_usersettings(lua_State* L) {
    const Options& o = opts(L);
    if (o.usersettings == "error") {
        // Unreachable while registration (below) gates on the same value —
        // kept for parity with the original shim, in case a future profile
        // registers the function eagerly.
        luaL_error(L, "UserSettings unavailable");
        return 0;
    }
    lua_newtable(L); // [result]
    lua_newtable(L); // [result, mt]
    lua_pushstring(L, o.us_string.c_str());
    lua_pushcclosure(L, us_tostring_cont, "__tostring", 1);
    lua_setfield(L, -2, "__tostring");
    lua_setmetatable(L, -2);
    return 1;
}

} // namespace

void register_usersettings(lua_State* L, const Options& opts) {
    if (opts.usersettings == "error") {
        lua_pushnil(L);
        lua_setglobal(L, "UserSettings");
    } else {
        lua_pushcfunction(L, lua_usersettings, "UserSettings");
        lua_setglobal(L, "UserSettings");
    }
}

} // namespace api
} // namespace rbxch
