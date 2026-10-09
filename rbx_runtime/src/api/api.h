// api.h — the Roblox API module contract for the rbx_runtime environment.
//
// Every Roblox-facing API surface (game, RunService, UserSettings, Random, …)
// is one module: a translation unit that exposes a register_* entry point and
// is listed in registry.cpp. Adding an API is:
//   1. create api/<name>.cpp with  void register_<name>(lua_State*, const Options&)
//   2. add its entry to kModules in registry.cpp (registration order = list
//      order) with the right profile tag: kEngineOnly for new engine surface,
//      kBoth ONLY if the native challenge client truly exposes it (the
//      Challenge profile is frozen — tests/unit/unit_profile.cpp pins it)
//   3. add the file to challenge_core in CMakeLists.txt
//
// Why an explicit list instead of static self-registration: challenge_core is
// a STATIC library — translation units whose symbols nothing references are
// silently dropped by the linker, which would make modules vanish only in
// some link configurations. The explicit list is also greppable ("what APIs
// exist?") and makes registration order (behavior-visible) obvious.
//
// Shared environment rules live here, not in the modules:
//   * opts(L)         — sandbox Options for the current state (set by
//                       setup_sandbox before any module runs; replaces the
//                       old process-global g_opts so run() is re-entrant).
//   * register_service / push_service — the rbx.Services table that backs
//                       game:GetService; service modules register themselves
//                       by name and game.cpp dispatches through this table.
#pragma once

#include "runner.h" // rbxch::Options

#include <cstddef>

struct lua_State;

namespace rbxch {
namespace api {

// Which profiles a module is exposed in (bitmask over rbxch::Profile).
// The Challenge profile is FROZEN: every entry on it must be tagged kBoth —
// Engine-only additions (workspace, Instance, …) get kEngineOnly, never
// kBoth, so the solve path's global surface can never drift (unit_profile
// pins the challenge list).
constexpr uint8_t kChallenge = 1u << 0;
constexpr uint8_t kEngine = 1u << 1;
constexpr uint8_t kBoth = kChallenge | kEngine;
constexpr uint8_t kEngineOnly = kEngine;

// Bit for a profile in a Module::profiles mask.
inline uint8_t profile_bit(Profile p) {
    return p == Profile::Engine ? kEngine : kChallenge;
}

// One API module. register_globals is the entry point called (in list order,
// when the module's profile mask matches Options::profile) while the
// environment is installed into a fresh lua_State.
struct Module {
    const char* name; // diagnostics + docs; e.g. "RunService"
    void (*register_globals)(lua_State* L, const Options& opts);
    uint8_t profiles; // kBoth | kEngineOnly (see constants above)
};

// The ordered module list — defined in registry.cpp. Registration order is
// behavior-visible (later modules can observe earlier globals), so keep it
// equal to the historical setup_sandbox() order unless intended otherwise.
extern const Module kModules[];
extern const size_t kModuleCount;

// Sandbox options for L. Valid inside every API callback; setup_sandbox()
// stores a pointer to the caller's Options in the Lua registry (key
// "rbx.Options") before any module runs, and run() keeps those Options alive
// for the lifetime of the state.
const Options& opts(lua_State* L);

// Service registry backing game:GetService(name).
//   register_service(L, name) — consumes the service table at the top of the
//                               stack, storing it in rbx.Services[name].
//   push_service(L, name)     — pushes rbx.Services[name] and returns true if
//                               the service exists; pushes nothing and returns
//                               false otherwise.
void register_service(lua_State* L, const char* name);
bool push_service(lua_State* L, const char* name);

} // namespace api
} // namespace rbxch
