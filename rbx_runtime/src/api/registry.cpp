// registry.cpp — THE ordered list of Roblox API modules (see api/api.h).
//
// To add a Roblox API surface (workspace, Players, Instance, …):
//   1. write api/<name>.cpp exposing  void register_<name>(lua_State*, const Options&)
//   2. declare it below and append it to kModules
//   3. add the .cpp to challenge_core in CMakeLists.txt
//
// ORDER IS BEHAVIOR: modules run in list order while the environment is
// installed; later modules may observe globals registered by earlier ones.
// The current order mirrors the historical setup_sandbox() exactly — do not
// reshuffle it without re-running the regressions (tools/regress9b.py and
// client/build/regress9b), which verify byte-exact answers against the
// native client's own sent answers.
#include "api/api.h"

#include "lua.h"

namespace rbxch {
namespace api {

const Options& opts(lua_State* L) {
    // setup_sandbox() stores (const Options*) as light userdata before any
    // module runs; run() keeps the Options alive for the state's lifetime.
    // (Replaces the old process-global g_opts, so run() is re-entrant.)
    lua_getfield(L, LUA_REGISTRYINDEX, "rbx.Options");
    const Options* o = (const Options*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return *o;
}

// Module entry points (one per api/*.cpp).
void register_random(lua_State* L, const Options& opts);
void register_game(lua_State* L, const Options& opts);
void register_runservice(lua_State* L, const Options& opts);
void register_usersettings(lua_State* L, const Options& opts);
void register_os(lua_State* L, const Options& opts);
void register_newproxy(lua_State* L, const Options& opts);

const Module kModules[] = {
    // kBoth = present in BOTH profiles. The Challenge profile is frozen:
    // engine surface (workspace, Instance, …) must be added as kEngineOnly.
    {"Random", &register_random, kBoth},          // Roblox PCG Random (rbxrandom.*)
    {"game", &register_game, kBoth},              // game = { JobId, GetService }
    {"RunService", &register_runservice, kBoth},  // registered into rbx.Services
    {"UserSettings", &register_usersettings, kBoth},
    {"os", &register_os, kBoth},                  // os.exit override
    {"newproxy", &register_newproxy, kBoth},
};

const size_t kModuleCount = sizeof(kModules) / sizeof(kModules[0]);

} // namespace api
} // namespace rbxch
