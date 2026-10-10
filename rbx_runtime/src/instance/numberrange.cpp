// numberrange.cpp — NumberRange value type (see value_types.h).
#include "instance/value_types.h"

#include "lua.h"
#include "lualib.h"

#include <cstring>

namespace rbx {

namespace {

int nr_index(lua_State* L) {
    const NumberRange* v = static_cast<const NumberRange*>(luaL_checkudata(L, 1, "NumberRange"));
    const char* k = luaL_checkstring(L, 2);
    if (strcmp(k, "Min") == 0 || strcmp(k, "min") == 0) {
        lua_pushnumber(L, v->min);
        return 1;
    }
    if (strcmp(k, "Max") == 0 || strcmp(k, "max") == 0) {
        lua_pushnumber(L, v->max);
        return 1;
    }
    lua_getmetatable(L, 1); // method lookup (Methods table on the metatable)
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

int nr_new(lua_State* L) {
    NumberRange v;
    v.min = luaL_checknumber(L, 1);
    v.max = luaL_optnumber(L, 2, v.min); // new(v) pins both ends
    push_numberrange(L, v);
    return 1;
}

int nr_eq(lua_State* L) {
    const NumberRange a = check_numberrange(L, 1);
    const NumberRange b = check_numberrange(L, 2);
    lua_pushboolean(L, a == b ? 1 : 0);
    return 1;
}

} // namespace

void push_numberrange(lua_State* L, const NumberRange& v) {
    void* p = lua_newuserdata(L, sizeof(NumberRange));
    *static_cast<NumberRange*>(p) = v;
    if (luaL_newmetatable(L, "NumberRange")) { // first time: fill it
        lua_newtable(L);                      // Methods (empty: lookup fallback)
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, nr_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, nr_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushstring(L, "NumberRange"); // typeof() == "NumberRange" (Roblox parity)
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

NumberRange check_numberrange(lua_State* L, int idx) {
    return *static_cast<const NumberRange*>(luaL_checkudata(L, idx, "NumberRange"));
}

void create_numberrange_class(lua_State* L) {
    push_numberrange(L, NumberRange{});
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, nr_new, "new");
    lua_setfield(L, -2, "new");
    lua_setglobal(L, "NumberRange");
}

} // namespace rbx
