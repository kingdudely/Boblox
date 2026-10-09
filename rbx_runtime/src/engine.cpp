// engine.cpp — see engine.h.
#include "engine.h"

#include "instance/scheduler.h"
#include "sandbox.h"

#include "lua.h"
#include "lualib.h"

#include "Luau/Common.h"

// Production bytecode (and our parity with runner.cpp) needs CALLFB on.
LUAU_FASTFLAG(LuauCallFeedback)

namespace rbxch {
namespace engine {

struct Environment {
    lua_State* L = nullptr;
    Options opts;
};

Environment* create(const Options& opts) {
    auto* env = new Environment();
    env->opts = opts;
    env->opts.profile = Profile::Engine; // always the engine surface
    FFlag::LuauCallFeedback.value = true; // same as runner.cpp (CALLFB)
    env->L = luaL_newstate();
    setup_sandbox(env->L, env->opts); // installs libs + all matching modules
    return env;
}

void destroy(Environment* env) {
    if (!env)
        return;
    if (env->L)
        lua_close(env->L);
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
    if (lua_pcall(env.L, 0, 0, 0) != 0) {
        r.status = Status::RuntimeError;
        r.error = lua_tostring(env.L, -1) ? lua_tostring(env.L, -1) : "runtime error";
        lua_pop(env.L, 1);
        return r;
    }
    r.status = Status::Ok;
    return r;
}

int step(Environment& env, double dt) {
    return rbx::scheduler(env.L)->pump(env.L, dt);
}

double now(const Environment& env) {
    return rbx::scheduler(env.L)->now;
}

lua_State* state(Environment& env) {
    return env.L;
}

} // namespace engine
} // namespace rbxch
