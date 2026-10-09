// runner.cpp — program EXECUTION only: load -> call -> answer (see runner.h).
//
// The Roblox environment lives elsewhere:
//   sandbox.cpp      — installs the environment (openlibs + API modules)
//   api/*.cpp        — the API modules themselves (game, RunService, …)
//   api/registry.cpp — ordered module list (THE file to edit to add an API)
#include "runner.h"

#include "sandbox.h"

#include "lua.h"
#include "lualib.h"

#include "Luau/Common.h"

#include <cstdlib>
#include <string>

// Roblox's production bytecode is compiled with the LuauCallFeedback feature
// enabled, which emits LOP_CALLFB instructions after namecalls. The upstream VM
// defaults this flag to false, which asserts (debug) or miscomputes (release)
// on such bytecode. Flip it on for the lifetime of this runner.
LUAU_FASTFLAG(LuauCallFeedback)

namespace {

// Native truncation of the returned double to the 32-bit answer wraps modulo
// 2^32 (same observed behavior as the Random seed cast: `(unsigned int)(int)v`
// on a value >= 2^31 keeps the low 32 bits).
uint32_t d2ans(double v) {
    if (!(v >= -9007199254740992.0 && v <= 9007199254740992.0))
        return 0x80000000u;
    int64_t t = (int64_t)v;
    return (uint32_t)t;
}

} // namespace

namespace rbxch {

Result run(const void* code, size_t size, const Options& opts) {
    Result res;
    Options o = opts; // stable copy: sandbox options outlive setup_sandbox()

    lua_State* L = luaL_newstate();
    FFlag::LuauCallFeedback.value = true; // Roblox production bytecode uses CALLFB
    setup_sandbox(L, o);

    int loadres = luau_load(L, "=challenge", (const char*)code, size, 0);
    if (loadres != 0) {
        const char* err = lua_tostring(L, -1);
        res.status = Status::LoadFailed;
        res.error = err ? err : "(?)";
        lua_close(L);
        return res;
    }

    // Args casting can be overridden for calibration (RBX_ARGS_SIGNED=1 -> (int32_t)).
    bool signedArgs = getenv("RBX_ARGS_SIGNED") != nullptr;
    if (signedArgs) {
        lua_pushnumber(L, (double)(int32_t)o.u1);
        lua_pushnumber(L, (double)(int32_t)o.u2);
    } else {
        lua_pushnumber(L, (double)o.u1);
        lua_pushnumber(L, (double)o.u2);
    }
    int callres = lua_pcall(L, 2, 1, 0);
    if (callres != 0) {
        const char* err = lua_tostring(L, -1);
        res.status = Status::RuntimeError;
        res.error = err ? err : "(?)";
        lua_close(L);
        return res;
    }
    double ret = lua_tonumber(L, -1);
    res.status = Status::Ok;
    res.raw = ret;
    res.answer = d2ans(ret);
    lua_close(L);
    return res;
}

} // namespace rbxch
