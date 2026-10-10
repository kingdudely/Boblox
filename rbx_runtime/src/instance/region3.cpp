// region3.cpp — Region3 value type (see value_types.h).
//
// Docs-verified surface: new(min, max), CFrame/Size properties,
// ExpandToGrid (snaps min down / max up to resolution multiples).
#include "instance/value_types.h"

#include "instance/vector3.h"

#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <cstring>

namespace rbx {

namespace {

CFrame region_cframe(const Region3& r) {
    CFrame c;
    c.x = (r.min.x + r.max.x) * 0.5;
    c.y = (r.min.y + r.max.y) * 0.5;
    c.z = (r.min.z + r.max.z) * 0.5;
    return c; // identity rotation
}

int rg_index(lua_State* L) {
    const Region3* v = static_cast<const Region3*>(luaL_checkudata(L, 1, "Region3"));
    const char* k = luaL_checkstring(L, 2);
    if (strcmp(k, "CFrame") == 0 || strcmp(k, "cframe") == 0) {
        push_cframe(L, region_cframe(*v));
        return 1;
    }
    if (strcmp(k, "Size") == 0 || strcmp(k, "size") == 0) {
        push_vector3(L, Vector3{v->max.x - v->min.x, v->max.y - v->min.y,
                                v->max.z - v->min.z});
        return 1;
    }
    lua_getmetatable(L, 1);
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

int rg_expand(lua_State* L) {
    Region3 v = check_region3(L, 1);
    const double res = luaL_checknumber(L, 2);
    if (!(res > 0.0)) {
        luaL_error(L, "resolution must be positive");
        return 0;
    }
    Region3 o;
    o.min = Vector3{std::floor(v.min.x / res) * res, std::floor(v.min.y / res) * res,
                    std::floor(v.min.z / res) * res};
    o.max = Vector3{std::ceil(v.max.x / res) * res, std::ceil(v.max.y / res) * res,
                    std::ceil(v.max.z / res) * res};
    push_region3(L, o);
    return 1;
}

int rg_new(lua_State* L) {
    Region3 v;
    v.min = check_vector3(L, 1);
    v.max = check_vector3(L, 2);
    push_region3(L, v);
    return 1;
}

int rg_eq(lua_State* L) {
    const Region3 a = check_region3(L, 1);
    const Region3 b = check_region3(L, 2);
    lua_pushboolean(L, a == b ? 1 : 0);
    return 1;
}

} // namespace

void push_region3(lua_State* L, const Region3& v) {
    void* p = lua_newuserdata(L, sizeof(Region3));
    *static_cast<Region3*>(p) = v;
    if (luaL_newmetatable(L, "Region3")) { // first time: fill it
        lua_newtable(L);
        lua_pushcfunction(L, rg_expand, "ExpandToGrid");
        lua_setfield(L, -2, "ExpandToGrid");
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, rg_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, rg_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushstring(L, "Region3"); // typeof() == "Region3" (Roblox parity)
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

Region3 check_region3(lua_State* L, int idx) {
    return *static_cast<const Region3*>(luaL_checkudata(L, idx, "Region3"));
}

void create_region3_class(lua_State* L) {
    push_region3(L, Region3{});
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, rg_new, "new");
    lua_setfield(L, -2, "new");
    lua_setglobal(L, "Region3");
}

} // namespace rbx
