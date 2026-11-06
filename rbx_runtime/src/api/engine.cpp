// engine.cpp — the Engine-profile API module: Instance.new, task, Vector3,
// and the `workspace` service.
//
// Tagged kEngineOnly in api/registry.cpp — the Challenge profile (the 0x9B
// solve path) never sees any of these globals; unit_profile.cpp pins that.
#include "api/api.h"

#include "instance/class_registry.h"
#include "instance/instance.h"
#include "instance/scheduler.h"
#include "instance/value_types.h"
#include "instance/vector3.h"

#include <cstdio>
#include <string>
#include <vector>

#include "lua.h"
#include "lualib.h"

namespace rbxch {
namespace api {

namespace {

// Roblox's warn(...) global: tostring each argument, tab-separated, on one
// line. It is not a base-library function (print is), and real game scripts
// call it on every recoverable error path.
int warn_global(lua_State* L) {
    std::string out = "Warning:";
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        out += ' ';
        if (const char* s = lua_tostring(L, i))
            out.append(s);
        else if (lua_isboolean(L, i))
            out += lua_toboolean(L, i) ? "true" : "false";
        else if (lua_isnil(L, i))
            out += "nil";
        else
            out += luaL_typename(L, i);
    }
    std::fprintf(stderr, "%s\n", out.c_str());
    return 0;
}

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
        // Wake matching WaitForChild parks (event-driven, exact).
        rbx::scheduler(L)->notify_child_added(L, parent, inst.get());
    }
    rbx::Instance::push(L, inst.get());
    return 1;
}

} // namespace

namespace {

// Every instance in an externally applied tree must be tracked for
// teardown_alive(): that is what makes engine::destroy own them. Pushing
// creates the userdata + engine reference, then popping only drops the
// stack slot (Luau never runs userdata finalizers; teardown handles the
// object lifetime explicitly).
void track_tree(lua_State* L, rbx::Instance* inst) {
    if (!inst)
        return;
    rbx::Instance::push(L, inst);
    lua_pop(L, 1);
    for (const auto& child : inst->children())
        track_tree(L, child.get());
}

} // namespace

void attach_datamodel(lua_State* L, rbx::Instance* root) {
    if (!L || !root)
        return;
    rbx::register_builtin_classes();
    track_tree(L, root);

    rbx::Instance* ws = root->find_first_child_of_class("Workspace");
    if (!ws && root->is_a("Workspace"))
        ws = root;
    if (ws) {
        rbx::Instance::push(L, ws);      // [workspace]
        lua_pushvalue(L, -1);            // [workspace, workspace]
        lua_setglobal(L, "workspace");   // [workspace]
        register_service(L, "workspace"); // consumes [workspace]
    }

    // `game` is the DataModel instance, exactly like the native client. The
    // pre-attach table cannot answer GetDescendants/IsA/FindFirstChild, and
    // real LocalScripts use those on game constantly. GetService is a
    // DataModel method (methods.cpp); services are its children.
    root->set_internal_prop("JobId", rbx::Variant(opts(L).job));
    if (ws) {
        root->set_internal_prop("Workspace", rbx::Variant(ws));
        root->set_internal_prop("workspace", rbx::Variant(ws));
    }
    rbx::Instance::push(L, root);
    lua_setglobal(L, "game");

    for (const auto& child : root->children()) {
        const std::string& svc = child->class_name();
        if (svc.empty())
            continue;
        rbx::Instance::push(L, child.get()); // [service]
        lua_pushvalue(L, -1);                // [service, service]
        register_service(L, svc.c_str());    // [service]
        lua_pop(L, 1);
        if (svc == "Workspace") {
            rbx::Instance::push(L, child.get());
            lua_pushvalue(L, -1);
            register_service(L, "workspace");
            lua_pop(L, 1);
        }
    }

    // Every other engine service singleton (RunService, UserInputService, …)
    // becomes a child of game, and replicated services take over their
    // registry slot. Real DataModels hold all services as children; without
    // this `game.Players` is nil even though GetService("Players") works.
    adopt_services(L, root);

    // Optional GUI tree (PlayerGui, Workspace.CurrentCamera). The module is
    // off by default; scripts that never touch GUI pay nothing.
    if (opts(L).gui)
        attach_gui(L, root);
}

namespace {

// The Engine profile's RunService is a real client Instance: frame events
// (RenderStepped/Heartbeat/Stepped/...) live on it, and engine::step fires
// them once per frame. The Challenge profile keeps its frozen hand table.
void register_runservice_instance(lua_State* L) {
    const rbx::ClassInfo* cls = rbx::find_class("RunService");
    if (!cls)
        return;
    rbx::Ref<rbx::Instance> rs = rbx::instantiate(cls);
    rbx::Instance::push(L, rs.get()); // [runservice]
    register_service(L, "RunService"); // consumes [runservice]

    lua_getglobal(L, "game"); // [game]
    if (lua_istable(L, -1)) {
        rbx::Instance::push(L, rs.get()); // [game, runservice]
        lua_setfield(L, -2, "RunService");
    }
    lua_pop(L, 1);
}

} // namespace

void register_engine(lua_State* L, const Options&) {
    rbx::register_builtin_classes();
    attach_runservice_behavior();
    rbx::create_vector3_class(L);
    rbx::create_color3_class(L);
    rbx::create_cframe_class(L);
    rbx::create_vector2_class(L);
    rbx::create_brickcolor_class(L);
    rbx::create_udim_class(L);
    rbx::create_udim2_class(L);
    rbx::create_rect_class(L);
    rbx::create_numberrange_class(L);
    rbx::create_numbersequence_class(L);
    rbx::create_colorsequence_class(L);
    rbx::create_content_class(L);
    rbx::create_physicalproperties_class(L);
    rbx::create_ray_class(L);
    rbx::create_region3_class(L);
    rbx::create_datetime_class(L);
    rbx::create_enum_class(L);
    rbx::create_tweeninfo_class(L);
    rbx::create_task_library(L);
    // Font is a datatype, not a window: replicated GUI scripts call Font.new
    // even when nothing is drawn, so this is never gated by --gui.
    register_gui_datatypes(L);

    lua_newtable(L); // Instance.new(class [, parent])
    lua_pushcfunction(L, instance_new, "new");
    lua_setfield(L, -2, "new");
    lua_setglobal(L, "Instance");

    lua_pushcfunction(L, warn_global, "warn");
    lua_setglobal(L, "warn");

    // workspace: one instance, both as the global alias and as the service.
    const rbx::ClassInfo* ws_cls = rbx::find_class("Workspace");
    rbx::Ref<rbx::Instance> ws = rbx::instantiate(ws_cls);
    rbx::Instance::push(L, ws.get()); // ... ws
    lua_pushvalue(L, -1);
    lua_setglobal(L, "workspace"); // global alias
    register_service(L, "workspace"); // game:GetService("workspace")
    // Remaining services as stable singletons (Engine profile only —
    // Challenge states keep the fresh-table fallback).
    rbx::register_service_singletons(L);
    register_runservice_instance(L);
}

} // namespace api
} // namespace rbxch
