// scheduler.cpp — cooperative scheduler + the `task` global library.
#include "instance/scheduler.h"

#include "lua.h"
#include "lualib.h"

#include <algorithm>

namespace rbx {

namespace {

constexpr const char* kSchedMT = "rbx.Scheduler";
// Safety cap: a task.wait(0) loop must not hang a single step forever.
constexpr int kMaxResumesPerStep = 100000;

struct SchedUD {
    Scheduler s;
};

int sched_gc(lua_State* L) {
    static_cast<SchedUD*>(lua_touserdata(L, 1))->s.~Scheduler();
    return 0;
}

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
        s->defer_start(L, th, th_ref, delay);
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

Scheduler* scheduler(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "rbx.Scheduler");
    if (lua_isuserdata(L, -1)) {
        auto* ud = static_cast<SchedUD*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        return &ud->s;
    }
    lua_pop(L, 1);
    auto* ud = static_cast<SchedUD*>(lua_newuserdata(L, sizeof(SchedUD)));
    new (&ud->s) Scheduler();
    if (luaL_newmetatable(L, kSchedMT)) {
        lua_pushcfunction(L, sched_gc, "__gc");
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
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

void Scheduler::defer_start(lua_State* L, lua_State* thread, int thread_ref, double delay) {
    (void)thread; // already prepared; resumed from the heap/deferred queue
    push_heap(now + delay, thread_ref);
}

void Scheduler::defer_start_thread(lua_State* L, int thread_ref) {
    deferred_.push_back(Entry{now, next_seq_++, thread_ref, false, 0.0});
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
    if (e.has_value)
        lua_pushnumber(th, e.value);
    const int st = lua_resume(th, L, e.has_value ? 1 : 0);
    int ref = e.thread_ref;
    if (st != 0 && st != LUA_YIELD) {
        const char* msg = lua_tostring(th, -1);
        last_error = msg ? msg : "unknown task error";
        lua_pop(th, 1);
    }
    ref_free(L, ref); // yielded threads re-anchored themselves
}

int Scheduler::pump(lua_State* L, double dt) {
    now += dt;
    int resumes = 0;
    while (resumes < kMaxResumesPerStep) {
        Entry e{};
        if (!deferred_.empty()) { // defer: next resumption point, before timed
            e = deferred_.front();
            deferred_.pop_front();
        } else if (!heap_.empty() && heap_.front().due <= now) {
            e = heap_.front();
            std::pop_heap(heap_.begin(), heap_.end(), HeapLater{});
            heap_.pop_back();
        } else {
            break;
        }
        resume_entry(L, e);
        resumes++;
    }
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
