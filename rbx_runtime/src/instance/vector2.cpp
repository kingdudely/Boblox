// vector2.cpp — Vector2 value type (see value_types.h).
#include "instance/value_types.h"

#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <cstring>

namespace rbx {

namespace {

int v2_index(lua_State* L) {
    const Vector2* v = static_cast<const Vector2*>(luaL_checkudata(L, 1, "Vector2"));
    const char* k = luaL_checkstring(L, 2);
    if (strcmp(k, "x") == 0 || strcmp(k, "X") == 0) {
        lua_pushnumber(L, v->x);
        return 1;
    }
    if (strcmp(k, "y") == 0 || strcmp(k, "Y") == 0) {
        lua_pushnumber(L, v->y);
        return 1;
    }
    if (strcmp(k, "Magnitude") == 0) {
        lua_pushnumber(L, std::sqrt(v->x * v->x + v->y * v->y));
        return 1;
    }
    lua_getmetatable(L, 1); // method lookup (Methods table on the metatable)
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

int v2_new(lua_State* L) {
    Vector2 v;
    v.x = luaL_optnumber(L, 1, 0.0);
    v.y = luaL_optnumber(L, 2, 0.0);
    push_vector2(L, v);
    return 1;
}

int v2_tostring(lua_State* L) {
    const Vector2* v = static_cast<const Vector2*>(luaL_checkudata(L, 1, "Vector2"));
    lua_pushfstring(L, "%g, %g", v->x, v->y);
    return 1;
}

int v2_eq(lua_State* L) {
    const Vector2 a = check_vector2(L, 1);
    const Vector2 b = check_vector2(L, 2);
    lua_pushboolean(L, a == b ? 1 : 0);
    return 1;
}

} // namespace

void push_vector2(lua_State* L, const Vector2& v) {
    void* p = lua_newuserdata(L, sizeof(Vector2));
    *static_cast<Vector2*>(p) = v;
    if (luaL_newmetatable(L, "Vector2")) { // first time: fill it
        lua_newtable(L);                   // Methods (empty: lookup fallback)
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, v2_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, v2_tostring, "__tostring");
        lua_setfield(L, -2, "__tostring");
        lua_pushcfunction(L, v2_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushstring(L, "Vector2"); // typeof() == "Vector2" (Roblox parity)
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

Vector2 check_vector2(lua_State* L, int idx) {
    return *static_cast<const Vector2*>(luaL_checkudata(L, idx, "Vector2"));
}

void create_vector2_class(lua_State* L) {
    push_vector2(L, Vector2{});
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, v2_new, "new");
    lua_setfield(L, -2, "new");
    lua_setglobal(L, "Vector2");
}

} // namespace rbx
