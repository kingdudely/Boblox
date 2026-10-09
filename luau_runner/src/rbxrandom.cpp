// rbxrandom.cpp — exact port of Roblox's Random from libroblox.so (build 2.739.691).
//
// Decompiled reference:
//   Random.new (sub_27B30AE):
//       |seed| <= 2^53 -> state = 0x5851F42D4C957F2D * (uint32)(int32)seed + 0x399D2694695129DE
//       else           -> state = 0x399D2694695129DE
//   NextInteger(min,max) (sub_2307554): PCG-xsh-rr style output, see below.
//   NextNumber(min,max)  (sub_42BFF60): two consecutive outputs -> double in [0,1).
#include "rbxrandom.h"

#include <cstdio>
#include <cstdlib>

#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <cstdint>

namespace {

constexpr uint64_t kMul = 0x5851F42D4C957F2DULL;
constexpr uint64_t kInc = 105ULL;
constexpr uint64_t kSeedAdd = 0x399D2694695129DEULL;

inline uint32_t ror32(uint32_t x, unsigned r) {
    r &= 31u;
    return (x >> r) | (x << ((32u - r) & 31u));
}

// PCG xsh-rr 32-bit output of the given 64-bit state.
inline uint32_t out32(uint64_t s) {
    return ror32((uint32_t)((s >> 27) ^ (s >> 45)), (unsigned)(s >> 59));
}

// For NextInteger min/max (normal small ranges).
inline int32_t d2i32(double v) {
    if (!(v >= -2147483648.0 && v < 2147483648.0))
        return (int32_t)(int64_t)v;
    return (int32_t)v;
}

// Roblox's seed conversion: x86-64 `(unsigned int)(int)v` on a double goes through
// a 64-bit conversion FIRST in practice for values in [2^31, 2^53) — the observed
// native behavior for seed >= 2^32 keeps the full 64-bit value (see decompile:
// Random.new used the low 32 bits for small seeds but carries above 2^32 appear in
// captured states). We therefore use the full signed 64-bit truncation.
inline uint64_t seed_from_double(double v) {
    int64_t t = (int64_t)v; // valid for |v| <= 2^53 per the caller's guard
    return (uint64_t)t;
}

struct RbxRandom {
    uint64_t state;
};

RbxRandom* check_random(lua_State* L, int idx) {
    return (RbxRandom*)luaL_checkudata(L, idx, "Random");
}

int rbx_random_new(lua_State* L) {
    int n = lua_gettop(L);
    if (n != 0 && n != 1)
        luaL_error(L, "Random.new requires 0 or 1 argument");
    uint64_t state;
    if (n == 1) {
        double seed = luaL_checknumber(L, 1);
        if (std::fabs(seed) <= 9007199254740992.0 /*2^53*/) {
            uint64_t s = seed_from_double(seed);
            state = kMul * s + kSeedAdd;
        } else {
            state = kSeedAdd;
        }
    } else {
        // Non-deterministic seed is not needed for the challenge; use a fixed
        // entropy source substitute to stay reproducible.
        state = kSeedAdd;
    }
    RbxRandom* r = (RbxRandom*)lua_newuserdata(L, sizeof(RbxRandom));
    r->state = state;
    luaL_getmetatable(L, "Random");
    lua_setmetatable(L, -2);
    return 1;
}

// Exact port of sub_2307554(a1=state, a2=min/int, a3=max/int).
int32_t next_integer_impl(RbxRandom* r, int32_t a2, int32_t a3) {
    uint64_t v3 = (uint64_t)((int64_t)a2 - (int64_t)a3);
    if (a2 <= a3)
        v3 = (uint64_t)((int64_t)a3 - (int64_t)a2);
    if (a2 < a3)
        a3 = a2; // a3 becomes the low end

    uint64_t v4 = kMul * r->state + kInc;
    uint32_t v5 = out32(r->state);

    if ((v3 >> 1) > 0x7FFFFFFEULL) {
        uint32_t v7 = out32(v4);
        r->state = kMul * v4 + kInc;
        uint64_t v8 = v3 + 1;
        if (v8 != 0) {
            uint64_t res = ((uint64_t)(v7 * (uint32_t)v8) >> 32) + (uint64_t)v7 * (v8 >> 32) +
                           (uint64_t)(uint32_t)a3 + (((uint64_t)v5 * (v8 >> 32)) >> 32);
            return (int32_t)res;
        }
        return (int32_t)(v5 | ((uint64_t)v7 << 32));
    }

    r->state = v4;
    return (int32_t)((uint32_t)a3 + (uint32_t)(((uint64_t)v5 * (v3 + 1)) >> 32));
}

int rbx_random_nextinteger(lua_State* L) {
    RbxRandom* r = check_random(L, 1);
    int32_t lo = d2i32(luaL_checknumber(L, 2));
    int32_t hi = d2i32(luaL_checknumber(L, 3));
    bool trace = getenv("RBX_RNG_TRACE") != nullptr;
    if (trace)
        fprintf(stderr, "NEXTINT# state_in=0x%016llx min=%d max=%d\n",
                (unsigned long long)r->state, lo, hi);
    int32_t v = next_integer_impl(r, lo, hi);
    if (trace)
        fprintf(stderr, "   -> %d\n", v);
    lua_pushnumber(L, (double)v);
    return 1;
}

int rbx_random_nextnumber(lua_State* L) {
    RbxRandom* r = check_random(L, 1);
    int n = lua_gettop(L);
    double lo = 0.0, hi = 1.0;
    if (n == 3) {
        lo = luaL_checknumber(L, 2);
        hi = luaL_checknumber(L, 3);
    } else if (n != 1) {
        luaL_error(L, "Random:NextNumber requires 0 or 2 arguments");
    }
    uint64_t v8 = kMul * r->state + kInc;
    uint32_t a = out32(r->state);
    uint32_t b = out32(v8);
    r->state = kMul * v8 + kInc;
    double frac = (double)(((uint64_t)a + ((uint64_t)b << 32)))/ 18446744073709551616.0;
    lua_pushnumber(L, lo + frac * (hi - lo));
    return 1;
}

int rbx_random_clone(lua_State* L) {
    RbxRandom* r = check_random(L, 1);
    RbxRandom* c = (RbxRandom*)lua_newuserdata(L, sizeof(RbxRandom));
    c->state = r->state;
    luaL_getmetatable(L, "Random");
    lua_setmetatable(L, -2);
    return 1;
}

void register_methods(lua_State* L) {
    luaL_newmetatable(L, "Random"); // creates and registers metatable, pushes it
    lua_newtable(L);                // __index table
    lua_pushcfunction(L, rbx_random_nextinteger, "NextInteger");
    lua_setfield(L, -2, "NextInteger");
    lua_pushcfunction(L, rbx_random_nextnumber, "NextNumber");
    lua_setfield(L, -2, "NextNumber");
    lua_pushcfunction(L, rbx_random_clone, "Clone");
    lua_setfield(L, -2, "Clone");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

} // namespace

void rbx_random_register(lua_State* L) {
    register_methods(L);
    lua_newtable(L);
    lua_pushcfunction(L, rbx_random_new, "new");
    lua_setfield(L, -2, "new");
    lua_setglobal(L, "Random");
}
