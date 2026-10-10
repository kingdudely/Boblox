// colorsequence.cpp — ColorSequence + ColorSequenceKeypoint (see value_types.h).
//
// Docs-verified surface: new(color), new(keypoint-table), Keypoints array.
#include "instance/value_types.h"

#include "instance/teardown.h"

#include "lua.h"
#include "lualib.h"

#include <cstring>

namespace rbx {

namespace {

constexpr const char* kKeyMT = "ColorSequenceKeypoint";

int csk_index(lua_State* L) {
    const ColorSequenceKeypoint* v =
        static_cast<const ColorSequenceKeypoint*>(luaL_checkudata(L, 1, kKeyMT));
    const char* k = luaL_checkstring(L, 2);
    if (strcmp(k, "Time") == 0 || strcmp(k, "time") == 0) {
        lua_pushnumber(L, v->time);
        return 1;
    }
    if (strcmp(k, "Color") == 0 || strcmp(k, "color") == 0) {
        push_color3(L, v->color);
        return 1;
    }
    if (strcmp(k, "Envelope") == 0 || strcmp(k, "envelope") == 0) {
        lua_pushnumber(L, v->envelope);
        return 1;
    }
    lua_getmetatable(L, 1);
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

int csk_new(lua_State* L) {
    ColorSequenceKeypoint v;
    v.time = luaL_checknumber(L, 1);
    v.color = check_color3(L, 2);
    v.envelope = luaL_optnumber(L, 3, 0.0);
    push_colorsequencekeypoint(L, v);
    return 1;
}

int csk_eq(lua_State* L) {
    const ColorSequenceKeypoint a = check_colorsequencekeypoint(L, 1);
    const ColorSequenceKeypoint b = check_colorsequencekeypoint(L, 2);
    lua_pushboolean(L, a == b ? 1 : 0);
    return 1;
}

int cs_index(lua_State* L) {
    const char* k = luaL_checkstring(L, 2);
    if (strcmp(k, "Keypoints") == 0 || strcmp(k, "keypoints") == 0) {
        const ColorSequence v = check_colorsequence(L, 1); // via slab handle
        lua_newtable(L);
        int i = 1;
        for (const auto& kp : v.keys) {
            push_colorsequencekeypoint(L, kp);
            lua_rawseti(L, -2, i++);
        }
        return 1;
    }
    lua_getmetatable(L, 1);
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

// new(color) | new({keypoints})
int cs_new(lua_State* L) {
    ColorSequence v;
    if (lua_istable(L, 1)) {
        const size_t n = lua_objlen(L, 1);
        for (size_t i = 1; i <= n; i++) {
            lua_rawgeti(L, 1, (int)i);
            v.keys.push_back(check_colorsequencekeypoint(L, -1));
            lua_pop(L, 1);
        }
    } else {
        const Color3 c = check_color3(L, 1);
        v.keys.push_back(ColorSequenceKeypoint{0.0, c, 0.0});
    }
    push_colorsequence(L, v);
    return 1;
}

int cs_eq(lua_State* L) {
    const ColorSequence a = check_colorsequence(L, 1);
    const ColorSequence b = check_colorsequence(L, 2);
    lua_pushboolean(L, a == b ? 1 : 0);
    return 1;
}

} // namespace

void push_colorsequencekeypoint(lua_State* L, const ColorSequenceKeypoint& v) {
    void* p = lua_newuserdata(L, sizeof(ColorSequenceKeypoint));
    *static_cast<ColorSequenceKeypoint*>(p) = v;
    if (luaL_newmetatable(L, kKeyMT)) { // first time: fill it
        lua_newtable(L);
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, csk_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, csk_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushstring(L, kKeyMT); // typeof() parity
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

ColorSequenceKeypoint check_colorsequencekeypoint(lua_State* L, int idx) {
    return *static_cast<const ColorSequenceKeypoint*>(
        luaL_checkudata(L, idx, kKeyMT));
}

void push_colorsequence(lua_State* L, const ColorSequence& v) {
    Slab* s = slab(L);
    const auto idx = (uint32_t)s->color_sequences.size();
    s->color_sequences.push_back(std::make_unique<ColorSequence>(v));
    void* p = lua_newuserdata(L, sizeof(idx));
    *static_cast<uint32_t*>(p) = idx;
    if (luaL_newmetatable(L, "ColorSequence")) { // first time: fill it
        lua_newtable(L);
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, cs_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, cs_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushstring(L, "ColorSequence"); // typeof() parity
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

ColorSequence check_colorsequence(lua_State* L, int idx) {
    const uint32_t h =
        *static_cast<const uint32_t*>(luaL_checkudata(L, idx, "ColorSequence"));
    Slab* s = slab(L);
    if (h >= s->color_sequences.size() || !s->color_sequences[h])
        luaL_error(L, "ColorSequence handle out of range");
    return *s->color_sequences[h]; // deep copy out (C++-side values own theirs)
}

void create_colorsequence_class(lua_State* L) {
    push_colorsequence(L, ColorSequence{});
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, cs_new, "new");
    lua_setfield(L, -2, "new");
    lua_setglobal(L, "ColorSequence");

    push_colorsequencekeypoint(L, ColorSequenceKeypoint{});
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, csk_new, "new");
    lua_setfield(L, -2, "new");
    lua_setglobal(L, "ColorSequenceKeypoint");
}

} // namespace rbx
