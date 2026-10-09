// teardown.cpp — see teardown.h (why Luau needs this).
#include "instance/teardown.h"

#include "instance/instance.h"
#include "instance/signal.h"

#include "lua.h"

#include <utility>
#include <vector>

namespace rbx {

namespace {

const char* kAliveKey = "rbx.Alive";

} // namespace

void track_alive(lua_State* L, void* obj, int tag) {
    lua_getfield(L, LUA_REGISTRYINDEX, kAliveKey);
    if (!lua_istable(L, -1)) { // first tracked object: create the table
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, kAliveKey);
    }
    lua_pushlightuserdata(L, obj); // [table, obj]   (key)
    lua_pushinteger(L, tag);       // [table, obj, tag] (value)
    lua_settable(L, -3);           // [table] (settable pops key AND value)
    lua_pop(L, 1);                 // []
}

void teardown_alive(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kAliveKey);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return; // nothing was ever created in this state
    }
    // Collect first — we must not run Lua while mutating the table.
    std::vector<std::pair<void*, int>> objs;
    lua_pushnil(L);
    while (lua_next(L, -2)) { // [table, key, tag]
        objs.emplace_back(lua_touserdata(L, -2), int(lua_tointeger(L, -1)));
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // the alive table (freed by lua_close anyway)

    // Pin every object first so nothing can implicitly auto-delete while we
    // tear the graph down (userdata-held Refs never release — see teardown.h).
    for (auto& [p, tag] : objs) {
        (void)tag;
        static_cast<RefCounted*>(p)->add_ref();
    }
    // Drop cross-references FIRST: force deletion in arbitrary order is then
    // safe (destructors of fully detached objects never touch other objects).
    for (auto& [p, tag] : objs) {
        if (tag == kAliveInstance)
            static_cast<Instance*>(p)->detach_all();
    }
    // Force-delete: userdata-held Refs never run their destructor (Luau has
    // no finalizers), so refcounts around us are stale by design.
    for (auto& [p, tag] : objs) {
        if (tag == kAliveInstance)
            delete static_cast<Instance*>(p);
        else
            delete static_cast<Signal*>(p);
    }
}

} // namespace rbx
