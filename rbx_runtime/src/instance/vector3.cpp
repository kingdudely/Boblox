// vector3.cpp — Vector3 value semantics (arithmetic + the script-facing
// constructor/properties/methods). Roblox's Vector3 is a value type: every
// operation returns a fresh userdata, never mutates an operand.
#include "instance/vector3.h"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rbx {

namespace {

constexpr const char* kMT = "Vector3";

const Vector3& as_v3(lua_State* L, int idx) {
    return *static_cast<const Vector3*>(luaL_checkudata(L, idx, kMT));
}

bool is_v3(lua_State* L, int idx) {
    return lua_isuserdata(L, idx) && luaL_checkudata(L, idx, kMT) != nullptr;
}

int v3_index(lua_State* L) {
    const Vector3* v = static_cast<const Vector3*>(luaL_checkudata(L, 1, kMT));
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
    if (strcmp(k, "Unit") == 0) {
        const double m = std::sqrt(v->x * v->x + v->y * v->y + v->z * v->z);
        push_vector3(L, m == 0.0 ? Vector3{} : Vector3{v->x / m, v->y / m, v->z / m});
        return 1;
    }
    lua_getmetatable(L, 1); // method lookup (Methods table on the metatable)
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

int v3_add(lua_State* L) {
    const Vector3& a = as_v3(L, 1);
    const Vector3& b = as_v3(L, 2);
    push_vector3(L, Vector3{a.x + b.x, a.y + b.y, a.z + b.z});
    return 1;
}

int v3_sub(lua_State* L) {
    const Vector3& a = as_v3(L, 1);
    const Vector3& b = as_v3(L, 2);
    push_vector3(L, Vector3{a.x - b.x, a.y - b.y, a.z - b.z});
    return 1;
}

int v3_mul(lua_State* L) {
    if (is_v3(L, 1) && is_v3(L, 2)) {
        const Vector3& a = as_v3(L, 1);
        const Vector3& b = as_v3(L, 2);
        push_vector3(L, Vector3{a.x * b.x, a.y * b.y, a.z * b.z});
        return 1;
    }
    if (is_v3(L, 1)) {
        const Vector3& a = as_v3(L, 1);
        const double s = luaL_checknumber(L, 2);
        push_vector3(L, Vector3{a.x * s, a.y * s, a.z * s});
        return 1;
    }
    const double s = luaL_checknumber(L, 1);
    const Vector3& b = as_v3(L, 2);
    push_vector3(L, Vector3{b.x * s, b.y * s, b.z * s});
    return 1;
}

int v3_div(lua_State* L) {
    if (is_v3(L, 1) && is_v3(L, 2)) {
        const Vector3& a = as_v3(L, 1);
        const Vector3& b = as_v3(L, 2);
        push_vector3(L, Vector3{a.x / b.x, a.y / b.y, a.z / b.z});
        return 1;
    }
    const Vector3& a = as_v3(L, 1);
    const double s = luaL_checknumber(L, 2);
    push_vector3(L, Vector3{a.x / s, a.y / s, a.z / s});
    return 1;
}

int v3_unm(lua_State* L) {
    const Vector3& a = as_v3(L, 1);
    push_vector3(L, Vector3{-a.x, -a.y, -a.z});
    return 1;
}

int v3_eq(lua_State* L) {
    if (!is_v3(L, 1) || !is_v3(L, 2)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, as_v3(L, 1) == as_v3(L, 2) ? 1 : 0);
    return 1;
}

int v3_tostring(lua_State* L) {
    const Vector3& v = as_v3(L, 1);
    lua_pushfstring(L, "%g, %g, %g", v.x, v.y, v.z);
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

int v3_dot(lua_State* L) {
    const Vector3& a = as_v3(L, 1);
    const Vector3& b = as_v3(L, 2);
    lua_pushnumber(L, a.x * b.x + a.y * b.y + a.z * b.z);
    return 1;
}

int v3_cross(lua_State* L) {
    const Vector3& a = as_v3(L, 1);
    const Vector3& b = as_v3(L, 2);
    push_vector3(L, Vector3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                            a.x * b.y - a.y * b.x});
    return 1;
}

int v3_lerp(lua_State* L) {
    const Vector3& a = as_v3(L, 1);
    const Vector3& b = as_v3(L, 2);
    const double t = luaL_checknumber(L, 3);
    push_vector3(L, Vector3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                            a.z + (b.z - a.z) * t});
    return 1;
}

int v3_fuzzy_eq(lua_State* L) {
    const Vector3& a = as_v3(L, 1);
    const Vector3& b = as_v3(L, 2);
    const double eps = luaL_optnumber(L, 3, 1e-5);
    lua_pushboolean(L, std::fabs(a.x - b.x) <= eps &&
                             std::fabs(a.y - b.y) <= eps &&
                             std::fabs(a.z - b.z) <= eps
                         ? 1
                         : 0);
    return 1;
}

int v3_abs(lua_State* L) {
    const Vector3& a = as_v3(L, 1);
    push_vector3(L, Vector3{std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)});
    return 1;
}

