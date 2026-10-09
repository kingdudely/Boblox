// sandbox.cpp — installs the Roblox environment into a fresh lua_State.
//
// Composition:
//   1. publish the sandbox Options into the Lua registry (api::opts reads it)
//   2. luaL_openlibs (standard Luau libraries)
//   3. run every API module from api/registry.cpp, in order
//
// All Roblox-specific surface lives in api/ — this file never grows when a
// new API is added (see api/api.h for the "add an API" recipe).
#include "sandbox.h"

#include "api/api.h"

#include "lua.h"
#include "lualib.h"

namespace rbxch {

void setup_sandbox(lua_State* L, const Options& opts) {
    // (const Options*) as light userdata — valid for the state's lifetime
    // because run() keeps its Options copy alive until lua_close(). Replaces
    // the old process-global g_opts, so run() is re-entrant.
    lua_pushlightuserdata(L, const_cast<Options*>(&opts));
    lua_setfield(L, LUA_REGISTRYINDEX, "rbx.Options");

    luaL_openlibs(L);

    // Profile-filtered: only modules whose mask matches Options::profile run.
    // The Challenge profile (the solve path default) sees exactly the
    // historical surface — see api/api.h and the unit_profile test.
    for (size_t i = 0; i < api::kModuleCount; i++) {
        const api::Module& m = api::kModules[i];
        if (m.profiles & api::profile_bit(opts.profile))
            m.register_globals(L, opts);
    }
}

} // namespace rbxch
