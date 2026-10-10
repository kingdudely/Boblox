// udim.cpp — UDim value type (see value_types.h).
#include "instance/value_types.h"

#include "lua.h"
#include "lualib.h"

#include <cstring>

namespace rbx {

namespace {

int ud_index(lua_State* L) {
    const UDim* v = static_cast<const UDim*>(luaL_checkudata(L, 1, "UDim"));
    const char* k = luaL_checkstring(L, 2);
    if (strcmp(k, "Scale") == 0 || strcmp(k, "scale") == 0) {
        lua_pushnumber(L, v->scale);
        return 1;
    }
    if (strcmp(k, "Offset") == 0 || strcmp(k, "offset") == 0) {
        lua_pushnumber(L, v->offset);
        return 1;
    }
    lua_getmetatable(L, 1); // method lookup (Methods table on the metatable)
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

int ud_new(lua_State* L) {
    UDim v;
    v.scale = luaL_optnumber(L, 1, 0.0);
    v.offset = luaL_optnumber(L, 2, 0.0);
    push_udim(L, v);
    return 1;
}

int ud_eq(lua_State* L) {
    const UDim a = check_udim(L, 1);
    const UDim b = check_udim(L, 2);
    lua_pushboolean(L, a == b ? 1 : 0);
    return 1;
}

} // namespace

void push_udim(lua_State* L, const UDim& v) {
    void* p = lua_newuserdata(L, sizeof(UDim));
    *static_cast<UDim*>(p) = v;
    if (luaL_newmetatable(L, "UDim")) { // first time: fill it
        lua_newtable(L);                // Methods (empty: lookup fallback)
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, ud_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, ud_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushstring(L, "UDim"); // typeof() == "UDim" (Roblox parity)
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

UDim check_udim(lua_State* L, int idx) {
    return *static_cast<const UDim*>(luaL_checkudata(L, idx, "UDim"));
}

void create_udim_class(lua_State* L) {
    push_udim(L, UDim{});
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, ud_new, "new");
    lua_setfield(L, -2, "new");
    lua_setglobal(L, "UDim");
}

} // namespace rbx
