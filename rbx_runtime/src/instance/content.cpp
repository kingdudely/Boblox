// content.cpp — Content value type (see value_types.h).
//
// Docs-verified surface: fromUri, fromAssetId, fromObject, none, Uri,
// Object. SourceType returns the Enum.ContentSourceType VALUE
// (None=0/Uri=1/Object=2, per api_dump.json) until the Enum kind lands —
// a numeric bridge, documented at the call site.
#include "instance/value_types.h"

#include "instance/instance.h" // fromObject (Instance userdata)
#include "instance/teardown.h" // string member: explicit destruction

#include "lua.h"
#include "lualib.h"

#include <cstring>

namespace rbx {

namespace {

// Enum.ContentSourceType values (tools/api_dump.json).
constexpr int kSourceNone = 0;
constexpr int kSourceUri = 1;
constexpr int kSourceObject = 2;

int ct_index(lua_State* L) {
    const Content v = check_content(L, 1); // via slab handle (validates first)
    const char* k = luaL_checkstring(L, 2);
    if (strcmp(k, "Uri") == 0 || strcmp(k, "uri") == 0) {
        if (v.source == kSourceUri)
            lua_pushlstring(L, v.uri.data(), v.uri.size());
        else
            lua_pushnil(L);
        return 1;
    }
    if (strcmp(k, "Object") == 0 || strcmp(k, "object") == 0) {
        if (v.source == kSourceObject && v.object)
            Instance::push(L, v.object);
        else
            lua_pushnil(L);
        return 1;
    }
    if (strcmp(k, "SourceType") == 0 || strcmp(k, "sourceType") == 0) {
        lua_pushnumber(L, v.source); // numeric bridge until Enum lands
        return 1;
    }
    lua_getmetatable(L, 1);
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

int ct_fromuri(lua_State* L) {
    Content v;
    v.source = kSourceUri;
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    v.uri.assign(s, len);
    push_content(L, v);
    return 1;
}

int ct_fromassetid(lua_State* L) {
    // No AssetId kind exists (ContentSourceType has none): asset ids address
    // the rbxassetid:// URI scheme, so this is a Uri content by construction.
    const long long id = (long long)luaL_checknumber(L, 1);
    Content v;
    v.source = kSourceUri;
    v.uri = "rbxassetid://" + std::to_string(id);
    push_content(L, v);
    return 1;
}

int ct_fromobject(lua_State* L) {
    Content v;
    v.source = kSourceObject;
    v.object = Instance::check(L, 1);
    push_content(L, v);
    return 1;
}

int ct_eq(lua_State* L) {
    const Content a = check_content(L, 1);
    const Content b = check_content(L, 2);
    lua_pushboolean(L, a == b ? 1 : 0);
    return 1;
}

} // namespace

void push_content(lua_State* L, const Content& v) {
    Slab* s = slab(L);
    const auto idx = (uint32_t)s->contents.size();
    s->contents.push_back(std::make_unique<Content>(v));
    void* p = lua_newuserdata(L, sizeof(idx));
    *static_cast<uint32_t*>(p) = idx;
    if (luaL_newmetatable(L, "Content")) { // first time: fill it
        lua_newtable(L);
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, ct_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, ct_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushstring(L, "Content"); // typeof() == "Content" (Roblox parity)
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

Content check_content(lua_State* L, int idx) {
    const uint32_t h =
        *static_cast<const uint32_t*>(luaL_checkudata(L, idx, "Content"));
    Slab* s = slab(L);
    if (h >= s->contents.size() || !s->contents[h])
        luaL_error(L, "Content handle out of range");
    return *s->contents[h]; // deep copy out (C++-side values own theirs)
}

void create_content_class(lua_State* L) {
    push_content(L, Content{});
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, ct_fromuri, "fromUri");
    lua_setfield(L, -2, "fromUri");
    lua_pushcfunction(L, ct_fromassetid, "fromAssetId");
    lua_setfield(L, -2, "fromAssetId");
    lua_pushcfunction(L, ct_fromobject, "fromObject");
    lua_setfield(L, -2, "fromObject");
    push_content(L, Content{}); // none
    lua_setfield(L, -2, "none");
    lua_setglobal(L, "Content");
}

} // namespace rbx
