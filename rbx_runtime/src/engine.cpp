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
#include "instance/tween.h"
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
    // Bytecode of every replicated ModuleScript, keyed by the module instance.
    // Owned here because the Instance tree is owned here: require() reads it
    // once, on first use, and the Lua side caches the returned value.
    std::map<const rbx::Instance*, std::vector<uint8_t>> module_sources;
};

namespace {

constexpr const char* kModuleCacheKey = "rbx.ModuleCache";
constexpr const char* kEnvironmentKey = "rbx.EngineEnvironment";

// require(moduleScript) — the native client's module loader, as a global.
//
// Roblox semantics this reproduces: only a ModuleScript is requireable; the
// chunk runs in the shared environment with `script` bound to the module; the
// return value is cached per ModuleScript so a second require neither re-runs
// the chunk nor reloads it. The value is looked up through the registry cache
// table before the bytecode map, so the cache survives even if the module's
// source is dropped later.
// Push a per-chunk environment whose __index is the shared globals and whose
// `script` field names the executing instance. Roblox gives every script its
// own environment table, so a global write inside a script is not visible to
// its siblings, and GETIMPORT compiles against that environment. Installing
// this on the chunk function (not on the state) is what keeps `script` alive
// across yields.
void push_script_env(lua_State* L, rbx::Instance* script) {
    lua_newtable(L);                    // env
    lua_newtable(L);                    // mt
    lua_pushvalue(L, LUA_GLOBALSINDEX); // shared globals
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    rbx::Instance::push(L, script);
    lua_setfield(L, -2, "script");
}

int m_require(lua_State* L) {
    rbx::Instance* mod = rbx::Instance::check(L, 1);
    if (!mod->is_a("ModuleScript")) {
        luaL_error(L, "require() expects a ModuleScript, got %s",
                   mod->class_name().c_str());
        return 0;
    }
    lua_getfield(L, LUA_REGISTRYINDEX, kEnvironmentKey);
    Environment* env = static_cast<Environment*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!env) {
        luaL_error(L, "require() outside an engine environment");
        return 0;
    }

    // [arg, cache]
    lua_getfield(L, LUA_REGISTRYINDEX, kModuleCacheKey);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, kModuleCacheKey);
    }
    lua_pushlightuserdata(L, mod);
    lua_rawget(L, -2); // [arg, cache, cached]
    // Nil means "not loaded yet". A `false` marker means the module is on the
    // stack right now: the native reports a cycle instead of recursing.
    if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
        luaL_error(L, "cyclic require of module '%s'", mod->name().c_str());
        return 0;
    }
    if (!lua_isnil(L, -1))
        return 1; // [arg, cache, cached] — Lua keeps the top value
    lua_pop(L, 1); // [arg, cache]

    const auto it = env->module_sources.find(mod);
    if (it == env->module_sources.end()) {
        luaL_error(L, "module '%s' has no loaded chunk", mod->name().c_str());
        return 0;
    }

    lua_pushlightuserdata(L, mod);
    lua_pushboolean(L, 0);
    lua_rawset(L, -3); // cache[module] = false (loading)

    // The module runs with its own environment, but Luau resolves imports at
    // load against the *thread's* globals, so swap the caller thread's
    // environment for the module's across load + call, then restore it. The
    // caller never executes while the module does, so this is not observable.
    lua_pushthread(L);
    lua_getfenv(L, -1);
    int saved_env = rbx::ref_new(L); // anchors + pops caller env
    lua_pop(L, 1);                         // thread
    push_script_env(L, mod);
    lua_pushthread(L);
    lua_insert(L, -2);
    lua_setfenv(L, -2); // caller thread env = module env
    lua_pop(L, 1);      // thread

    const std::string chunkname = "=" + mod->name();
    if (luau_load(L, chunkname.c_str(), (const char*)it->second.data(),
                  it->second.size(), 0) != 0) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "load error";
        lua_pop(L, 1);
        lua_pushlightuserdata(L, mod);
        lua_pushnil(L);
        lua_rawset(L, -3);
        luaL_error(L, "%s", err.c_str());
        return 0;
    }
    const int st = lua_pcall(L, 0, 1, 0); // [arg, cache, result|error]
    // Restore the caller's environment whatever the module did.
    lua_rawgeti(L, LUA_REGISTRYINDEX, saved_env);
    lua_pushthread(L);
    lua_insert(L, -2);
    lua_setfenv(L, -2);
    lua_pop(L, 1); // thread
    rbx::ref_free(L, saved_env);
    if (st != 0) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "runtime error";
        lua_pop(L, 1); // error
        lua_pushlightuserdata(L, mod);
        lua_pushnil(L);
        lua_rawset(L, -3);
        luaL_error(L, "%s", err.c_str());
        return 0;
    }
    lua_pushlightuserdata(L, mod);
    lua_pushvalue(L, -2);
    lua_rawset(L, -4); // cache[module] = result
    lua_remove(L, 2);  // [arg, result]
    return 1;
}

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
    // The module loader is part of the client's global surface, not a service.
    lua_pushlightuserdata(env->L, env);
    lua_setfield(env->L, LUA_REGISTRYINDEX, kEnvironmentKey);
    lua_pushcfunction(env->L, m_require, "require");
    lua_setglobal(env->L, "require");
    return env;
}

