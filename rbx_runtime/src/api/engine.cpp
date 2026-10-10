// engine.cpp — the Engine-profile API module: Instance.new, task, Vector3,
// and the `workspace` service.
//
// Tagged kEngineOnly in api/registry.cpp — the Challenge profile (the 0x9B
// solve path) never sees any of these globals; unit_profile.cpp pins that.
#include "api/api.h"

#include "instance/class_registry.h"
#include "instance/instance.h"
#include "instance/scheduler.h"
#include "instance/vector3.h"

#include "lua.h"
#include "lualib.h"

namespace rbxch {
namespace api {

namespace {

int instance_new(lua_State* L) {
    const char* cls_name = luaL_checkstring(L, 1);
    const rbx::ClassInfo* cls = rbx::find_class(cls_name);
    if (!cls) { // every registered class is constructible (Roblox parity)
        luaL_error(L, "%s is not a valid class name", cls_name);
        return 0;
    }
    if (!cls->creatable) { // native: "Unable to create an Instance of type 'X'"
        luaL_error(L, "Unable to create an Instance of type '%s'", cls_name);
        return 0;
    }
    rbx::Instance* parent = lua_isnoneornil(L, 2) ? nullptr : rbx::Instance::check(L, 2);
    rbx::Ref<rbx::Instance> inst = rbx::instantiate(cls);
    if (parent) { // attach through the tree API (one place owns parenting)
        std::string err;
        if (!inst->reparent(parent, &err)) {
            luaL_error(L, "%s", err.c_str());
            return 0;
        }
    }
    rbx::Instance::push(L, inst.get());
    return 1;
}

} // namespace

void register_engine(lua_State* L, const Options&) {
    rbx::register_builtin_classes();
    rbx::create_vector3_class(L);
    rbx::create_task_library(L);

    lua_newtable(L); // Instance.new(class [, parent])
    lua_pushcfunction(L, instance_new, "new");
    lua_setfield(L, -2, "new");
    lua_setglobal(L, "Instance");

    // workspace: one instance, both as the global alias and as the service.
    const rbx::ClassInfo* ws_cls = rbx::find_class("Workspace");
    rbx::Ref<rbx::Instance> ws = rbx::instantiate(ws_cls);
    rbx::Instance::push(L, ws.get()); // ... ws
    lua_pushvalue(L, -1);
    lua_setglobal(L, "workspace"); // global alias
    register_service(L, "workspace"); // game:GetService("workspace")
}

} // namespace api
} // namespace rbxch
