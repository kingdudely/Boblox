// game.cpp — the `game` global (DataModel root): JobId + GetService dispatch.
//
// GetService(name) behavior contract (proven by the regressions):
//   * registered service  -> the SAME table on every call
//   * unknown service     -> a FRESH empty table per call (deliberate: a
//     challenge program can observe identity, so we must not return a shared
//     stub). Future services (workspace, Players, …) become visible here by
//     registering in their own module — this file never changes.
#include "api/api.h"

#include "lua.h"
#include "lualib.h"

namespace rbxch {
namespace api {

namespace {

int lua_game_getservice(lua_State* L) {
    // Called as game:GetService(name) — index 2 is `name` (index 1 = self).
    // A dot-call game.GetService("X") must error exactly like before.
    const char* name = luaL_checkstring(L, 2);
    if (push_service(L, name))
        return 1; // [service]
    lua_newtable(L); // unknown service: fresh empty table (see contract above)
    return 1;
}

} // namespace

void register_game(lua_State* L, const Options& opts) {
    lua_newtable(L); // [game]
    lua_pushstring(L, opts.job.c_str());
    lua_setfield(L, -2, "JobId");
    lua_pushcfunction(L, lua_game_getservice, "GetService");
    lua_setfield(L, -2, "GetService");
    lua_setglobal(L, "game");
}

} // namespace api
} // namespace rbxch
