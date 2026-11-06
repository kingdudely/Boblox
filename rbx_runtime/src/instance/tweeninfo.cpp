// tweeninfo.cpp — TweenInfo value type (see value_types.h).
//
// Roblox: TweenInfo.new(time = 1, easingStyle = Enum.EasingStyle.Quad,
//                       easingDirection = Enum.EasingDirection.Out,
//                       repeatCount = 0, reverses = false, delayTime = 0)
// Fields are read-only; typeof() == "TweenInfo".
#include "instance/value_types.h"

#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <cstring>

namespace rbx {

namespace {

EnumItem check_easing_field(lua_State* L, int idx, const char* enum_name,
                            EnumItem fallback) {
    if (lua_isnoneornil(L, idx))
        return fallback;
    const EnumItem v = check_enumitem(L, idx);
    if (v.enum_name != enum_name)
        luaL_error(L, "Unable to cast value to Enum.%s", enum_name);
    return v;
}

int ti_index(lua_State* L) {
    const TweenInfo* v =
        static_cast<const TweenInfo*>(luaL_checkudata(L, 1, "TweenInfo"));
    const char* k = luaL_checkstring(L, 2);
    if (std::strcmp(k, "Time") == 0) {
        lua_pushnumber(L, v->time);
        return 1;
    }
    if (std::strcmp(k, "DelayTime") == 0) {
        lua_pushnumber(L, v->delay_time);
        return 1;
    }
    if (std::strcmp(k, "RepeatCount") == 0) {
        lua_pushnumber(L, v->repeat_count);
        return 1;
    }
    if (std::strcmp(k, "Reverses") == 0) {
        lua_pushboolean(L, v->reverses ? 1 : 0);
        return 1;
    }
    if (std::strcmp(k, "EasingStyle") == 0) {
        push_enumitem(L, v->easing_style);
        return 1;
    }
    if (std::strcmp(k, "EasingDirection") == 0) {
        push_enumitem(L, v->easing_direction);
        return 1;
    }
    lua_getmetatable(L, 1); // method lookup (Methods table on the metatable)
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

int ti_new(lua_State* L) {
    TweenInfo v;
    v.time = luaL_optnumber(L, 1, 1.0);
    v.easing_style = check_easing_field(L, 2, "EasingStyle",
                                       EnumItem{"EasingStyle", 3});
    v.easing_direction = check_easing_field(
        L, 3, "EasingDirection", EnumItem{"EasingDirection", 1});
    v.repeat_count = int(luaL_optnumber(L, 4, 0.0));
    v.reverses = lua_toboolean(L, 5) != 0;
    v.delay_time = luaL_optnumber(L, 6, 0.0);
    push_tweeninfo(L, v);
    return 1;
}

int ti_eq(lua_State* L) {
    const TweenInfo a = check_tweeninfo(L, 1);
    const TweenInfo b = check_tweeninfo(L, 2);
    lua_pushboolean(L, a == b ? 1 : 0);
    return 1;
}

int ti_tostring(lua_State* L) {
    const TweenInfo v = check_tweeninfo(L, 1);
    lua_pushfstring(L, "TweenInfo(Time = %f, RepeatCount = %d)",
                    v.time, v.repeat_count);
    return 1;
}

} // namespace

void push_tweeninfo(lua_State* L, const TweenInfo& v) {
    void* p = lua_newuserdata(L, sizeof(TweenInfo));
    new (p) TweenInfo(v);
    if (luaL_newmetatable(L, "TweenInfo")) { // first time: fill it
        lua_newtable(L);                     // Methods (empty: lookup fallback)
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, ti_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, ti_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushcfunction(L, ti_tostring, "__tostring");
        lua_setfield(L, -2, "__tostring");
        lua_pushstring(L, "TweenInfo"); // typeof() == "TweenInfo"
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

TweenInfo check_tweeninfo(lua_State* L, int idx) {
    return *static_cast<const TweenInfo*>(
        luaL_checkudata(L, idx, "TweenInfo"));
}

void create_tweeninfo_class(lua_State* L) {
    push_tweeninfo(L, TweenInfo{});
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, ti_new, "new");
    lua_setfield(L, -2, "new");
    lua_setglobal(L, "TweenInfo");
}

} // namespace rbx
