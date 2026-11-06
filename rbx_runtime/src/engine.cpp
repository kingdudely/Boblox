// engine.cpp — see engine.h.
#include "engine.h"

#include "api/api.h"
#include "instance/class_registry.h"
#include "instance/frame_clock.h"
#include "instance/instance.h"
#include "instance/refs.h"
#include "instance/scheduler.h"
#include "instance/signal.h"
#include "instance/teardown.h"
#include "sandbox.h"

#include "lua.h"
#include "lualib.h"

#include "Luau/Common.h"

#include <vector>

// Production bytecode (and our parity with runner.cpp) needs CALLFB on.
LUAU_FASTFLAG(LuauCallFeedback)

namespace rbxch {
namespace engine {

struct Environment {
    lua_State* L = nullptr;
    Options opts;
    rbx::Scheduler* sched = nullptr; // materialized at create; see destroy()
    // Client RunService (weak: the service registry owns it). Refreshed after
    // attach because a replicated DataModel may replace the service table.
    rbx::Instance* run_service = nullptr;
};

namespace {

rbx::Instance* find_run_service(lua_State* L) {
    if (!api::push_service(L, "RunService"))
        return nullptr;
    rbx::Instance* rs = rbx::Instance::get(L, -1);
    lua_pop(L, 1);
    return rs;
}

void fire_frame_event(rbx::Instance* rs, const char* name, lua_State* L,
                      const std::vector<rbx::Variant>& args) {
    if (rs && rbx::find_event(rs->class_info(), name))
        rs->event_signal(name)->fire(L, args);
}

} // namespace

Environment* create(const Options& opts) {
    auto* env = new Environment();
    env->opts = opts;
    env->opts.profile = Profile::Engine; // always the engine surface
    FFlag::LuauCallFeedback.value = true; // same as runner.cpp (CALLFB)
    env->L = luaL_newstate();
    setup_sandbox(env->L, env->opts); // installs libs + all matching modules
    // Materialize the scheduler up front: Luau has no userdata finalizers, so
    // destroy() must be able to run its C++ destructor explicitly.
    env->sched = rbx::scheduler(env->L);
    env->run_service = find_run_service(env->L);
    return env;
}

void destroy(Environment* env) {
    if (!env)
        return;
    if (env->L) {
        // Luau never runs userdata finalizers — free the engine-side C++
        // objects ourselves, right before the state memory goes away.
        rbx::teardown_alive(env->L);
        if (env->sched)
            env->sched->~Scheduler(); // SchedUD shell is freed by lua_close
        lua_close(env->L);
    }
    delete env;
}

Result execute(Environment& env, const void* code, size_t size) {
    Result r;
    if (luau_load(env.L, "=script", (const char*)code, size, 0) != 0) {
        r.status = Status::LoadFailed;
        r.error = lua_tostring(env.L, -1) ? lua_tostring(env.L, -1) : "load error";
        lua_pop(env.L, 1);
        return r;
    }

    // A real LocalScript's main chunk is a coroutine: it may yield from
    // Signal:Wait()/WaitForChild()/task.wait() and resume when the world
    // changes. Running it on the main state would make every such call an
    // error and would serialize scripts that should run concurrently.
    lua_State* thread = lua_newthread(env.L); // [chunk, thread]
    const int thread_ref = rbx::ref_new(env.L); // anchors thread, pops it
    lua_xmove(env.L, thread, 1);               // [thread: chunk]
    const int status = lua_resume(thread, env.L, 0);
    if (status == LUA_YIELD) {
        // The yielded thread is anchored by its waiter (Signal/task), not by
        // this spawn ref; drop the spawn anchor.
        int ref = thread_ref;
        rbx::ref_free(env.L, ref);
        r.status = Status::Ok;
        return r;
    }
    if (status != 0) {
        r.status = Status::RuntimeError;
        const char* msg = lua_tostring(thread, -1);
        r.error = msg ? msg : "runtime error";
        lua_pop(thread, 1);
        int ref = thread_ref;
        rbx::ref_free(env.L, ref);
        return r;
    }
    int ref = thread_ref;
    rbx::ref_free(env.L, ref);
    r.status = Status::Ok;
    return r;
}

int step(Environment& env, double dt) {
    const int resumed = env.sched->pump(env.L, dt);
    if (!env.run_service)
        env.run_service = find_run_service(env.L);
    if (!env.run_service)
        return resumed;

    // Client frame order: render-step bindings, RenderStepped, then pre-render
    // and the post-frame signals. Only enabled client events cost anything.
    rbx::run_render_steps(env.L, dt);
    const std::vector<rbx::Variant> frame{rbx::Variant(dt)};
    fire_frame_event(env.run_service, "RenderStepped", env.L, frame);
    fire_frame_event(env.run_service, "PreRender", env.L, frame);
    fire_frame_event(env.run_service, "PreAnimation", env.L, frame);
    fire_frame_event(env.run_service, "Heartbeat", env.L, frame);
    fire_frame_event(env.run_service, "PostSimulation", env.L, frame);
    fire_frame_event(env.run_service, "Stepped", env.L,
                     {rbx::Variant(env.sched->now), rbx::Variant(dt)});
    return resumed;
}

double now(const Environment& env) {
    return env.sched->now;
}

lua_State* state(Environment& env) {
    return env.L;
}

void attach(Environment& env, rbx::Instance* datamodel) {
    api::attach_datamodel(env.L, datamodel);
    env.run_service = find_run_service(env.L);
}

} // namespace engine
} // namespace rbxch
