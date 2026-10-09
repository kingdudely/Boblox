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
#pragma once

#include "runner.h"

#include <cstddef>

struct lua_State;

namespace rbxch {
namespace engine {

struct Environment;

Environment* create(const Options& opts);
void destroy(Environment* env);

// Load + run a compiled STANDARD-Luau main chunk in the persistent state.
// The chunk runs to completion (or until it yields via task.wait inside a
// spawned thread); scheduler tasks keep living across calls.
Result execute(Environment& env, const void* code, size_t size);

// Advance the scheduler clock by dt and resume everything due.
// Returns the number of task resumes performed.
int step(Environment& env, double dt);

// Current virtual time (sum of steps).
double now(const Environment& env);

// The state (for embedders that need direct Luau access).
lua_State* state(Environment& env);

} // namespace engine
} // namespace rbxch
