// services.cpp — the rbx.Services table backing game:GetService(name).
//
// Services are plain Lua tables (today) or Instance-backed objects (future
// DataModel work) stored under one registry key. A service module creates its
// table and calls register_service(L, "Name"); game.cpp dispatches lookups
// through push_service. Adding a service therefore never edits game.cpp.
//
// Behavior contract (verified by the regressions):
//   * GetService of a REGISTERED name always returns the same table.
//   * GetService of an UNKNOWN name returns a FRESH empty table per call —
//     deliberately not a shared stub, because challenge programs may compare
//     identity (`game:GetService("X") == game:GetService("X")`). Changing
//     this to a cached stub would alter observable behavior.
#include "api/api.h"

#include "instance/instance.h"

#include "lua.h"
#include "lualib.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace rbxch {
namespace api {

namespace {

const char* kServicesKey = "rbx.Services";

// Pushes the rbx.Services table, creating it on first use. Pops nothing else.
void push_services_table(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kServicesKey);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, kServicesKey);
    }
}

} // namespace

void register_service(lua_State* L, const char* name) {
    // Consumes the service table at the top of the stack.
    push_services_table(L);                // [service, services]
    lua_pushvalue(L, -2);                  // [service, services, service]
    lua_setfield(L, -2, name);             // [service, services]
    lua_pop(L, 2);                         // []
}

bool push_service(lua_State* L, const char* name) {
    push_services_table(L);                // [services]
    lua_getfield(L, -1, name);             // [services, service|nil]
    lua_remove(L, -2);                     // [service|nil]
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    return true;                           // [service]
}

void adopt_services(lua_State* L, rbx::Instance* root) {
    if (!L || !root)
        return;
    int seen = 0, adopted = 0, rebound = 0;
    push_services_table(L);                // [services]
    lua_pushnil(L);
    while (lua_next(L, -2)) {              // [services, key, value]
        ++seen;
        rbx::Instance* svc = rbx::Instance::get(L, -1);
        if (svc) {
            const std::string name = svc->class_name();
            rbx::Instance* replicated =
                root->find_first_child_of_class(name);
            if (!replicated)
                replicated = root->find_first_child(name);
            if (replicated) {
                // The replicated service is the real one: rebind the registry
                // to it so GetService(name) == root.name == the tree child.
                rbx::Instance::push(L, replicated); // [.., key, value, inst]
                register_service(L, name.c_str());  // consumes inst
                ++rebound;
            } else if (!svc->parent()) {
                std::string err;
                if (svc->reparent(root, &err))
                    ++adopted;
                else
                    std::fprintf(stderr, "adopt_services: %s: %s\n",
                                 name.c_str(), err.c_str());
            }
        }
        lua_pop(L, 1);                     // [services, key]
    }
    lua_pop(L, 1);                         // []
    if (std::getenv("RBX_LOG_SERVICES"))
        std::fprintf(stderr,
                     "adopt_services: seen=%d adopted=%d rebound=%d\n",
                     seen, adopted, rebound);
}

} // namespace api
} // namespace rbxch
