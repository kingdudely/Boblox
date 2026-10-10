// rect.cpp — Rect value type (see value_types.h).
#include "instance/value_types.h"

#include "lua.h"
#include "lualib.h"

#include <cstring>

namespace rbx {

namespace {

int rt_index(lua_State* L) {
    const Rect* v = static_cast<const Rect*>(luaL_checkudata(L, 1, "Rect"));
    const char* k = luaL_checkstring(L, 2);
    if (strcmp(k, "Min") == 0 || strcmp(k, "min") == 0) {
        push_vector2(L, v->min);
        return 1;
    }
    if (strcmp(k, "Max") == 0 || strcmp(k, "max") == 0) {
        push_vector2(L, v->max);
        return 1;
    }
    if (strcmp(k, "Width") == 0 || strcmp(k, "width") == 0) {
        lua_pushnumber(L, v->max.x - v->min.x);
        return 1;
    }
    if (strcmp(k, "Height") == 0 || strcmp(k, "height") == 0) {
        lua_pushnumber(L, v->max.y - v->min.y);
        return 1;
    }
    lua_getmetatable(L, 1); // method lookup (Methods table on the metatable)
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

int rt_new(lua_State* L) {
    Rect v;
    v.min.x = luaL_checknumber(L, 1);
    v.min.y = luaL_checknumber(L, 2);
    v.max.x = luaL_checknumber(L, 3);
    v.max.y = luaL_checknumber(L, 4);
    push_rect(L, v);
    return 1;
}

int rt_eq(lua_State* L) {
    const Rect a = check_rect(L, 1);
    const Rect b = check_rect(L, 2);
    lua_pushboolean(L, a == b ? 1 : 0);
    return 1;
}

} // namespace

void push_rect(lua_State* L, const Rect& v) {
    void* p = lua_newuserdata(L, sizeof(Rect));
    *static_cast<Rect*>(p) = v;
    if (luaL_newmetatable(L, "Rect")) { // first time: fill it
        lua_newtable(L);                // Methods (empty: lookup fallback)
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, rt_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, rt_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushstring(L, "Rect"); // typeof() == "Rect" (Roblox parity)
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

Rect check_rect(lua_State* L, int idx) {
    return *static_cast<const Rect*>(luaL_checkudata(L, idx, "Rect"));
}

void create_rect_class(lua_State* L) {
    push_rect(L, Rect{});
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, rt_new, "new");
    lua_setfield(L, -2, "new");
    lua_setglobal(L, "Rect");
}

} // namespace rbx
