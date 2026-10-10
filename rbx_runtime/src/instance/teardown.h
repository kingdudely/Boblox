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
// Heap-member VALUES (NumberSequence/ColorSequence/Content) need one more
// level: their C++ storage cannot live inside GC-managed userdata memory
// (Lua may free it at any GC cycle, leaving a dangling pointer for
// teardown). They live in the per-state Slab instead; userdata holds only
// a stable index handle (a plain integer — nothing to destruct). The slab
// is cleared wholesale at teardown, so nothing leaks and nothing dangles,
// no matter when Lua frees individual userdata.
//
// Contract: after engine::destroy(env), every Instance*/Signal*/Ref obtained
// from that environment is invalid. Drop your own Refs BEFORE calling
// destroy — the teardown force-deletes regardless of the refcount.
//
// (Two environments can coexist: tracking is per lua_State, so destroying
// one never touches the other's objects.)
#pragma once

#include "instance/variant.h"

#include <cstdint>
#include <memory>
#include <vector>

struct lua_State;

namespace rbx {

enum AliveTag : int { kAliveInstance = 1, kAliveSignal = 2 };

// Heap storage for pushed value objects (see above). Indices are append-only
// and never reused, so handles stay valid for the environment's lifetime.
struct Slab {
    std::vector<std::unique_ptr<NumberSequence>> sequences;
    std::vector<std::unique_ptr<ColorSequence>> color_sequences;
    std::vector<std::unique_ptr<Content>> contents;
};

// Fetch (lazily creating) the per-state slab. The slab is deleted by
// teardown_alive — never delete it yourself.
Slab* slab(lua_State* L);

// Registry a pushed object for teardown (idempotent per pointer+state).
void track_alive(lua_State* L, void* obj, int tag);

// Force-delete every tracked object of this state. Call right BEFORE
// lua_close (never after — the pointers die with the objects).
void teardown_alive(lua_State* L);

} // namespace rbx
