// engine.h — the persistent Engine-profile environment (the game-loop
// embedder API; the future renderer drives this).
//
// The challenge path (runner.h run()) creates a FRESH state per call and
// closes it — byte-exact and re-entrant. The engine environment is the
// opposite: one state that lives across script execution + scheduler pumps,
// so spawned tasks, signals, and the instance tree persist.
//
//   auto* env = engine::create(opts);          // Engine sandbox installed
//   engine::execute(env, code, size);          // run a main chunk (persists)
//   engine::step(env, 1.0 / 60.0);             // advance the task scheduler
//   engine::destroy(env);
//
// The clock is virtual (only engine::step advances it) — deterministic tests.
//
// Ownership: destroy(env) FORCE-DELETES every engine object (Instances,
// Signals) created in that state and destructs the scheduler — after it,
// every Instance*/Signal*/Ref from this environment is invalid (Luau has no
// userdata finalizers; see instance/teardown.h). Drop your own Refs first.
#pragma once

#include "runner.h"

#include <cstddef>
#include <cstdint>
#include <vector>

struct lua_State;

namespace rbx {
class Instance;
}

namespace rbxch {
namespace engine {

struct Environment;

Environment* create(const Options& opts);
void destroy(Environment* env);

// Load + run a compiled STANDARD-Luau main chunk in the persistent state.
// The chunk runs to completion (or until it yields via task.wait inside a
// spawned thread); scheduler tasks keep living across calls. When `script` is
// given, the chunk's thread runs with that instance bound to `script` for its
// whole lifetime (including after yields), like the native client; without it
// the chunk sees the engine's globals unchanged.
Result execute(Environment& env, const void* code, size_t size,
               rbx::Instance* script = nullptr);

// Advance the scheduler clock by dt and resume everything due.
// Returns the number of task resumes performed.
int step(Environment& env, double dt);

// Current virtual time (sum of steps).
double now(const Environment& env);

// The state (for embedders that need direct Luau access).
lua_State* state(Environment& env);

// Register the compiled chunk of a replicated ModuleScript so the sandbox's
// `require` can run it. Roblox never auto-runs a ModuleScript: the client
// receives its bytecode with the world and executes it the first time some
// script requires it, caching the returned value per ModuleScript. The chunk
// must already be container-decoded and wire-standardized (the caller owns
// that, because those formats live in the client transport layer).
void set_module_source(Environment& env, const rbx::Instance* module,
                       std::vector<uint8_t> chunk);

// Point the Engine sandbox's `game` / `workspace` / GetService surface at an
// already-applied DataModel tree instead of the empty service singletons
// created at create(). Every instance in the tree is registered for engine
// teardown, so `destroy(env)` owns them exactly like instances created from
// Lua. The caller must drop its own Refs before destroy (see teardown.h).
void attach(Environment& env, rbx::Instance* datamodel);

} // namespace engine
} // namespace rbxch
