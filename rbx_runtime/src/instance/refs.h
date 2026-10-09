// refs.h — minimal registry-reference helpers (Luau has no lauxlib luaL_ref).
//
// ref_new pops the value at the top of L, stores it in a registry table
// ("rbx.Refs"), and returns an integer key; ref_push pushes it back;
// ref_free drops the anchor. Refs die with the lua_State, so leaks only
// matter for the lifetime of one environment (close frees everything).
#pragma once

#include "lua.h"

namespace rbx {

inline void refs_table(lua_State* L) { // pushes the refs table
    lua_getfield(L, LUA_REGISTRYINDEX, "rbx.Refs");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "rbx.Refs");
    }
}

// Pops the top value; returns its ref key (1-based, monotonic).
inline int ref_new(lua_State* L) {
    refs_table(L);                              // ... value, refs
    lua_getfield(L, -1, "n");                   // ... value, refs, n
    int n = (int)lua_tointeger(L, -1) + 1;
    lua_pop(L, 1);                              // ... value, refs
    lua_pushinteger(L, n);                      // ... value, refs, n
    lua_pushvalue(L, -3);                       // ... value, refs, n, value
    lua_settable(L, -3);                        // ... value, refs (refs[n]=v)
    lua_pushinteger(L, n);                      // ... value, refs, n
    lua_setfield(L, -2, "n");                   // ... value, refs (counter!)
    lua_pop(L, 2);                              // ...
    return n;
}

inline void ref_push(lua_State* L, int key) {
    refs_table(L);
    lua_rawgeti(L, -1, key);
    lua_remove(L, -2);
}

inline void ref_free(lua_State* L, int& key) {
    if (key <= 0)
        return;
    refs_table(L);
    lua_pushnil(L);
    lua_rawseti(L, -2, key);
    lua_pop(L, 1);
    key = 0;
}

} // namespace rbx
