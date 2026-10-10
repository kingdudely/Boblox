// scheduler.cpp — cooperative scheduler + the `task` global library.
#include "instance/scheduler.h"

#include "instance/instance.h" // resume-with-instance (WaitForChild hit)

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rbx {

namespace {

// Safety cap: a task.wait(0) loop must not hang a single step forever.
constexpr int kMaxResumesPerStep = 100000;

struct SchedUD {
    Scheduler s;
    // No __gc: Luau's VM never invokes userdata finalizers (see
    // instance/teardown.h) — the Scheduler is explicitly destructed by
    // engine::destroy right before lua_close.
};

// task.* ----------------------------------------------------------------------------

// Common: move fn at `fn_idx` plus args [fn_idx+1..top] onto a fresh thread.
// Returns the anchored thread ref; the thread is left on L's stack top? No —
// fully handed to the scheduler (anchored in refs).
int spawn_thread(lua_State* L, int fn_idx, Scheduler* s, double delay, bool immediate) {
    luaL_checktype(L, fn_idx, LUA_TFUNCTION);
    const int top = lua_gettop(L);
    lua_State* th = lua_newthread(L); // ... thread
    const int th_ref = ref_new(L);    // anchor; pops the thread
    for (int i = fn_idx; i <= top; i++) {
        lua_pushvalue(L, i);
        lua_xmove(L, th, 1); // fn + args live on the new thread's stack
    }
    // lua_resume takes the number of ARGUMENTS (the function itself, copied
    // onto the thread first, is not counted) — [fn, args...], narg = #args.
    const int nargs = top - fn_idx;
    if (immediate)
        s->resume_now(L, th, th_ref, nargs);
    else
        s->defer_start_thread(L, th_ref, delay);
    return 0;
}

int task_spawn(lua_State* L) {
    return spawn_thread(L, 1, scheduler(L), 0.0, true);
}

int task_defer(lua_State* L) {
    return spawn_thread(L, 1, scheduler(L), 0.0, false);
}

int task_delay(lua_State* L) {
    const double t = luaL_checknumber(L, 1);
    return spawn_thread(L, 2, scheduler(L), t, false);
}

int task_wait(lua_State* L) {
    const double t = luaL_optnumber(L, 1, 0.0);
    Scheduler* s = scheduler(L);
    s->yield_current(L, t); // anchors + queues; errors outside a task thread
    return lua_yield(L, 0); // resumes with the wait duration
}

} // namespace

Scheduler::~Scheduler() = default; // see header: needs complete Instance

Scheduler* scheduler(lua_State* L) {    lua_getfield(L, LUA_REGISTRYINDEX, "rbx.Scheduler");
    if (lua_isuserdata(L, -1)) {
        auto* ud = static_cast<SchedUD*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        return &ud->s;
    }
    lua_pop(L, 1);
    auto* ud = static_cast<SchedUD*>(lua_newuserdata(L, sizeof(SchedUD)));
    new (&ud->s) Scheduler();
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, "rbx.Scheduler"); // anchor per state
    lua_pop(L, 1); // leave the stack exactly as we found it (spawn's gettop!)
    return &ud->s;
}

// ---- core ------------------------------------------------------------------------

void Scheduler::push_heap(double due, int thread_ref) {
    heap_.push_back(Entry{due, next_seq_++, thread_ref, false, 0.0});
    std::push_heap(heap_.begin(), heap_.end(), HeapLater{});
}

void Scheduler::resume_now(lua_State* L, lua_State* thread, int thread_ref, int nargs) {
    const int st = lua_resume(thread, L, nargs);
    if (st == LUA_YIELD) {
        // task.wait inside re-anchored + queued itself; drop the spawn anchor.
        ref_free(L, thread_ref);
        return;
    }
    if (st != 0) {
        const char* msg = lua_tostring(thread, -1);
        last_error = msg ? msg : "unknown task error";
        lua_pop(thread, 1);
    }
    ref_free(L, thread_ref);
}

void Scheduler::defer_start_thread(lua_State* L, int thread_ref, double delay) {
    (void)L; // anchored already; resumed from the deferred queue
    deferred_.push_back(Entry{now + delay, next_seq_++, thread_ref, false, 0.0});
}

void Scheduler::wait_child(lua_State* L, Instance* parent, const std::string& name,
                           double timeout) {
    if (lua_pushthread(L) == 1) { // cannot yield the main thread
        lua_pop(L, 1);
        luaL_error(L, "WaitForChild must run inside a task.spawn/defer/delay thread");
        return;
    }
    const int th_ref = ref_new(L); // anchor the waiter thread (transferred on wake)
    Waiter w;
    w.thread_ref = th_ref;
    w.parent = Ref<Instance>(parent);
    w.name = name;
    w.deadline = std::isinf(timeout) ? std::numeric_limits<double>::infinity()
                                     : now + timeout;
    waiters_.push_back(std::move(w));
    // NOTE: the caller yields (return lua_yield); the resume carries nil
    // (timeout) or the child (notify). No loop — Luau has no continuations.
}

