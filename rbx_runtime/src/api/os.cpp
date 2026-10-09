// os.cpp — os.exit override (sandbox escape hatch control).
//
// Profile-driven: os_exit=missing makes any call error (production client
// behavior); "exit" terminates the process; anything else ("ok") is a no-op.
// The os table itself comes from luaL_openlibs — this module only replaces
// the `exit` field (creating a bare table if `os` were ever missing).
#include "api/api.h"

#include "lua.h"
#include "lualib.h"

#include <cstdlib>

namespace rbxch {
namespace api {

namespace {

int lua_osexit(lua_State* L) {
    const Options& o = opts(L);
    if (o.os_exit == "missing") {
        luaL_error(L, "os.exit unavailable");
        return 0;
    }
    if (o.os_exit == "exit")
        std::exit(0);
    return 0;
}

} // namespace

void register_os(lua_State* L, const Options&) {
    lua_getglobal(L, "os");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "os");
    }
    lua_pushcfunction(L, lua_osexit, "exit");
    lua_setfield(L, -2, "exit");
    lua_pop(L, 1);
}

} // namespace api
} // namespace rbxch