int v3_ceil(lua_State* L) {
    const Vector3& a = as_v3(L, 1);
    push_vector3(L, Vector3{std::ceil(a.x), std::ceil(a.y), std::ceil(a.z)});
    return 1;
}

int v3_floor(lua_State* L) {
    const Vector3& a = as_v3(L, 1);
    push_vector3(L, Vector3{std::floor(a.x), std::floor(a.y), std::floor(a.z)});
    return 1;
}

int v3_round(lua_State* L) {
    const Vector3& a = as_v3(L, 1);
    push_vector3(L, Vector3{std::round(a.x), std::round(a.y), std::round(a.z)});
    return 1;
}

int v3_sign(lua_State* L) {
    const Vector3& a = as_v3(L, 1);
    auto sign = [](double v) { return v > 0 ? 1.0 : (v < 0 ? -1.0 : 0.0); };
    push_vector3(L, Vector3{sign(a.x), sign(a.y), sign(a.z)});
    return 1;
}

int v3_max(lua_State* L) {
    const Vector3& a = as_v3(L, 1);
    const Vector3& b = as_v3(L, 2);
    push_vector3(L, Vector3{std::max(a.x, b.x), std::max(a.y, b.y),
                            std::max(a.z, b.z)});
    return 1;
}

int v3_min(lua_State* L) {
    const Vector3& a = as_v3(L, 1);
    const Vector3& b = as_v3(L, 2);
    push_vector3(L, Vector3{std::min(a.x, b.x), std::min(a.y, b.y),
                            std::min(a.z, b.z)});
    return 1;
}

int v3_angle(lua_State* L) {
    const Vector3& a = as_v3(L, 1);
    const Vector3& b = as_v3(L, 2);
    const double ma = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    const double mb = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
    double c = 0.0;
    if (ma > 0.0 && mb > 0.0)
        c = (a.x * b.x + a.y * b.y + a.z * b.z) / (ma * mb);
    c = std::max(-1.0, std::min(1.0, c));
    lua_pushnumber(L, std::acos(c));
    return 1;
}

int v3_distance(lua_State* L) {
    const Vector3& a = as_v3(L, 1);
    const Vector3& b = as_v3(L, 2);
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    lua_pushnumber(L, std::sqrt(dx * dx + dy * dy + dz * dz));
    return 1;
}

void set_method(lua_State* L, const char* name, lua_CFunction fn) {
    lua_pushcfunction(L, fn, name);
    lua_setfield(L, -2, name);
}

} // namespace

void push_vector3(lua_State* L, const Vector3& v) {
    void* p = lua_newuserdata(L, sizeof(Vector3));
    *static_cast<Vector3*>(p) = v;
    if (luaL_newmetatable(L, kMT)) { // first time: fill it
        lua_newtable(L); // Methods
        set_method(L, "Dot", v3_dot);
        set_method(L, "Cross", v3_cross);
        set_method(L, "Lerp", v3_lerp);
        set_method(L, "FuzzyEq", v3_fuzzy_eq);
        set_method(L, "Abs", v3_abs);
        set_method(L, "Ceil", v3_ceil);
        set_method(L, "Floor", v3_floor);
        set_method(L, "Round", v3_round);
        set_method(L, "Sign", v3_sign);
        set_method(L, "Max", v3_max);
        set_method(L, "Min", v3_min);
        set_method(L, "Angle", v3_angle);
        set_method(L, "Distance", v3_distance);
        lua_setfield(L, -2, "Methods");
        set_method(L, "__index", v3_index);
        set_method(L, "__newindex", [](lua_State* L) -> int {
            luaL_error(L, "Vector3 is immutable");
            return 0;
        });
        set_method(L, "__add", v3_add);
        set_method(L, "__sub", v3_sub);
        set_method(L, "__mul", v3_mul);
        set_method(L, "__div", v3_div);
        set_method(L, "__unm", v3_unm);
        set_method(L, "__eq", v3_eq);
        set_method(L, "__tostring", v3_tostring);
        lua_pushstring(L, kMT); // typeof() == "Vector3" (Roblox parity)
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

Vector3 check_vector3(lua_State* L, int idx) {
    return *static_cast<const Vector3*>(luaL_checkudata(L, idx, kMT));
}

void create_vector3_class(lua_State* L) {
    push_vector3(L, Vector3{}); // ensures the metatable exists
    lua_pop(L, 1);

    lua_newtable(L);
    set_method(L, "new", v3_new);
    push_vector3(L, Vector3{});
    lua_setfield(L, -2, "zero");
    push_vector3(L, Vector3{1, 1, 1});
    lua_setfield(L, -2, "one");
    push_vector3(L, Vector3{1, 0, 0});
    lua_setfield(L, -2, "xAxis");
    push_vector3(L, Vector3{0, 1, 0});
    lua_setfield(L, -2, "yAxis");
    push_vector3(L, Vector3{0, 0, 1});
    lua_setfield(L, -2, "zAxis");
    lua_setglobal(L, "Vector3");
}

} // namespace rbx