void Scheduler::notify_child_added(lua_State* L, Instance* parent, Instance* child) {
    (void)L; // matching only; resuming happens at the next pump
    for (auto it = waiters_.begin(); it != waiters_.end();) {
        if (it->parent.get() == parent && it->name == child->name()) {
            Entry e{now, next_seq_++, it->thread_ref, false, 0.0};
            e.has_inst = true;
            e.value_inst = Ref<Instance>(child);
            it->thread_ref = 0; // ownership moved (ref_free skips <= 0)
            deferred_.push_back(std::move(e));
            it = waiters_.erase(it);
        } else {
            ++it;
        }
    }
}

void Scheduler::yield_current(lua_State* L, double t) {
    if (lua_pushthread(L) == 1) { // cannot yield the main thread
        lua_pop(L, 1);
        luaL_error(L, "task.wait must run inside a task.spawn/defer/delay thread");
        return;
    }
    const int th_ref = ref_new(L); // anchor the yielded thread
    Entry e{now + t, next_seq_++, th_ref, true, t};
    heap_.push_back(e);
    std::push_heap(heap_.begin(), heap_.end(), HeapLater{});
}

void Scheduler::resume_entry(lua_State* L, const Entry& e) {
    ref_push(L, e.thread_ref); // ... thread
    auto* th = lua_tothread(L, -1);
    lua_pop(L, 1);
    if (!th) { // defensive: never resume garbage through a stale ref
        last_error = "scheduler entry references a dead thread";
        return;
    }
    if (e.has_inst)
        Instance::push(th, e.value_inst.get());
    else if (e.has_value)
        lua_pushnumber(th, e.value);
    const int st = lua_resume(th, L, (e.has_value || e.has_inst) ? 1 : 0);
    int ref = e.thread_ref;
    if (st != 0 && st != LUA_YIELD) {
        const char* msg = lua_tostring(th, -1);
        last_error = msg ? msg : "unknown task error";
        lua_pop(th, 1);
    }
    ref_free(L, ref); // yielded threads re-anchored themselves
}

int Scheduler::pump(lua_State* L, double dt) {
    // Discrete-event drain: process everything due in [now, now+dt], advancing
    // the clock to each event's due as we go. Re-yields therefore anchor to
    // WAKE time (not step-end): chained short sleeps experience exact sim
    // time instead of one wake per step (time dilation). Each step still
    // advances the clock by exactly dt.
    const double end = now + dt;
    int resumes = 0;
    while (resumes < kMaxResumesPerStep) {
        // Waiter deadlines first: expired WaitForChild parks resume with nil.
        // (Deterministic order: timeouts precede same-instant dues.)
        for (auto it = waiters_.begin(); it != waiters_.end();) {
            if (it->deadline <= end) {
                Entry e{now, next_seq_++, it->thread_ref, false, 0.0};
                it->thread_ref = 0; // ownership moved (ref_free skips <= 0)
                deferred_.push_back(std::move(e));
                it = waiters_.erase(it);
            } else {
                ++it;
            }
        }
        // Earliest due across both queues (deferred wins ties — matches the
        // historical "deferred first" priority for same-instant work).
        const bool have_def =
            !deferred_.empty() && deferred_.front().due <= end;
        const bool have_heap = !heap_.empty() && heap_.front().due <= end;
        if (!have_def && !have_heap)
            break;
        Entry e{};
        if (have_def && (!have_heap || deferred_.front().due <= heap_.front().due)) {
            e = deferred_.front();
            deferred_.pop_front();
        } else {
            e = heap_.front();
            std::pop_heap(heap_.begin(), heap_.end(), HeapLater{});
            heap_.pop_back();
        }
        if (e.due > now)
            now = e.due;
        resume_entry(L, e);
        resumes++;
    }
    now = end;
    return resumes;
}

void create_task_library(lua_State* L) {
    lua_newtable(L);
    lua_pushcfunction(L, task_spawn, "spawn");
    lua_setfield(L, -2, "spawn");
    lua_pushcfunction(L, task_defer, "defer");
    lua_setfield(L, -2, "defer");
    lua_pushcfunction(L, task_wait, "wait");
    lua_setfield(L, -2, "wait");
    lua_pushcfunction(L, task_delay, "delay");
    lua_setfield(L, -2, "delay");
    lua_setglobal(L, "task");
}

} // namespace rbx
