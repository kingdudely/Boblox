// color3.cpp — Color3 value type (see value_types.h).
#include "instance/value_types.h"

#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <cstring>

namespace rbx {

namespace {

int c3_index(lua_State* L) {
    const Color3* v = static_cast<const Color3*>(luaL_checkudata(L, 1, "Color3"));
    const char* k = luaL_checkstring(L, 2);
    if (strcmp(k, "r") == 0 || strcmp(k, "R") == 0) {
        lua_pushnumber(L, v->r);
        return 1;
    }
    if (strcmp(k, "g") == 0 || strcmp(k, "G") == 0) {
        lua_pushnumber(L, v->g);
        return 1;
    }
    if (strcmp(k, "b") == 0 || strcmp(k, "B") == 0) {
        lua_pushnumber(L, v->b);
        return 1;
    }
    lua_getmetatable(L, 1); // method lookup (Methods table on the metatable)
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

// toHSV: h, s, v all in [0, 1] (Roblox convention).
int c3_tohsv(lua_State* L) {
    const Color3 v = check_color3(L, 1);
    const double mx = v.r > v.g ? (v.r > v.b ? v.r : v.b) : (v.g > v.b ? v.g : v.b);
    const double mn = v.r < v.g ? (v.r < v.b ? v.r : v.b) : (v.g < v.b ? v.g : v.b);
    const double vv = mx;
    double hh = 0.0, ss = 0.0;
    if (mx > 0.0) {
        ss = (mx - mn) / mx;
        if (mx == v.r)
            hh = (v.g - v.b) / (mx - mn);
        else if (mx == v.g)
            hh = 2.0 + (v.b - v.r) / (mx - mn);
        else
            hh = 4.0 + (v.r - v.g) / (mx - mn);
        hh /= 6.0;
        if (hh < 0.0)
            hh += 1.0;
    }
    lua_pushnumber(L, hh);
    lua_pushnumber(L, ss);
    lua_pushnumber(L, vv);
    return 3;
}

int c3_new(lua_State* L) {
    Color3 v;
    v.r = luaL_optnumber(L, 1, 0.0);
    v.g = luaL_optnumber(L, 2, 0.0);
    v.b = luaL_optnumber(L, 3, 0.0);
    push_color3(L, v);
    return 1;
}

int c3_fromrgb(lua_State* L) {
    Color3 v;
    v.r = luaL_checknumber(L, 1) / 255.0;
    v.g = luaL_checknumber(L, 2) / 255.0;
    v.b = luaL_checknumber(L, 3) / 255.0;
    push_color3(L, v);
    return 1;
}

// fromHSV: h, s, v all in [0, 1].
int c3_fromhsv(lua_State* L) {
    const double h = luaL_checknumber(L, 1);
    const double s = luaL_checknumber(L, 2);
    const double v = luaL_checknumber(L, 3);
    const double c = v * s;
    double hh = h * 6.0;
    while (hh < 0.0)
        hh += 6.0;
    while (hh >= 6.0)
        hh -= 6.0;
    const double x = c * (1.0 - std::fabs(std::fmod(hh, 2.0) - 1.0));
    double r = 0.0, g = 0.0, b = 0.0;
    const int sector = (int)hh;
    if (sector == 0) {
        r = c;
        g = x;
    } else if (sector == 1) {
        r = x;
        g = c;
    } else if (sector == 2) {
        g = c;
        b = x;
    } else if (sector == 3) {
        g = x;
        b = c;
    } else if (sector == 4) {
        r = x;
        b = c;
    } else {
        r = c;
        b = x;
    }
    const double m = v - c;
    push_color3(L, Color3{r + m, g + m, b + m});
    return 1;
}

int c3_tostring(lua_State* L) {
    const Color3* v = static_cast<const Color3*>(luaL_checkudata(L, 1, "Color3"));
    lua_pushfstring(L, "%g, %g, %g", v->r, v->g, v->b);
    return 1;
}

int c3_eq(lua_State* L) {
    const Color3 a = check_color3(L, 1);
    const Color3 b = check_color3(L, 2);
    lua_pushboolean(L, a == b ? 1 : 0);
    return 1;
}

} // namespace

void push_color3(lua_State* L, const Color3& v) {
    void* p = lua_newuserdata(L, sizeof(Color3));
    *static_cast<Color3*>(p) = v;
    if (luaL_newmetatable(L, "Color3")) { // first time: fill it
        lua_newtable(L);
        lua_pushcfunction(L, c3_tohsv, "toHSV");
        lua_setfield(L, -2, "toHSV");
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, c3_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, c3_tostring, "__tostring");
        lua_setfield(L, -2, "__tostring");
        lua_pushcfunction(L, c3_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushstring(L, "Color3"); // typeof() == "Color3" (Roblox parity)
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

Color3 check_color3(lua_State* L, int idx) {
    return *static_cast<const Color3*>(luaL_checkudata(L, idx, "Color3"));
}

void create_color3_class(lua_State* L) {
    push_color3(L, Color3{});
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, c3_new, "new");
    lua_setfield(L, -2, "new");
    lua_pushcfunction(L, c3_fromrgb, "fromRGB");
    lua_setfield(L, -2, "fromRGB");
    lua_pushcfunction(L, c3_fromhsv, "fromHSV");
    lua_setfield(L, -2, "fromHSV");
    lua_setglobal(L, "Color3");
}

} // namespace rbx
