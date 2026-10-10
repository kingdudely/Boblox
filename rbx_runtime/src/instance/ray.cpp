// ray.cpp — Ray value type (see value_types.h).
#include "instance/value_types.h"

#include "instance/vector3.h"

#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <cstring>

namespace rbx {

namespace {

int ray_index(lua_State* L) {
    const Ray* v = static_cast<const Ray*>(luaL_checkudata(L, 1, "Ray"));
    const char* k = luaL_checkstring(L, 2);
    if (strcmp(k, "Origin") == 0 || strcmp(k, "origin") == 0) {
        push_vector3(L, v->origin);
        return 1;
    }
    if (strcmp(k, "Direction") == 0 || strcmp(k, "direction") == 0) {
        push_vector3(L, v->direction);
        return 1;
    }
    lua_getmetatable(L, 1); // method lookup (Methods table on the metatable)
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

// Closest point on the ray to p (t clamped >= 0: rays are half-infinite).
Vector3 closest_point(const Ray& r, const Vector3& p) {
    const double dd = r.direction.x * r.direction.x + r.direction.y * r.direction.y +
                      r.direction.z * r.direction.z;
    double t = 0.0;
    if (dd > 0.0) {
        t = ((p.x - r.origin.x) * r.direction.x + (p.y - r.origin.y) * r.direction.y +
             (p.z - r.origin.z) * r.direction.z) / dd;
        if (t < 0.0)
            t = 0.0;
    }
    return Vector3{r.origin.x + t * r.direction.x, r.origin.y + t * r.direction.y,
                   r.origin.z + t * r.direction.z};
}

int ray_closest(lua_State* L) {
    const Ray r = check_ray(L, 1);
    const Vector3 p = check_vector3(L, 2);
    push_vector3(L, closest_point(r, p));
    return 1;
}

int ray_distance(lua_State* L) {
    const Ray r = check_ray(L, 1);
    const Vector3 p = check_vector3(L, 2);
    const Vector3 c = closest_point(r, p);
    const double dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z;
    lua_pushnumber(L, std::sqrt(dx * dx + dy * dy + dz * dz));
    return 1;
}

int ray_new(lua_State* L) {
    Ray v;
    v.origin = check_vector3(L, 1);
    v.direction = check_vector3(L, 2);
    push_ray(L, v);
    return 1;
}

int ray_eq(lua_State* L) {
    const Ray a = check_ray(L, 1);
    const Ray b = check_ray(L, 2);
    lua_pushboolean(L, a == b ? 1 : 0);
    return 1;
}

} // namespace

void push_ray(lua_State* L, const Ray& v) {
    void* p = lua_newuserdata(L, sizeof(Ray));
    *static_cast<Ray*>(p) = v;
    if (luaL_newmetatable(L, "Ray")) { // first time: fill it
        lua_newtable(L);
        lua_pushcfunction(L, ray_closest, "ClosestPoint");
        lua_setfield(L, -2, "ClosestPoint");
        lua_pushcfunction(L, ray_distance, "Distance");
        lua_setfield(L, -2, "Distance");
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, ray_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, ray_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushstring(L, "Ray"); // typeof() == "Ray" (Roblox parity)
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

Ray check_ray(lua_State* L, int idx) {
    return *static_cast<const Ray*>(luaL_checkudata(L, idx, "Ray"));
}

void create_ray_class(lua_State* L) {
    push_ray(L, Ray{});
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, ray_new, "new");
    lua_setfield(L, -2, "new");
    lua_setglobal(L, "Ray");
}

} // namespace rbx
