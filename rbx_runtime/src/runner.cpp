// runner.cpp — library implementation of challenge_runner (see runner.h).
#include "runner.h"

#include "rbxrandom.h"

#include "lua.h"
#include "lualib.h"

#include "Luau/Common.h"

#include <cstdlib>
#include <cstring>
#include <string>

// Roblox's production bytecode is compiled with the LuauCallFeedback feature
// enabled, which emits LOP_CALLFB instructions after namecalls. The upstream VM
// defaults this flag to false, which asserts (debug) or miscomputes (release)
// on such bytecode. Flip it on for the lifetime of this runner.
LUAU_FASTFLAG(LuauCallFeedback)

namespace {

using rbxch::Options;

Options* g_opts = nullptr;

/* ---------------- game / RunService / UserSettings / os -------------- */

int lua_game_getservice(lua_State* L) {
    const char* name = luaL_checkstring(L, 2);
    if (std::strcmp(name, "RunService") == 0) {
        lua_getfield(L, LUA_REGISTRYINDEX, "rbx.RunService");
        return 1;
    }
    lua_newtable(L); // unknown service
    return 1;
}

int lua_isstudio(lua_State* L) {
    const std::string& s = g_opts->studio;
    if (s == "error") {
        luaL_error(L, "IsStudio unavailable");
        return 0;
    }
    lua_pushboolean(L, s == "true");
    return 1;
}

int us_tostring_cont(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    return 1;
}

int lua_usersettings(lua_State* L) {
    if (g_opts->usersettings == "error") {
        luaL_error(L, "UserSettings unavailable");
        return 0;
    }
    lua_newtable(L);
    lua_newtable(L);
    lua_pushstring(L, g_opts->us_string.c_str());
    lua_pushcclosure(L, us_tostring_cont, "__tostring", 1);
    lua_setfield(L, -2, "__tostring");
    lua_setmetatable(L, -2);
    return 1;
}

int lua_osexit(lua_State* L) {
    const std::string& s = g_opts->os_exit;
    if (s == "missing") {
        luaL_error(L, "os.exit unavailable");
        return 0;
    }
    if (s == "exit")
        std::exit(0);
    return 0;
}

int lua_newproxy_shim(lua_State* L) {
    if (g_opts->newproxy == "error") {
        luaL_error(L, "newproxy unavailable");
        return 0;
    }
    int b = luaL_optboolean(L, 1, 0);
    (void)lua_newuserdata(L, sizeof(void*));
    if (b) {
        lua_newtable(L);
        lua_setmetatable(L, -2);
    }
    return 1;
}

void setup_sandbox(lua_State* L, Options& o) {
    g_opts = &o;
    luaL_openlibs(L);

    // Roblox Random (PCG)
    rbx_random_register(L);

    // game = { JobId, GetService }
    lua_newtable(L);
    lua_pushstring(L, o.job.c_str());
    lua_setfield(L, -2, "JobId");
    lua_pushcfunction(L, lua_game_getservice, "GetService");
    lua_setfield(L, -2, "GetService");
    lua_setglobal(L, "game");

    // RunService = { IsStudio }
    lua_newtable(L);
    lua_pushcfunction(L, lua_isstudio, "IsStudio");
    lua_setfield(L, -2, "IsStudio");
    lua_setfield(L, LUA_REGISTRYINDEX, "rbx.RunService");

    // UserSettings global
    if (o.usersettings == "error") {
        lua_pushnil(L);
        lua_setglobal(L, "UserSettings");
    } else {
        lua_pushcfunction(L, lua_usersettings, "UserSettings");
        lua_setglobal(L, "UserSettings");
    }

    // os.exit adjustment
    lua_getglobal(L, "os");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setglobal(L, "os");
    }
    lua_pushcfunction(L, lua_osexit, "exit");
    lua_setfield(L, -2, "exit");
    lua_pop(L, 1);

    // newproxy override
    lua_pushcfunction(L, lua_newproxy_shim, "newproxy");
    lua_setglobal(L, "newproxy");
}

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
    Options o = opts;

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
