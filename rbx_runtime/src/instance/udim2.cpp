// udim2.cpp — UDim2 value type (see value_types.h).
#include "instance/value_types.h"

#include "lua.h"
#include "lualib.h"

#include <cstring>

namespace rbx {

namespace {

int u2_index(lua_State* L) {
    const UDim2* v = static_cast<const UDim2*>(luaL_checkudata(L, 1, "UDim2"));
    const char* k = luaL_checkstring(L, 2);
    if (strcmp(k, "X") == 0 || strcmp(k, "x") == 0) {
        push_udim(L, v->x);
        return 1;
    }
    if (strcmp(k, "Y") == 0 || strcmp(k, "y") == 0) {
        push_udim(L, v->y);
        return 1;
    }
    if (strcmp(k, "Width") == 0 || strcmp(k, "width") == 0) {
        lua_pushnumber(L, v->x.offset);
        return 1;
    }
    if (strcmp(k, "Height") == 0 || strcmp(k, "height") == 0) {
        lua_pushnumber(L, v->y.offset);
        return 1;
    }
    lua_getmetatable(L, 1); // method lookup (Methods table on the metatable)
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

int u2_new(lua_State* L) {
    UDim2 v;
    v.x.scale = luaL_optnumber(L, 1, 0.0);
    v.x.offset = luaL_optnumber(L, 2, 0.0);
    v.y.scale = luaL_optnumber(L, 3, 0.0);
    v.y.offset = luaL_optnumber(L, 4, 0.0);
    push_udim2(L, v);
    return 1;
}

int u2_fromscale(lua_State* L) {
    UDim2 v;
    v.x.scale = luaL_checknumber(L, 1);
    v.y.scale = luaL_checknumber(L, 2);
    push_udim2(L, v);
    return 1;
}

int u2_fromoffset(lua_State* L) {
    UDim2 v;
    v.x.offset = luaL_checknumber(L, 1);
    v.y.offset = luaL_checknumber(L, 2);
    push_udim2(L, v);
    return 1;
}

int u2_eq(lua_State* L) {
    const UDim2 a = check_udim2(L, 1);
    const UDim2 b = check_udim2(L, 2);
    lua_pushboolean(L, a == b ? 1 : 0);
    return 1;
}

} // namespace

void push_udim2(lua_State* L, const UDim2& v) {
    void* p = lua_newuserdata(L, sizeof(UDim2));
    *static_cast<UDim2*>(p) = v;
    if (luaL_newmetatable(L, "UDim2")) { // first time: fill it
        lua_newtable(L);                 // Methods (empty: lookup fallback)
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, u2_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, u2_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushstring(L, "UDim2"); // typeof() == "UDim2" (Roblox parity)
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

UDim2 check_udim2(lua_State* L, int idx) {
    return *static_cast<const UDim2*>(luaL_checkudata(L, idx, "UDim2"));
}

void create_udim2_class(lua_State* L) {
    push_udim2(L, UDim2{});
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, u2_new, "new");
    lua_setfield(L, -2, "new");
    lua_pushcfunction(L, u2_fromscale, "fromScale");
    lua_setfield(L, -2, "fromScale");
    lua_pushcfunction(L, u2_fromoffset, "fromOffset");
    lua_setfield(L, -2, "fromOffset");
    lua_setglobal(L, "UDim2");
}

} // namespace rbx
