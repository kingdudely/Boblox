// enums.cpp — the `Enum` global + EnumItem userdata (see value_types.h).
//
// Enum definitions come from the generated table (enums.inc, all 648 dump
// enums); this file builds the runtime side. Design notes:
//   * Enum objects wrap a pointer to the STATIC table (process-lifetime) —
//     no per-state storage, no teardown interaction. typeof() == "Enum".
//   * EnumItems carry (enum, value) and live in the per-state slab like
//     sequences (std::string member). typeof() == "EnumItem".
//   * The `Enum` root itself is userdata reporting "Enum" (Roblox parity).
#include "instance/value_types.h"

#include "instance/enums.inc"
#include "instance/teardown.h"

#include "lua.h"
#include "lualib.h"

#include <cstring>

namespace rbx {

namespace {

const EnumDef* find_enum(const char* name) {
    for (size_t i = 0; i < kEnumCount; i++) {
        if (std::strcmp(kEnums[i]->name, name) == 0)
            return kEnums[i];
    }
    return nullptr;
}

const EnumItemDef* find_item(const EnumDef* e, const char* name) {
    for (size_t i = 0; i < e->item_count; i++) {
        if (std::strcmp(e->items[i].name, name) == 0)
            return &e->items[i];
    }
    return nullptr;
}

int enum_getitems(lua_State* L) {
    const EnumDef* e = *static_cast<const EnumDef**>(luaL_checkudata(L, 1, "Enum"));
    lua_newtable(L);
    int i = 1;
    for (size_t k = 0; k < e->item_count; k++) {
        push_enumitem(L, EnumItem{e->name, e->items[k].value});
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

int enum_index(lua_State* L) {
    const EnumDef* e = *static_cast<const EnumDef**>(luaL_checkudata(L, 1, "Enum"));
    const char* k = luaL_checkstring(L, 2);
    if (const EnumItemDef* it = find_item(e, k)) {
        push_enumitem(L, EnumItem{e->name, it->value});
        return 1;
    }
    if (strcmp(k, "GetEnumItems") == 0) {
        lua_pushcfunction(L, enum_getitems, "GetEnumItems");
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int enum_eq(lua_State* L) {
    const EnumDef* a = *static_cast<const EnumDef**>(luaL_checkudata(L, 1, "Enum"));
    const EnumDef* b = *static_cast<const EnumDef**>(luaL_checkudata(L, 2, "Enum"));
    lua_pushboolean(L, a == b ? 1 : 0);
    return 1;
}

void push_enum(lua_State* L, const EnumDef* e) {
    void* p = lua_newuserdata(L, sizeof(e));
    *static_cast<const EnumDef**>(p) = e;
    if (luaL_newmetatable(L, "Enum")) { // first time: fill it
        lua_pushcfunction(L, enum_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, enum_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushstring(L, "Enum"); // typeof() == "Enum" (Roblox parity)
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

int root_index(lua_State* L) {
    luaL_checkudata(L, 1, "EnumRoot");
    const char* k = luaL_checkstring(L, 2);
    if (const EnumDef* e = find_enum(k)) {
        push_enum(L, e);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

int item_index(lua_State* L) {
    const EnumItem v = check_enumitem(L, 1);
    const char* k = luaL_checkstring(L, 2);
    if (strcmp(k, "Name") == 0 || strcmp(k, "name") == 0) {
        const EnumDef* e = find_enum(v.enum_name.c_str());
        const char* label = "";
        if (e) {
            for (size_t i = 0; i < e->item_count; i++) {
                if (e->items[i].value == v.value) {
                    label = e->items[i].name;
                    break;
                }
            }
        }
        lua_pushstring(L, label);
        return 1;
    }
    if (strcmp(k, "Value") == 0 || strcmp(k, "value") == 0) {
        lua_pushnumber(L, v.value);
        return 1;
    }
    lua_getmetatable(L, 1);
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

int item_eq(lua_State* L) {
    const EnumItem a = check_enumitem(L, 1);
    const EnumItem b = check_enumitem(L, 2);
    lua_pushboolean(L, a == b ? 1 : 0);
    return 1;
}

} // namespace

void push_enumitem(lua_State* L, const EnumItem& v) {
    Slab* s = slab(L);
    const auto idx = (uint32_t)s->enum_items.size();
    s->enum_items.push_back(std::make_unique<EnumItem>(v));
    void* p = lua_newuserdata(L, sizeof(idx));
    *static_cast<uint32_t*>(p) = idx;
    if (luaL_newmetatable(L, "EnumItem")) { // first time: fill it
        lua_newtable(L);
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, item_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, item_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushstring(L, "EnumItem"); // typeof() == "EnumItem" (Roblox parity)
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

EnumItem check_enumitem(lua_State* L, int idx) {
    const uint32_t h =
        *static_cast<const uint32_t*>(luaL_checkudata(L, idx, "EnumItem"));
    Slab* s = slab(L);
    if (h >= s->enum_items.size() || !s->enum_items[h])
        luaL_error(L, "EnumItem handle out of range");
    return *s->enum_items[h]; // deep copy out (C++-side values own theirs)
}

void create_enum_class(lua_State* L) {
    lua_newuserdata(L, 1); // root payload unused; metatable carries behavior
    if (luaL_newmetatable(L, "EnumRoot")) {
        lua_pushcfunction(L, root_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushstring(L, "Enum"); // typeof(Enum) == "Enum" (Roblox parity)
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
    lua_setglobal(L, "Enum");
}

} // namespace rbx
