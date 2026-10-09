// runservice.cpp — RunService service: IsStudio().
//
// Registers itself into rbx.Services ("RunService"), so game:GetService
// finds it without game.cpp knowing this module exists. See api/api.h.
#include "api/api.h"

#include "lua.h"
#include "lualib.h"

namespace rbxch {
namespace api {

namespace {

int lua_isstudio(lua_State* L) {
    const Options& o = opts(L);
    if (o.studio == "error") {
        luaL_error(L, "IsStudio unavailable");
        return 0;
    }
    lua_pushboolean(L, o.studio == "true");
    return 1;
}

} // namespace

void register_runservice(lua_State* L, const Options&) {
    lua_newtable(L); // [RunService]
    lua_pushcfunction(L, lua_isstudio, "IsStudio");
    lua_setfield(L, -2, "IsStudio");
    register_service(L, "RunService"); // consumes [RunService]
}

} // namespace api
} // namespace rbxch
