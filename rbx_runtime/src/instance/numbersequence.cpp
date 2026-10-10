// numbersequence.cpp — NumberSequence + NumberSequenceKeypoint (see value_types.h).
//
// Docs-verified surface: new(n), new(n0, n1), new(keypoint-table),
// Keypoints array. new(n) pins a single keypoint at t=0; new(n0, n1) spans
// t=0..1. Table entries must be keypoint userdata (strict).
#include "instance/value_types.h"

#include "instance/teardown.h"

#include "lua.h"
#include "lualib.h"

#include <cstring>

namespace rbx {

namespace {

constexpr const char* kKeyMT = "NumberSequenceKeypoint";

int nsk_index(lua_State* L) {
    const NumberSequenceKeypoint* v =
        static_cast<const NumberSequenceKeypoint*>(luaL_checkudata(L, 1, kKeyMT));
    const char* k = luaL_checkstring(L, 2);
    if (strcmp(k, "Time") == 0 || strcmp(k, "time") == 0) {
        lua_pushnumber(L, v->time);
        return 1;
    }
    if (strcmp(k, "Value") == 0 || strcmp(k, "value") == 0) {
        lua_pushnumber(L, v->value);
        return 1;
    }
    if (strcmp(k, "Envelope") == 0 || strcmp(k, "envelope") == 0) {
        lua_pushnumber(L, v->envelope);
        return 1;
    }
    lua_getmetatable(L, 1);
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

int nsk_new(lua_State* L) {
    NumberSequenceKeypoint v;
    v.time = luaL_checknumber(L, 1);
    v.value = luaL_checknumber(L, 2);
    v.envelope = luaL_optnumber(L, 3, 0.0);
    push_numbersequencekeypoint(L, v);
    return 1;
}

int nsk_eq(lua_State* L) {
    const NumberSequenceKeypoint a = check_numbersequencekeypoint(L, 1);
    const NumberSequenceKeypoint b = check_numbersequencekeypoint(L, 2);
    lua_pushboolean(L, a == b ? 1 : 0);
    return 1;
}

int ns_index(lua_State* L) {
    const char* k = luaL_checkstring(L, 2);
    if (strcmp(k, "Keypoints") == 0 || strcmp(k, "keypoints") == 0) {
        const NumberSequence v = check_numbersequence(L, 1); // via slab handle
        lua_newtable(L);
        int i = 1;
        for (const auto& kp : v.keys) {
            push_numbersequencekeypoint(L, kp);
            lua_rawseti(L, -2, i++);
        }
        return 1;
    }
    lua_getmetatable(L, 1);
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

// new(n) | new(n0, n1) | new({keypoints})
int ns_new(lua_State* L) {
    NumberSequence v;
    if (lua_istable(L, 1)) {
        const size_t n = lua_objlen(L, 1);
        for (size_t i = 1; i <= n; i++) {
            lua_rawgeti(L, 1, (int)i);
            v.keys.push_back(check_numbersequencekeypoint(L, -1));
            lua_pop(L, 1);
        }
    } else {
        const double n0 = luaL_checknumber(L, 1);
        if (lua_isnoneornil(L, 2)) {
            v.keys.push_back(NumberSequenceKeypoint{0.0, n0, 0.0});
        } else {
            const double n1 = luaL_checknumber(L, 2);
            v.keys.push_back(NumberSequenceKeypoint{0.0, n0, 0.0});
            v.keys.push_back(NumberSequenceKeypoint{1.0, n1, 0.0});
        }
    }
    push_numbersequence(L, v);
    return 1;
}

int ns_eq(lua_State* L) {
    const NumberSequence a = check_numbersequence(L, 1);
    const NumberSequence b = check_numbersequence(L, 2);
    lua_pushboolean(L, a == b ? 1 : 0);
    return 1;
}

} // namespace

void push_numbersequencekeypoint(lua_State* L, const NumberSequenceKeypoint& v) {
    void* p = lua_newuserdata(L, sizeof(NumberSequenceKeypoint));
    *static_cast<NumberSequenceKeypoint*>(p) = v;
    if (luaL_newmetatable(L, kKeyMT)) { // first time: fill it
        lua_newtable(L);
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, nsk_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, nsk_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushstring(L, kKeyMT); // typeof() parity
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

NumberSequenceKeypoint check_numbersequencekeypoint(lua_State* L, int idx) {
    return *static_cast<const NumberSequenceKeypoint*>(
        luaL_checkudata(L, idx, kKeyMT));
}

void push_numbersequence(lua_State* L, const NumberSequence& v) {
    // Heap storage lives in the per-state slab (see teardown.h): the userdata
    // holds only a stable index handle, so Lua freeing the userdata at any
    // GC cycle can never dangle teardown. Copies share nothing (each push
    // copies into a fresh slot); values are immutable from Lua anyway.
    Slab* s = slab(L);
    const auto idx = (uint32_t)s->sequences.size();
    s->sequences.push_back(std::make_unique<NumberSequence>(v));
    void* p = lua_newuserdata(L, sizeof(idx));
    *static_cast<uint32_t*>(p) = idx;
    if (luaL_newmetatable(L, "NumberSequence")) { // first time: fill it
        lua_newtable(L);
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, ns_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, ns_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushstring(L, "NumberSequence"); // typeof() parity
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

NumberSequence check_numbersequence(lua_State* L, int idx) {
    const uint32_t h =
        *static_cast<const uint32_t*>(luaL_checkudata(L, idx, "NumberSequence"));
    Slab* s = slab(L);
    if (h >= s->sequences.size() || !s->sequences[h])
        luaL_error(L, "NumberSequence handle out of range");
    return *s->sequences[h]; // deep copy out (C++-side values own theirs)
}

void create_numbersequence_class(lua_State* L) {
    push_numbersequence(L, NumberSequence{});
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, ns_new, "new");
    lua_setfield(L, -2, "new");
    lua_setglobal(L, "NumberSequence");

    push_numbersequencekeypoint(L, NumberSequenceKeypoint{});
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, nsk_new, "new");
    lua_setfield(L, -2, "new");
    lua_setglobal(L, "NumberSequenceKeypoint");
}

} // namespace rbx
