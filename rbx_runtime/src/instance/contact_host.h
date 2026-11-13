// contact_host.h — the solver supplies BasePart contact queries and events.
//
// The instance/runtime layer owns the public Lua API, but it must not link the
// client's Jolt world (the challenge profile has no solver).  The live client
// installs a backend that answers GetTouchingParts/IsGrounded and drains the
// contact transitions produced by the physics step.  With no backend, the
// runtime keeps the geometric fallback used by offline engine tests.
#pragma once

#include "instance/refcount.h"

#include <vector>

struct lua_State;

namespace rbx {

class Instance;

namespace contact {

struct Pair {
    Ref<Instance> a;
    Ref<Instance> b;
};

class Backend {
  public:
    virtual ~Backend() = default;

    // Parts whose collision geometry is in contact with `part` in the latest
    // completed physics state.  The backend deduplicates and excludes `part`.
    virtual void touching_parts(Instance* part,
                                std::vector<Instance*>* out) = 0;

    // True when `part` belongs to an assembly resting on an upward-facing
    // contact.  The character root uses CharacterVirtual's ground state.
    virtual bool grounded(Instance* part) = 0;

    // Contact transitions since the previous drain.  Each pair is reported
    // once; the dispatcher fires Touched/TouchEnded on both instances.
    virtual std::vector<Pair> take_begun() = 0;
    virtual std::vector<Pair> take_ended() = 0;
};

void set_backend(Backend* backend);
Backend* backend();

// Drain begun/ended pairs and fire the native BasePart events.  Safe to call
// with no backend or no Lua state.
void dispatch(lua_State* L);

} // namespace contact
} // namespace rbx
