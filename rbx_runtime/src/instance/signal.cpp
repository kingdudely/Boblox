// signal.cpp — see signal.h.
#include "instance/signal.h"

#include "instance/class_registry.h" // push_variant (handler dispatch)
#include "instance/refs.h"
#include "instance/scheduler.h"
#include "instance/teardown.h"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cstring>

namespace rbx {

namespace {

constexpr const char* kMT = "RBXScriptSignal";

struct SignalUD {
    Ref<Signal> sig;
};

struct ConnUD {
    Ref<Signal> sig;
    uint64_t id = 0;
    // Connected state is queried from the Signal (authoritative — reflects
    // disconnects from any handle + consumed Once/Wait entries).
};

int conn_disconnect(lua_State* L) {
    auto* c = static_cast<ConnUD*>(lua_touserdata(L, 1));
    if (c->sig)
        c->sig->disconnect(L, c->id);
    return 0;
}

int conn_index(lua_State* L) {
    auto* c = static_cast<ConnUD*>(lua_touserdata(L, 1));
    const char* k = luaL_checkstring(L, 2);
    if (std::strcmp(k, "Connected") == 0) {
        // Authoritative: reflects disconnects from any handle + Once fires.
        lua_pushboolean(L, c->sig && c->sig->connected(c->id) ? 1 : 0);
        return 1;
    }
    if (std::strcmp(k, "Disconnect") == 0) {
        lua_pushcfunction(L, conn_disconnect, "Disconnect");
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

// NOTE: no __gc on the connection metatable — Luau's VM never invokes
// userdata finalizers (see instance/teardown.h); the Signal itself is
// released by teardown_alive / instance teardown at env close.

void push_connection(lua_State* L, Signal* sig, uint64_t id) {
    auto* ud = static_cast<ConnUD*>(lua_newuserdata(L, sizeof(ConnUD)));
    new (&ud->sig) Ref<Signal>(sig);
    ud->id = id;
    if (luaL_newmetatable(L, "RBXScriptConnection")) {
        lua_pushcfunction(L, conn_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushstring(L, "RBXScriptConnection"); // typeof() parity
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

// Signal methods (Connect/Once/Wait) ------------------------------------------------

int sig_connect_impl(lua_State* L, bool once) {
    Signal* sig = Signal::check(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const uint64_t id = sig->connect_ref(L, lua_gettop(L) >= 3 ? 3 : 2, once);
    push_connection(L, sig, id);
    return 1;
}

int sig_connect(lua_State* L) {
    return sig_connect_impl(L, false);
}

int sig_once(lua_State* L) {
    return sig_connect_impl(L, true);
}

int sig_wait(lua_State* L) {
    Signal* sig = Signal::check(L, 1);
    if (lua_pushthread(L) == 1) { // main thread cannot yield
        lua_pop(L, 1);
        luaL_error(L, "Wait must run inside a coroutine (task.spawn)"); return 0;
    }
    lua_pop(L, 1);
    lua_pushthread(L); // re-push for the anchor
    const int th_ref = ref_new(L); // anchor the waiter thread
    sig->add_waiter(th_ref);
    // Yield until Fire hands us to the scheduler.
    return lua_yield(L, 0);
}

} // namespace

uint64_t Signal::connect_ref(lua_State* L, int fn_idx, bool once) {
    // Anchor the handler in the refs table so it survives GC.
    lua_pushvalue(L, fn_idx);
    const int fn_ref = ref_new(L);
    Entry e;
    e.id = next_id_++;
    e.fn_ref = fn_ref;
    e.once = once;
    entries_.push_back(e);
    return e.id;
}

void Signal::add_waiter(int thread_ref) {
    Entry e;
    e.id = next_id_++;
    e.waiter_ref = thread_ref;
    entries_.push_back(e);
}

void Signal::disconnect(lua_State* L, uint64_t id) {
    for (Entry& e : entries_) {
        if (e.id == id && e.connected) {
            e.connected = false;
            ref_free(L, e.fn_ref);
            ref_free(L, e.waiter_ref);
        }
    }
}

bool Signal::connected(uint64_t id) const {
    for (const Entry& e : entries_) {
        if (e.id == id)
            return e.connected;
    }
    return false;
}

int Signal::take_waiter(uint64_t id) {
    for (Entry& e : entries_) {
        if (e.id == id && e.waiter_ref != 0) {
            const int ref = e.waiter_ref;
            e.waiter_ref = 0; // ownership moved to the scheduler
            e.connected = false;
            e.fn_ref = 0;
            return ref;
        }
    }
    return 0;
}

void Signal::fire(lua_State* L, const std::vector<Variant>& args) {
    // Copy: handlers may Connect/Disconnect during dispatch.
    std::vector<Entry> snapshot = entries_;
    for (const Entry& e : snapshot) {
        if (!e.connected)
            continue;
        if (e.waiter_ref != 0) {
            // Wait(): consume the entry and hand the thread ref to the
            // scheduler (resumed at the next pump).
            const int th_ref = take_waiter(e.id);
            if (th_ref != 0)
                scheduler(L)->defer_start_thread(L, th_ref);
            continue;
        }
        if (e.fn_ref == 0)
            continue;
        ref_push(L, e.fn_ref);
        for (const Variant& a : args)
            push_variant(L, a);
        if (lua_pcall(L, (int)args.size(), 0, 0) != 0)
            lua_pop(L, 1); // swallow handler errors (Roblox logs, doesn't crash)
        if (e.once)
            disconnect(L, e.id);
    }
}

// ---- Lua glue -------------------------------------------------------------------

void Signal::push(lua_State* L, Signal* sig) {
    auto* ud = static_cast<SignalUD*>(lua_newuserdata(L, sizeof(SignalUD)));
    new (&ud->sig) Ref<Signal>(sig);
    track_alive(L, sig, kAliveSignal); // deterministic teardown (teardown.h)
    create_metatable(L);
    lua_setmetatable(L, -2);
}

Signal* Signal::check(lua_State* L, int idx) {
    auto* ud = static_cast<SignalUD*>(luaL_checkudata(L, idx, kMT));
    return ud->sig.get();
}

void Signal::create_metatable(lua_State* L) {
    if (!luaL_newmetatable(L, kMT))
        return;
    // typeof() reads __type from the userdata's METATABLE (luaT_objtypenamestr):
    // set it here while the metatable is on top, before Methods is created.
    lua_pushstring(L, "RBXScriptSignal"); // typeof() parity
    lua_setfield(L, -2, "__type");
    lua_newtable(L); // Methods
    lua_pushcfunction(L, sig_connect, "Connect");
    lua_setfield(L, -2, "Connect");
    lua_pushcfunction(L, sig_once, "Once");
    lua_setfield(L, -2, "Once");
    lua_pushcfunction(L, sig_wait, "Wait");
    lua_setfield(L, -2, "Wait");
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, "Methods");
    lua_setfield(L, -2, "__index");
}

} // namespace rbx
