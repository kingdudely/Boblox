// signal.h — RBXScriptSignal: Connect / Once / Wait + C++ Fire.
//
// * Connect(fn)     -> Connection{ Connected, Disconnect() }
// * Once(fn)        -> auto-disconnects after the first Fire
// * Wait()          -> yields the current scheduler thread until the next
//                      Fire (resumes at the next pump)
// * Fire(args)      (C++ only): calls connected handlers synchronously in
//                      connection order; Wait-ers are queued on the
//                      scheduler (deterministic single-threaded pump).
//
// Handler functions and waiter threads are anchored in the state's refs
// table (refs.h) — they die with the lua_State, and are unref'd on
// Disconnect / Once-fire / resume.
#pragma once

#include "instance/refcount.h"

#include "instance/variant.h"

#include <cstdint>
#include <string>
#include <vector>

struct lua_State;

namespace rbx {

class Signal : public RefCounted {
  public:
    explicit Signal(std::string name) : name_(std::move(name)) {
    }

    const std::string& name() const {
        return name_;
    }

    // Invoke handlers (in connection order); Wait-ers are scheduled to
    // resume at the next pump. args are pushed to each handler.
    void fire(lua_State* L, const std::vector<Variant>& args = {});

    // Lua-driven mutations (used by the signal methods).
    uint64_t connect_ref(lua_State* L, int fn_idx, bool once); // anchors fn
    void add_waiter(int thread_ref);                            // anchors thread
    void disconnect(lua_State* L, uint64_t id);
    bool connected(uint64_t id) const;
    // Consume a waiter entry WITHOUT freeing its ref (ownership transfers to
    // the scheduler, which frees it when the thread is resumed).
    int take_waiter(uint64_t id);

    // -- Lua userdata glue ------------------------------------------------------
    static void push(lua_State* L, Signal* sig); // strong ref for the userdata
    static Signal* check(lua_State* L, int idx);
    static void create_metatable(lua_State* L); // "RBXScriptSignal"

  private:
    std::string name_;
    struct Entry {
        uint64_t id = 0;
        int fn_ref = 0;       // 0 = none; >0 handler anchored in refs
        int waiter_ref = 0;   // 0 = none; >0 thread anchored in refs
        bool once = false;
        bool connected = true;
    };
    std::vector<Entry> entries_; // connection order
    uint64_t next_id_ = 1;
};

} // namespace rbx
