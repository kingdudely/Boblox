// frame_clock.h — the client frame clock shared by RunService and engine::step.
//
// Frame events belong to the VM side (RunService instances own their signals),
// but BindToRenderStep callbacks are not signals: they are an ordered render
// pipeline keyed by name and priority. They live in the Lua registry so they
// die with the state and need no C++ teardown.
#pragma once

struct lua_State;

namespace rbx {

// Registry key holding the render-step binding array.
inline constexpr const char* kRenderStepsKey = "rbx.RenderSteps";

// Invoke every live binding in ascending priority order (ties: bind order)
// with one deltaTime argument. Handler errors are swallowed exactly like
// signal handlers (Roblox logs and keeps rendering).
void run_render_steps(lua_State* L, double dt);

// Remove all bindings with this name (UnbindFromRenderStep).
void clear_render_steps(lua_State* L, const char* name);

} // namespace rbx
