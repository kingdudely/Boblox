// teardown.h — deterministic destruction of engine objects at env close.
//
// Why this exists: Luau has NO userdata finalizers — the VM ignores `__gc`
// metamethods entirely (luaC_freeall just frees object memory), so C++
// objects referenced only from Lua userdata would leak at lua_close. Every
// object that gets pushed as userdata is therefore tracked per-state in the
// registry ("rbx.Alive") and force-deleted right before lua_close
// (engine::destroy). Objects never pushed hold only references from other
// engine objects and die with them — no tracking needed.
//
// Contract: after engine::destroy(env), every Instance*/Signal*/Ref obtained
// from that environment is invalid. Drop your own Refs BEFORE calling
// destroy — the teardown force-deletes regardless of the refcount.
//
// (Two environments can coexist: tracking is per lua_State, so destroying
// one never touches the other's objects.)
#pragma once

struct lua_State;

namespace rbx {

enum AliveTag : int { kAliveInstance = 1, kAliveSignal = 2 };

// Registry a pushed object for teardown (idempotent per pointer+state).
void track_alive(lua_State* L, void* obj, int tag);

// Force-delete every tracked object of this state. Call right BEFORE
// lua_close (never after — the pointers die with the objects).
void teardown_alive(lua_State* L);

} // namespace rbx
