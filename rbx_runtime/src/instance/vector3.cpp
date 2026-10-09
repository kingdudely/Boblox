// vector3.cpp — see vector3.h.
#include "instance/vector3.h"

#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <cstring>

namespace rbx {

namespace {

int v3_index(lua_State* L) {
    const Vector3* v = static_cast<const Vector3*>(luaL_checkudata(L, 1, "Vector3"));
    const char* k = luaL_checkstring(L, 2);
    if (strcmp(k, "x") == 0 || strcmp(k, "X") == 0) {
        lua_pushnumber(L, v->x);
        return 1;
    }
    if (strcmp(k, "y") == 0 || strcmp(k, "Y") == 0) {
        lua_pushnumber(L, v->y);
        return 1;
    }
    if (strcmp(k, "z") == 0 || strcmp(k, "Z") == 0) {
        lua_pushnumber(L, v->z);
        return 1;
    }
    if (strcmp(k, "Magnitude") == 0) {
        lua_pushnumber(L, std::sqrt(v->x * v->x + v->y * v->y + v->z * v->z));
        return 1;
    }
    lua_getmetatable(L, 1); // method lookup (Methods table on the metatable)
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

int v3_dot(lua_State* L) {
    const Vector3* a = static_cast<const Vector3*>(luaL_checkudata(L, 1, "Vector3"));
    const Vector3* b = static_cast<const Vector3*>(luaL_checkudata(L, 2, "Vector3"));
    lua_pushnumber(L, a->x * b->x + a->y * b->y + a->z * b->z);
    return 1;
}

int v3_new(lua_State* L) {
    Vector3 v;
    v.x = luaL_optnumber(L, 1, 0.0);
    v.y = luaL_optnumber(L, 2, 0.0);
    v.z = luaL_optnumber(L, 3, 0.0);
    push_vector3(L, v);
    return 1;
}

int v3_tostring(lua_State* L) {
    const Vector3* v = static_cast<const Vector3*>(luaL_checkudata(L, 1, "Vector3"));
    lua_pushfstring(L, "%g, %g, %g", v->x, v->y, v->z);
    return 1;
}

int v3_eq(lua_State* L) {
    const Vector3* a = static_cast<const Vector3*>(luaL_checkudata(L, 1, "Vector3"));
    const Vector3* b = static_cast<const Vector3*>(luaL_checkudata(L, 2, "Vector3"));
    lua_pushboolean(L, *a == *b ? 1 : 0);
    return 1;
}

} // namespace

void push_vector3(lua_State* L, const Vector3& v) {
    void* p = lua_newuserdata(L, sizeof(Vector3));
    *static_cast<Vector3*>(p) = v;
    if (luaL_newmetatable(L, "Vector3")) { // first time: fill it
        lua_newtable(L);
        lua_pushcfunction(L, v3_dot, "Dot");
        lua_setfield(L, -2, "Dot");
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, v3_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, v3_tostring, "__tostring");
        lua_setfield(L, -2, "__tostring");
        lua_pushcfunction(L, v3_eq, "__eq");
        lua_setfield(L, -2, "__eq");
    }
    lua_setmetatable(L, -2);
}

Vector3 check_vector3(lua_State* L, int idx) {
    return *static_cast<const Vector3*>(luaL_checkudata(L, idx, "Vector3"));
}

void create_vector3_class(lua_State* L) {
    push_vector3(L, Vector3{}); // ensures the metatable exists
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, v3_new, "new");
    lua_setfield(L, -2, "new");
    push_vector3(L, Vector3{}); // zero
    lua_setfield(L, -2, "zero");
    lua_setglobal(L, "Vector3");
}

} // namespace rbx
