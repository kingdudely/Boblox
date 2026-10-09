// scheduler.h — the single-threaded cooperative scheduler (task library).
//
// Deterministic virtual clock: `now` only advances via step(dt) from the
// embedder (engine::step) — tests are exact, no wall-clock dependence.
//
//   task.spawn(f, ...)   new thread, resumed IMMEDIATELY (Roblox semantics);
//                        a task.wait() inside it yields into the scheduler.
//   task.defer(f, ...)   new thread, started at the next pump.
//   task.wait([t])       yield the current thread until now+t (returns t).
//   task.delay(t, f, …)  new thread, started at now+t.
//
// The Scheduler lives in the Lua registry ("rbx.Scheduler", full userdata)
// so every task.* call and engine::step find the same instance per state.
#pragma once

#include "instance/refs.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

struct lua_State;

namespace rbx {

struct Scheduler {
    double now = 0.0;

    // resume `thread` (already prepared on the given stack) NOW; if it
    // yields, it becomes schedulable (task.wait). thread_ref is the refs-key
    // anchoring the thread; consumed here (freed when the thread finishes).
    void resume_now(lua_State* L, lua_State* thread, int thread_ref, int nargs_on_thread);
    // schedule an already-prepared thread to start at now+delay.
    void defer_start(lua_State* L, lua_State* thread, int thread_ref, double delay);
    // yield the CURRENT thread (the lua_State of a running task.* call)
    // until now+t; pushes the resume value (t). Errors outside a schedulable
    // thread.
    void yield_current(lua_State* L, double t);
    // Schedule an already-anchored thread (a signal Wait-er) to resume at
    // the next pump, with no resume values.
    void defer_start_thread(lua_State* L, int thread_ref);

    // Advance the clock by dt and resume everything due (plus deferred
    // starts). Resumed threads that yield re-enter the queue. Returns the
    // number of resumes performed (capped — see kMaxResumesPerStep).
    int pump(lua_State* L, double dt);

    // Last scheduler-caused runtime error (resume errors are not Lua errors
    // of the caller; they are collected here for diagnostics/tests).
    std::string last_error;

  private:
    struct Entry {
        double due = 0.0;
        uint64_t seq = 0; // tie-break for stable order
        int thread_ref = 0;
        bool has_value = false; // resume with `value` (task.wait returns t)
        double value = 0.0;
    };
    struct HeapLater { // min-heap by (due, seq)
        bool operator()(const Entry& a, const Entry& b) const {
            return a.due != b.due ? a.due > b.due : a.seq > b.seq;
        }
    };
    std::vector<Entry> heap_;
    std::deque<Entry> deferred_;
    uint64_t next_seq_ = 1;

    void push_heap(double due, int thread_ref);
    void resume_entry(lua_State* L, const Entry& e);
};

// Fetch (lazily creating) the per-state scheduler.
Scheduler* scheduler(lua_State* L);

// Install the `task` global (spawn/defer/wait/delay). Engine profile only.
void create_task_library(lua_State* L);

} // namespace rbx