void set_module_source(Environment& env, const rbx::Instance* module,
                       std::vector<uint8_t> chunk) {
    if (!module)
        return;
    env.module_sources[module] = std::move(chunk);
}

void destroy(Environment* env) {
    if (!env)
        return;
    rbx::dump_stub_stats();
    if (env->L) {
        // Luau never runs userdata finalizers — free the engine-side C++
        // objects ourselves, right before the state memory goes away.
        //
        // Order matters (live MM2 heap-use-after-free, 2026-11-06): the
        // scheduler's parked WaitForChild threads hold Ref<Instance>, and
        // teardown_alive force-deletes every instance. Pin first so the
        // scheduler's releases cannot delete (and be deleted again), then
        // destroy the scheduler, then force-delete the objects.
        rbx::pin_alive(env->L);
        if (env->sched)
            env->sched->~Scheduler(); // SchedUD shell is freed by lua_close
        rbx::teardown_alive(env->L);
        lua_close(env->L);
    }
    delete env;
}

Result execute(Environment& env, const void* code, size_t size,
               rbx::Instance* script) {
    Result r;
    // A real LocalScript's main chunk is a coroutine: it may yield from
    // Signal:Wait()/WaitForChild()/task.wait() and resume when the world
    // changes. Running it on the main state would make every such call an
    // error and would serialize scripts that should run concurrently.
    lua_State* thread = lua_newthread(env.L); // [thread]
    if (script) {
        // `script` is per-chunk, not a global: a chunk that yields and resumes
        // later must still see its own instance. Setting the global before
        // each execute leaked across concurrent scripts (Adopt Me's loader
        // reads script.AwaitPersistentModels at line 16, after yielding, and
        // saw nil or another chunk's value).
        push_script_env(env.L, script);
        lua_setfenv(env.L, -2); // thread environment = the script's env
    }
    // Load ON the script thread, not on the main state: Luau resolves
    // GETIMPORT against the loading thread's globals and stores that table as
    // the chunk's environment, so `script` has to be installed before the
    // chunk is loaded or every compiled global access resolves to nil.
    if (luau_load(thread, "=script", (const char*)code, size, 0) != 0) {
        r.status = Status::LoadFailed;
        r.error = lua_tostring(thread, -1) ? lua_tostring(thread, -1)
                                           : "load error";
        lua_pop(thread, 1);
        lua_pop(env.L, 1); // thread
        return r;
    }
    const int thread_ref = rbx::ref_new(env.L); // anchors thread, pops it
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
    // TweenService playback advances on the same frame delta (headless runs
    // animate identically: no renderer is involved).
    rbx::tween_step(env.L, dt);
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
