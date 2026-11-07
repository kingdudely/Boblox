// methods.cpp — hand-written Instance behaviors (the ones scripts actually
// call). Everything attaches by NAME onto generated-or-hand method slots
// (attach_method, upserting), so behavior arrives without per-class files:
//
//   * the universal Instance surface: WaitForChild, GetDescendants, Clone,
//     ClearAllChildren, IsAncestorOf/IsDescendantOf, FindFirstAncestor[OfClass/
//     WhichIsA], GetFullName, Get/Set/GetAttributes + GetAttributeChangedSignal
//   * Humanoid:TakeDamage (+ Died firing) — the flagship attach-onto-generated
//     proof: Humanoid itself is a generated row; only the behavior is hand code.
//
// Conventions: methods receive (self, args...) with self checked at index 1;
// errors use luaL_error (statement form — Luau's is noreturn-void, so never
// `return luaL_error(...)`). Blocking calls subscribe instead of looping:
// WaitForChild parks in the scheduler (single correctly-propagated yield)
// and the resume carries the answer — C never resumes mid-body (Luau has no
// continuations).
#include "api/api.h" // push_service: GetService fallback (RunService, …)
#include "instance/class_registry.h"
#include "instance/frame_clock.h"
#include "instance/instance.h"
#include "instance/remote.h"
#include "instance/scheduler.h"
#include "instance/signal.h"
#include "instance/tween.h"
#include "instance/value_types.h"
#include "instance/vector3.h"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace rbx {

namespace {

// Ancestor walk with a predicate; used by IsAncestorOf/FindFirstAncestor*.
template <typename Pred>
Instance* walk_up(Instance* inst, Pred pred) {
    for (Instance* a = inst->parent(); a; a = a->parent()) {
        if (pred(a))
            return a;
    }
    return nullptr;
}

void collect_descendants(Instance* inst, std::vector<Instance*>& out) {
    for (const auto& c : inst->children()) {
        out.push_back(c.get());
        collect_descendants(c.get(), out);
    }
}

// Attribute values from Lua: the scriptable scalar/vector/instance set.
// Returns false for nil (means REMOVE) via removed=true, or unsupported
// types (function/table/thread) — the caller errors those.
bool attr_from_lua(lua_State* L, int idx, Variant& out, bool& removed) {
    removed = false;
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        removed = true;
        return true;
    case LUA_TBOOLEAN:
        out = Variant(lua_toboolean(L, idx) != 0);
        return true;
    case LUA_TNUMBER:
        out = Variant(double(lua_tonumber(L, idx)));
        return true;
    case LUA_TSTRING: {
        size_t len = 0;
        const char* s = lua_tolstring(L, idx, &len);
        out = Variant(std::string(s, len));
        return true;
    }
    default:
        break;
    }
    if (!lua_getmetatable(L, idx))
        return false; // no metatable: function/table/thread
    lua_getfield(L, -1, "__type");
    const char* t = lua_tostring(L, -1);
    const std::string type = t ? t : "";
    lua_pop(L, 2);
    // The __type name doubles as the attribute type tag; check_variant does
    // the actual marshalling (strict per-type, EnumItems accept any enum).
    static const std::pair<const char*, PropType> kTypes[] = {
        {"Vector3", PropType::Vector3}, {"Color3", PropType::Color3},
        {"CFrame", PropType::CFrame}, {"Vector2", PropType::Vector2},
        {"BrickColor", PropType::BrickColor}, {"UDim", PropType::UDim},
        {"UDim2", PropType::UDim2}, {"Rect", PropType::Rect},
        {"NumberRange", PropType::NumberRange},
        {"NumberSequence", PropType::NumberSequence},
        {"ColorSequence", PropType::ColorSequence},
        {"Content", PropType::Content},
        {"PhysicalProperties", PropType::PhysicalProperties},
        {"Ray", PropType::Ray}, {"Region3", PropType::Region3},
        {"DateTime", PropType::DateTime}, {"EnumItem", PropType::Enum},
        {"Instance", PropType::Instance}, {"Font", PropType::Font},
    };
    for (const auto& [tag, pt] : kTypes) {
        if (type == tag) {
            out = check_variant(L, idx, pt);
            return true;
        }
    }
    return false;
}

// DataModel:GetService(name). Services replicated into the DataModel are real
// children, so look there first (identity is the child, like the native
// client); client-only services (RunService) fall back to the engine registry.
// The lowercase "workspace" spelling is accepted as an alias.
int m_datamodel_get_service(lua_State* L) {
    Instance* dm = Instance::check(L, 1);
    const char* name = luaL_checkstring(L, 2);
    Instance* svc = dm->find_first_child_of_class(name);
    if (!svc)
        svc = dm->find_first_child(name);
    if (!svc && std::strcmp(name, "workspace") == 0)
        svc = dm->find_first_child_of_class("Workspace");
    if (svc) {
        Instance::push(L, svc);
        return 1;
    }
    if (rbxch::api::push_service(L, name))
        return 1;
    // Unknown names stay a fresh empty table (the pre-DataModel contract;
    // challenge programs observe identity, so this must not be cached).
    lua_newtable(L);
    return 1;
}

int m_wait_for_child(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    const char* name = luaL_checkstring(L, 2);
    if (name[0] == '\0') { // native: "WaitForChild called with an empty child name."
        luaL_error(L, "WaitForChild called with an empty child name.");
        return 0;
    }
    const bool has_timeout = !lua_isnoneornil(L, 3);
    const double timeout = luaL_optnumber(L, 3, 0.0);
    if (Instance* c = inst->find_first_child(name)) {
        Instance::push(L, c);
        return 1;
    }
    if (has_timeout && !(timeout > 0.0)) { // timeout <= 0 (or NaN): miss now
        lua_pushnil(L);
        return 1;
    }
    // Park until notify_child_added (or deadline); the resume carries the
    // child (or nil). Single yield, correctly propagated — no loop: Luau
    // has no continuations, so a C function never resumes mid-body.
    scheduler(L)->wait_child(
        L, inst, name,
        has_timeout ? timeout : std::numeric_limits<double>::infinity());
    return lua_yield(L, 0);
}

int m_get_descendants(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    std::vector<Instance*> out;
    collect_descendants(inst, out);
    lua_newtable(L);
    int i = 1;
    for (Instance* c : out) {
        Instance::push(L, c);
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

int m_clone(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    std::string err;
    Ref<Instance> c = inst->clone(&err);
    if (!c.get()) {
        luaL_error(L, "%s", err.c_str());
        return 0;
    }
    Instance::push(L, c.get());
    return 1;
}

int m_clear_all_children(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    const std::vector<Ref<Instance>> kids(inst->children().begin(), inst->children().end());
    for (const auto& k : kids)
        k->destroy();
    return 0;
}

int m_is_ancestor_of(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    Instance* other = Instance::check(L, 2);
    lua_pushboolean(L, walk_up(other, [&](Instance* a) { return a == inst; }) ? 1 : 0);
    return 1;
}

int m_is_descendant_of(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    Instance* other = Instance::check(L, 2);
    lua_pushboolean(L, walk_up(inst, [&](Instance* a) { return a == other; }) ? 1 : 0);
    return 1;
}

Instance* find_descendant_named(Instance* inst, const std::string& name) {
    std::vector<Instance*> stack;
    const auto& kids = inst->children();
    for (size_t i = kids.size(); i-- > 0;)
        stack.push_back(kids[i].get());
    while (!stack.empty()) {
        Instance* cur = stack.back();
        stack.pop_back();
        if (cur->name() == name)
            return cur;
        const auto& cs = cur->children();
        for (size_t i = cs.size(); i-- > 0;)
            stack.push_back(cs[i].get());
    }
    return nullptr;
}

Instance* find_first_child_which_is_a(Instance* inst, const std::string& cls,
                                      bool recursive) {
    for (const auto& c : inst->children())
        if (c->is_a(cls))
            return c.get();
    if (!recursive)
        return nullptr;
    std::vector<Instance*> stack;
    const auto& kids = inst->children();
    for (size_t i = kids.size(); i-- > 0;)
        stack.push_back(kids[i].get());
    while (!stack.empty()) {
        Instance* cur = stack.back();
        stack.pop_back();
        if (cur->is_a(cls))
            return cur;
        const auto& cs = cur->children();
        for (size_t i = cs.size(); i-- > 0;)
            stack.push_back(cs[i].get());
    }
    return nullptr;
}

int m_find_first_child_alias(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    const char* name = luaL_checkstring(L, 2);
    const bool recursive = lua_toboolean(L, 3) != 0;
    Instance* hit = recursive ? find_descendant_named(inst, name)
                              : inst->find_first_child(name);
    if (hit)
        Instance::push(L, hit);
    else
        lua_pushnil(L);
    return 1;
}

int m_get_children_alias(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    lua_newtable(L);
    int i = 1;
    for (const auto& c : inst->children()) {
        Instance::push(L, c.get());
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

int m_is_a_alias(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    lua_pushboolean(L, inst->is_a(luaL_checkstring(L, 2)) ? 1 : 0);
    return 1;
}

int m_find_first_child_which_is_a_method(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    const char* cls = luaL_checkstring(L, 2);
    const bool recursive = lua_toboolean(L, 3) != 0;
    Instance* hit = find_first_child_which_is_a(inst, cls, recursive);
    if (hit)
        Instance::push(L, hit);
    else
        lua_pushnil(L);
    return 1;
}

int m_find_first_descendant(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    Instance* hit = find_descendant_named(inst, luaL_checkstring(L, 2));
    if (hit)
        Instance::push(L, hit);
    else
        lua_pushnil(L);
    return 1;
}

int m_find_first_ancestor(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    const char* name = luaL_checkstring(L, 2);
    Instance* hit = walk_up(inst, [&](Instance* a) { return a->name() == name; });
    if (hit)
        Instance::push(L, hit);
    else
        lua_pushnil(L);
    return 1;
}

int m_find_first_ancestor_of_class(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    const char* cls = luaL_checkstring(L, 2);
    Instance* hit = walk_up(inst, [&](Instance* a) {
        return a->class_info() && a->class_info()->name == cls;
    });
    if (hit)
        Instance::push(L, hit);
    else
        lua_pushnil(L);
    return 1;
}

int m_find_first_ancestor_which_is_a(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    const char* cls = luaL_checkstring(L, 2);
    Instance* hit = walk_up(inst, [&](Instance* a) { return a->is_a(cls); });
    if (hit)
        Instance::push(L, hit);
    else
        lua_pushnil(L);
    return 1;
}

int m_get_debug_id(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    lua_pushfstring(L, "%s@%p", inst->class_name().c_str(), (void*)inst);
    return 1;
}

int m_is_property_modified(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    const char* name = luaL_checkstring(L, 2);
    const PropInfo* p = find_property(inst->class_info(), name);
    if (!p) {
        luaL_error(L, "%s is not a valid property of %s", name,
                   inst->class_name().c_str());
        return 0;
    }
    Variant current;
    if (!inst->get_prop(name, current)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, current == p->default_value ? 0 : 1);
    return 1;
}

int m_reset_property_to_default(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    const char* name = luaL_checkstring(L, 2);
    const PropInfo* p = find_property(inst->class_info(), name);
    if (!p) {
        luaL_error(L, "%s is not a valid property of %s", name,
                   inst->class_name().c_str());
        return 0;
    }
    if (p->readonly) {
        luaL_error(L, "Property %s is read only", name);
        return 0;
    }
    inst->set_prop(name, p->default_value, nullptr);
    inst->changed_signal(name)->fire(L);
    inst->event_signal("Changed")->fire(L, {Variant(std::string(name))});
    return 0;
}

int m_get_full_name(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    std::string path = inst->name();
    for (Instance* a = inst->parent(); a; a = a->parent())
        path = a->name() + "." + path;
    lua_pushlstring(L, path.data(), path.size());
    return 1;
}

int m_get_attribute(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    const char* name = luaL_checkstring(L, 2);
    Variant v;
    if (inst->get_attr(name, v))
        push_variant(L, v);
    else
        lua_pushnil(L);
    return 1;
}

int m_set_attribute(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    const char* name = luaL_checkstring(L, 2);
    Variant v;
    bool removed = false;
    if (!attr_from_lua(L, 3, v, removed)) {
        luaL_error(L, "unsupported attribute type for '%s'", name);
        return 0;
    }
    if (removed)
        inst->remove_attr(name);
    else
        inst->set_attr(name, v);
    inst->attr_changed_signal(name)->fire(L);
    return 0;
}

int m_get_attributes(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    lua_newtable(L);
    for (const std::string& name : inst->attr_names()) {
        Variant v;
        if (!inst->get_attr(name, v))
            continue;
        push_variant(L, v);
        lua_setfield(L, -2, name.c_str());
    }
    return 1;
}

int m_get_attribute_changed_signal(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    const char* name = luaL_checkstring(L, 2);
    Signal::push(L, inst->attr_changed_signal(name));
    return 1;
}

int m_players_get_players(lua_State* L) {
    Instance* self = Instance::check(L, 1);
    lua_newtable(L);
    int n = 0;
    for (const auto& c : self->children()) {
        if (!c->is_a("Player"))
            continue;
        Instance::push(L, c.get());
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int m_players_get_player_by_user_id(lua_State* L) {
    Instance* self = Instance::check(L, 1);
    const int64_t want = int64_t(luaL_checknumber(L, 2));
    for (const auto& c : self->children()) {
        if (!c->is_a("Player"))
            continue;
        Variant id;
        if (!c->get_prop("UserId", id))
            continue;
        if (const auto* v = std::get_if<int64_t>(&id); v && *v == want) {
            Instance::push(L, c.get());
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

int m_players_get_player_from_character(lua_State* L) {
    Instance* self = Instance::check(L, 1);
    if (lua_isnoneornil(L, 2)) {
        lua_pushnil(L);
        return 1;
    }
    Instance* character = Instance::check(L, 2);
    for (const auto& c : self->children()) {
        if (!c->is_a("Player"))
            continue;
        Variant ch;
        if (!c->get_prop("Character", ch))
            continue;
        if (const auto* p = std::get_if<Instance*>(&ch); p && *p == character) {
            Instance::push(L, c.get());
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

// LocalScript:AwaitPersistentModels(). This is a CoreScript-only yield that
// is absent from the public API dump (the native client still exposes it: a
// live Adopt Me LocalScript, a copy of Roblox's InitialClientLoad, calls it at
// line 16 and aborts the whole loader when the name does not resolve). It
// yields until Workspace's persistent models are resident; this client has no
// model streaming, so the models are already there and there is nothing to
// wait for. Returns no values, exactly like the native on a fully loaded job.
int m_localscript_await_persistent_models(lua_State* L) {
    Instance::check(L, 1);
    return 0;
}

int m_player_get_network_ping(lua_State* L) {
    Instance::check(L, 1);
    lua_pushnumber(L, 0.0); // local transport has no server ping sample yet
    return 1;
}

void push_render_steps_table(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kRenderStepsKey);
    if (lua_istable(L, -1))
        return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, kRenderStepsKey);
}

int m_runservice_bind_render_step(lua_State* L) {
    Instance::check(L, 1);
    const char* name = luaL_checkstring(L, 2); // step name
    const double priority = luaL_checknumber(L, 3); // RenderPriority value
    luaL_checktype(L, 4, LUA_TFUNCTION);

    push_render_steps_table(L);
    const int next = int(lua_objlen(L, -1)) + 1;
    lua_createtable(L, 0, 3);
    lua_pushstring(L, name);
    lua_setfield(L, -2, "name");
    lua_pushnumber(L, priority);
    lua_setfield(L, -2, "priority");
    lua_pushvalue(L, 4); // the handler
    lua_setfield(L, -2, "fn");
    lua_rawseti(L, -2, next); // table[next] = entry
    lua_pop(L, 1);
    return 0;
}

int m_runservice_unbind_render_step(lua_State* L) {
    Instance::check(L, 1);
    clear_render_steps(L, luaL_checkstring(L, 2));
    return 0;
}

struct PartHit {
    Instance* part = nullptr;
    double t = 0.0;
    double px = 0.0, py = 0.0, pz = 0.0;
    double nx = 0.0, ny = 0.0, nz = 0.0;
};

void collect_base_parts(Instance* inst, std::vector<Instance*>* out) {
    for (const auto& c : inst->children()) {
        if (c->is_a("BasePart"))
            out->push_back(c.get());
        collect_base_parts(c.get(), out);
    }
}

bool part_cframe_size(Instance* part, rbx::CFrame* cf, rbx::Vector3* size) {
    Variant cv, sv;
    if (!part->get_prop("CFrame", cv) || !part->get_prop("Size", sv))
        return false;
    const auto* c = std::get_if<rbx::CFrame>(&cv);
    const auto* s = std::get_if<rbx::Vector3>(&sv);
    if (!c || !s)
        return false;
    *cf = *c;
    *size = *s;
    return true;
}

// Ray vs oriented box. Transforms the ray into part space with the CFrame
// rotation (transpose), runs the standard slab test, then transforms the hit
// normal back. Rotation is real, not axis-aligned approximation.
bool ray_part(Instance* part, const rbx::Ray& ray, PartHit* hit) {
    rbx::CFrame cf;
    rbx::Vector3 sz;
    if (!part_cframe_size(part, &cf, &sz))
        return false;
    const double ox = ray.origin.x - cf.x;
    const double oy = ray.origin.y - cf.y;
    const double oz = ray.origin.z - cf.z;
    const double lo[3] = {
        cf.r00 * ox + cf.r10 * oy + cf.r20 * oz,
        cf.r01 * ox + cf.r11 * oy + cf.r21 * oz,
        cf.r02 * ox + cf.r12 * oy + cf.r22 * oz};
    const double ld[3] = {
        cf.r00 * ray.direction.x + cf.r10 * ray.direction.y + cf.r20 * ray.direction.z,
        cf.r01 * ray.direction.x + cf.r11 * ray.direction.y + cf.r21 * ray.direction.z,
        cf.r02 * ray.direction.x + cf.r12 * ray.direction.y + cf.r22 * ray.direction.z};
    const double half[3] = {sz.x * 0.5, sz.y * 0.5, sz.z * 0.5};
    double tmin = -std::numeric_limits<double>::infinity();
    double tmax = std::numeric_limits<double>::infinity();
    int axis = -1;
    bool entering = true;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(ld[i]) < 1e-12) {
            if (lo[i] < -half[i] || lo[i] > half[i])
                return false;
            continue;
        }
        double t1 = (-half[i] - lo[i]) / ld[i];
        double t2 = (half[i] - lo[i]) / ld[i];
        bool flip = t1 > t2;
        if (flip) std::swap(t1, t2);
        if (t1 > tmin) {
            tmin = t1;
            axis = i;
            entering = !flip;
        }
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return false;
    }
    double t = tmin;
    if (t < 0.0) {
        t = tmax;
        if (t < 0.0) return false;
        entering = false;
    }
    double ln[3] = {0, 0, 0};
    if (axis >= 0) {
        const double s = ld[axis] > 0 ? 1.0 : -1.0;
        ln[axis] = entering ? -s : s;
    }
    // local normal -> world (rotation columns)
    const double nx = cf.r00 * ln[0] + cf.r01 * ln[1] + cf.r02 * ln[2];
    const double ny = cf.r10 * ln[0] + cf.r11 * ln[1] + cf.r12 * ln[2];
    const double nz = cf.r20 * ln[0] + cf.r21 * ln[1] + cf.r22 * ln[2];
    hit->part = part;
    hit->t = t;
    hit->px = ray.origin.x + ray.direction.x * t;
    hit->py = ray.origin.y + ray.direction.y * t;
    hit->pz = ray.origin.z + ray.direction.z * t;
    hit->nx = nx;
    hit->ny = ny;
    hit->nz = nz;
    return true;
}

bool instance_in_table(lua_State* L, int table, Instance* part) {
    if (!lua_istable(L, table))
        return false;
    const int n = int(lua_objlen(L, table));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, table, i);
        Instance* cand = Instance::get(L, -1);
        lua_pop(L, 1);
        if (cand == part)
            return true;
    }
    return false;
}

int m_workspace_find_part_on_ray(lua_State* L) {
    Instance* ws = Instance::check(L, 1);
    const rbx::Ray ray = check_ray(L, 2);
    const bool ignore_list = !lua_isnoneornil(L, 3);
    const bool white_list = !lua_isnoneornil(L, 4);
    std::vector<Instance*> parts;
    collect_base_parts(ws, &parts);
    PartHit best;
    for (Instance* p : parts) {
        if (ignore_list && instance_in_table(L, 3, p))
            continue;
        if (white_list && !instance_in_table(L, 4, p))
            continue;
        PartHit h;
        if (ray_part(p, ray, &h) && (!best.part || h.t < best.t))
            best = h;
    }
    if (!best.part) {
        lua_pushnil(L);
        lua_pushnil(L);
        lua_pushnil(L);
        return 3;
    }
    Instance::push(L, best.part);
    lua_pushnumber(L, best.px);
    lua_pushnumber(L, best.py);
    lua_pushnumber(L, best.pz);
    lua_pushnumber(L, best.nx);
    lua_pushnumber(L, best.ny);
    lua_pushnumber(L, best.nz);
    return 7;
}

int m_workspace_find_parts_in_region3(lua_State* L) {
    Instance* ws = Instance::check(L, 1);
    const rbx::Region3 region = check_region3(L, 2);
    const bool ignore_list = !lua_isnoneornil(L, 3);
    std::vector<Instance*> parts;
    collect_base_parts(ws, &parts);
    lua_newtable(L);
    int n = 0;
    for (Instance* p : parts) {
        if (ignore_list && instance_in_table(L, 3, p))
            continue;
        rbx::CFrame cf;
        rbx::Vector3 sz;
        if (!part_cframe_size(p, &cf, &sz))
            continue;
        if (cf.x < region.min.x || cf.x > region.max.x ||
            cf.y < region.min.y || cf.y > region.max.y ||
            cf.z < region.min.z || cf.z > region.max.z)
            continue;
        Instance::push(L, p);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int m_workspace_is_region3_empty(lua_State* L) {
    Instance* ws = Instance::check(L, 1);
    const rbx::Region3 region = check_region3(L, 2);
    std::vector<Instance*> parts;
    collect_base_parts(ws, &parts);
    for (Instance* p : parts) {
        rbx::CFrame cf;
        rbx::Vector3 sz;
        if (!part_cframe_size(p, &cf, &sz))
            continue;
        if (cf.x >= region.min.x && cf.x <= region.max.x &&
            cf.y >= region.min.y && cf.y <= region.max.y &&
            cf.z >= region.min.z && cf.z <= region.max.z) {
            lua_pushboolean(L, 0);
            return 1;
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

int m_workspace_raycast(lua_State* L) {
    Instance* ws = Instance::check(L, 1);
    rbx::Ray ray;
    // Raycast(origin, direction, params): origin/direction are Vector3s.
    const rbx::Vector3 origin = check_vector3(L, 2);
    const rbx::Vector3 direction = check_vector3(L, 3);
    ray.origin = origin;
    ray.direction = direction;
    std::vector<Instance*> parts;
    collect_base_parts(ws, &parts);
    PartHit best;
    for (Instance* p : parts) {
        PartHit h;
        if (ray_part(p, ray, &h) && (!best.part || h.t < best.t))
            best = h;
    }
    if (!best.part) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    Instance::push(L, best.part);
    lua_setfield(L, -2, "Instance");
    lua_pushnumber(L, best.px); lua_setfield(L, -2, "X");
    lua_pushnumber(L, best.py); lua_setfield(L, -2, "Y");
    lua_pushnumber(L, best.pz); lua_setfield(L, -2, "Z");
    lua_pushnumber(L, best.t); lua_setfield(L, -2, "Distance");
    rbx::push_vector3(L, rbx::Vector3{best.px, best.py, best.pz});
    lua_setfield(L, -2, "Position");
    rbx::push_vector3(L, rbx::Vector3{best.nx, best.ny, best.nz});
    lua_setfield(L, -2, "Normal");
    return 1;
}

int m_workspace_get_real_physics_fps(lua_State* L) {
    Instance::check(L, 1);
    lua_pushnumber(L, 60.0);
    return 1;
}

int m_workspace_get_num_awake_parts(lua_State* L) {
    Instance::check(L, 1);
    lua_pushinteger(L, 0);
    return 1;
}

int m_workspace_get_physics_throttling(lua_State* L) {
    Instance::check(L, 1);
    lua_pushinteger(L, 0);
    return 1;
}

int m_workspace_get_server_time_now(lua_State* L) {
    Instance::check(L, 1);
    lua_pushnumber(L, scheduler(L)->now);
    return 1;
}

int m_workspace_calc_jump_height(lua_State* L) {
    Instance::check(L, 1);
    const double gravity = luaL_checknumber(L, 2);
    const double jump_power = luaL_checknumber(L, 3);
    lua_pushnumber(L, gravity > 0 ? (jump_power * jump_power) / (2 * gravity)
                                  : 0.0);
    return 1;
}

int m_workspace_calc_jump_distance(lua_State* L) {
    Instance::check(L, 1);
    const double gravity = luaL_checknumber(L, 2);
    const double jump_power = luaL_checknumber(L, 3);
    const double walk_speed = luaL_checknumber(L, 4);
    lua_pushnumber(L, gravity > 0 ? walk_speed * (2 * jump_power) / gravity
                                  : 0.0);
    return 1;
}

int m_workspace_calc_jump_power(lua_State* L) {
    Instance::check(L, 1);
    const double gravity = luaL_checknumber(L, 2);
    const double jump_height = luaL_checknumber(L, 3);
    lua_pushnumber(L, std::sqrt(2 * gravity * jump_height));
    return 1;
}

int m_runservice_is_client(lua_State* L) {
    Instance::check(L, 1);
    lua_pushboolean(L, 1);
    return 1;
}

int m_runservice_is_server(lua_State* L) {
    Instance::check(L, 1);
    lua_pushboolean(L, 0);
    return 1;
}

int m_bindable_event_fire(lua_State* L) {
    Instance* self = Instance::check(L, 1);
    std::vector<Variant> args;
    for (int i = 2; i <= lua_gettop(L); ++i) {
        switch (lua_type(L, i)) {
        case LUA_TBOOLEAN:
            args.emplace_back(lua_toboolean(L, i) != 0);
            break;
        case LUA_TNUMBER:
            args.emplace_back(double(lua_tonumber(L, i)));
            break;
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, i, &len);
            args.emplace_back(std::string(s, len));
            break;
        }
        default:
            // Signal::fire marshals Variants; arbitrary tables/functions
            // need the raw-stack path that the renderer-facing event bus
            // will add. Keep the failure legible instead of silently
            // dropping an argument.
            luaL_error(L, "BindableEvent:Fire argument %d has unsupported type %s",
                       i - 1, luaL_typename(L, i));
            return 0;
        }
    }
    if (find_event(self->class_info(), "Event"))
        self->event_signal("Event")->fire(L, args);
    return 0;
}

int m_player_distance_from_character(lua_State* L) {
    Instance* self = Instance::check(L, 1);
    const Variant pos = check_variant(L, 2, PropType::Vector3);
    const Vector3& target = std::get<Vector3>(pos);
    Variant ch;
    double dist = 0.0;
    if (self->get_prop("Character", ch)) {
        if (auto* model = std::get_if<Instance*>(&ch); model && *model) {
            Instance* root = (*model)->find_first_child("HumanoidRootPart");
            Variant cf;
            if (root && root->get_prop("CFrame", cf)) {
                if (const auto* frame = std::get_if<CFrame>(&cf)) {
                    const double dx = frame->x - target.x;
                    const double dy = frame->y - target.y;
                    const double dz = frame->z - target.z;
                    dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                }
            }
        }
    }
    lua_pushnumber(L, dist);
    return 1;
}

// ReplicatedFirst loading-screen gate. The default loading GUI is a client
// concern this headless runtime does not draw, but scripts still call the
// native API to dismiss it and then branch on the state. Keep the state on
// the service instance (an attribute, like any other dynamic value) so two
// environments never share it.
constexpr const char* kDefaultLoadingGuiRemoved = "DefaultLoadingGuiRemoved";

bool default_loading_gui_removed(Instance* inst) {
    Variant v;
    if (inst->get_attr(kDefaultLoadingGuiRemoved, v))
        if (const auto* b = std::get_if<bool>(&v))
            return *b;
    return false;
}

int m_replicated_first_remove_default_loading_screen(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    inst->set_attr(kDefaultLoadingGuiRemoved, Variant(true));
    if (find_event(inst->class_info(), "DefaultLoadingGuiRemoved"))
        inst->event_signal("DefaultLoadingGuiRemoved")->fire(L);
    if (find_event(inst->class_info(), "RemoveDefaultLoadingGuiSignal"))
        inst->event_signal("RemoveDefaultLoadingGuiSignal")->fire(L);
    return 0;
}

int m_replicated_first_is_default_loading_gui_removed(lua_State* L) {
    lua_pushboolean(L, default_loading_gui_removed(Instance::check(L, 1)));
    return 1;
}

int m_replicated_first_set_default_loading_gui_removed(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    inst->set_attr(kDefaultLoadingGuiRemoved,
                   Variant(lua_toboolean(L, 2) != 0));
    return 0;
}

int m_replicated_first_is_finished_replicating(lua_State* L) {
    // The script runs only after the snapshot has been applied and the world
    // reaches this environment, so by construction replication is finished.
    lua_pushboolean(L, 1);
    return 1;
}

// ---- StarterGui core-GUI switches ------------------------------------------
// Headless draws no core GUI, but real LocalScripts toggle these and read them
// back. Keep the switch on the StarterGui instance so two DataModels never
// share state; default matches ClientSettings (everything enabled).
std::string core_gui_attr(const EnumItem& e) {
    const std::string item = enum_item_name(e);
    return "__CoreGuiEnabled_" + e.enum_name + (item.empty() ? "" : "_" + item);
}

int m_starter_gui_set_core_gui_enabled(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    const EnumItem e = check_enumitem(L, 2);
    inst->set_attr(core_gui_attr(e), Variant(lua_toboolean(L, 3) != 0));
    return 0;
}

int m_starter_gui_get_core_gui_enabled(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    const EnumItem e = check_enumitem(L, 2);
    bool enabled = true;
    Variant v;
    if (inst->get_attr(core_gui_attr(e), v))
        if (const auto* b = std::get_if<bool>(&v))
            enabled = *b;
    lua_pushboolean(L, enabled);
    return 1;
}

// ---- PolicyService ----------------------------------------------------------
// The native call yields for a policy round-trip; the values a client can act
// on are the region/account gates below. No policy service is contacted by a
// headless client, so return the permissive defaults (same shape as Roblox).
int m_policy_service_get_policy_info(lua_State* L) {
    Instance::check(L, 1);
    lua_createtable(L, 0, 5);
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "ArePaidRandomItemsRestricted");
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "IsPaidItemTradingAllowed");
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "IsThirdPartyPurchaseAllowed");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "IsSubjectToChinaPolicies");
    lua_newtable(L);
    lua_setfield(L, -2, "AllowedExternalLinkReferences");
    return 1;
}

// ---- CollectionService tags -------------------------------------------------
void collect_tagged(Instance* inst, const std::string& tag,
                    std::vector<Instance*>& out) {
    if (inst->has_tag(tag))
        out.push_back(inst);
    for (const auto& c : inst->children())
        collect_tagged(c.get(), tag, out);
}

int m_collection_service_get_tagged(lua_State* L) {
    Instance::check(L, 1);
    const std::string tag = luaL_checkstring(L, 2);
    std::vector<Instance*> found;
    lua_getglobal(L, "game");
    if (Instance* root = Instance::get(L, -1)) {
        collect_tagged(root, tag, found);
    } else {
        // Standalone engine states (engine::create before attach) expose
        // `game` as a service table, not a DataModel. The tagged instances
        // still live under the service singletons, so walk those and dedupe.
        lua_pop(L, 1);
        lua_getfield(L, LUA_REGISTRYINDEX, "rbx.Services");
        std::vector<Instance*> seen;
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2)) {
                if (Instance* root = Instance::get(L, -1)) {
                    bool dup = false;
                    for (Instance* s : seen)
                        if (s == root)
                            dup = true;
                    if (!dup) {
                        seen.push_back(root);
                        collect_tagged(root, tag, found);
                    }
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        lua_getglobal(L, "game"); // restore the popped global
    }
    lua_pop(L, 1);
    lua_createtable(L, int(found.size()), 0);
    for (size_t i = 0; i < found.size(); ++i) {
        Instance::push(L, found[i]);
        lua_rawseti(L, -2, int(i + 1));
    }
    return 1;
}

// Defined with the CollectionService signal surface below (AddTag fires it).
Signal* tag_signal(lua_State* L, bool added, const char* tag, bool create);

int m_collection_service_add_tag(lua_State* L) {
    Instance::check(L, 1);
    Instance* target = Instance::check(L, 2);
    const char* tag = luaL_checkstring(L, 3);
    const bool fresh = !target->has_tag(tag);
    target->add_tag(tag);
    // Roblox fires only on an actual change (re-adding is a no-op).
    if (fresh)
        if (Signal* sig = tag_signal(L, true, tag, false))
            sig->fire(L, {Variant(target)});
    return 0;
}

int m_collection_service_remove_tag(lua_State* L) {
    Instance::check(L, 1);
    Instance* target = Instance::check(L, 2);
    const char* tag = luaL_checkstring(L, 3);
    if (target->remove_tag(tag))
        if (Signal* sig = tag_signal(L, false, tag, false))
            sig->fire(L, {Variant(target)});
    return 0;
}

int m_collection_service_has_tag(lua_State* L) {
    Instance::check(L, 1);
    lua_pushboolean(L, Instance::check(L, 2)->has_tag(luaL_checkstring(L, 3)));
    return 1;
}

int m_collection_service_get_tags(lua_State* L) {
    Instance::check(L, 1);
    const std::vector<std::string> tags = Instance::check(L, 2)->tags();
    lua_createtable(L, int(tags.size()), 0);
    for (size_t i = 0; i < tags.size(); ++i) {
        lua_pushlstring(L, tags[i].data(), tags[i].size());
        lua_rawseti(L, -2, int(i + 1));
    }
    return 1;
}

// Per-state tag signals. GetInstanceAddedSignal(tag) must return the SAME
// signal on every call and keep it alive after the handler drops it, so the
// table is anchored in the registry and keyed by tag name (same pattern as
// RunService's render-step table).
const char* const kTagAddedKey = "rbx.CollectionService.TagAdded";
const char* const kTagRemovedKey = "rbx.CollectionService.TagRemoved";

void push_tag_signal_table(lua_State* L, const char* reg_key) {
    lua_getfield(L, LUA_REGISTRYINDEX, reg_key);
    if (lua_istable(L, -1))
        return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, reg_key);
}

// Returns the tag's signal, creating it when `create`; the stack is left
// balanced (the registry table owns the userdata, so the Signal* stays valid
// for the environment's lifetime). nullptr when absent and create == false.
Signal* tag_signal(lua_State* L, bool added, const char* tag, bool create) {
    const char* reg_key = added ? kTagAddedKey : kTagRemovedKey;
    push_tag_signal_table(L, reg_key);
    lua_getfield(L, -1, tag);
    if (lua_isuserdata(L, -1)) {
        Signal* sig = Signal::check(L, -1);
        lua_pop(L, 2);
        return sig;
    }
    lua_pop(L, 1);
    if (!create) {
        lua_pop(L, 1);
        return nullptr;
    }
    Signal* sig =
        new Signal(std::string("CollectionService.") + tag +
                   (added ? ".Added" : ".Removed"));
    Signal::push(L, sig); // userdata (+strong ref)
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, tag); // table[tag] = userdata
    lua_pop(L, 2);            // userdata, table
    return sig;
}

// Desktop is never a ten-foot (console/TV) interface; core scripts branch on
// this to pick TV-sized UI, so answering correctly matters.
int m_gui_service_is_ten_foot_interface(lua_State* L) {
    Instance::check(L, 1);
    lua_pushboolean(L, 0);
    return 1;
}

int m_collection_service_get_added_signal(lua_State* L) {
    Instance::check(L, 1);
    Signal* sig = tag_signal(L, true, luaL_checkstring(L, 2), true);
    Signal::push(L, sig);
    return 1;
}

int m_collection_service_get_removed_signal(lua_State* L) {
    Instance::check(L, 1);
    Signal* sig = tag_signal(L, false, luaL_checkstring(L, 2), true);
    Signal::push(L, sig);
    return 1;
}

int m_take_damage(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    const double amount = luaL_checknumber(L, 2);
    double cur = 0.0;
    Variant h;
    if (inst->get_prop("Health", h)) {
        if (const auto* d = std::get_if<double>(&h))
            cur = *d;
        else if (const auto* i = std::get_if<int64_t>(&h))
            cur = (double)*i;
    }
    const double next = cur - amount;
    std::string err;
    if (!inst->set_prop("Health", Variant(next), &err)) {
        luaL_error(L, "%s", err.c_str());
        return 0;
    }
    inst->changed_signal("Health")->fire(L);
    if (next <= 0.0 && find_event(inst->class_info(), "Died"))
        inst->event_signal("Died")->fire(L);
    return 0;
}

} // namespace

void clear_render_steps(lua_State* L, const char* name) {
    if (!name)
        return;
    push_render_steps_table(L);
    const int n = int(lua_objlen(L, -1));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, i);
        bool match = false;
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "name");
            const char* binding = lua_tostring(L, -1);
            match = binding && std::string(binding) == name;
            lua_pop(L, 1);
        }
        if (match) {
            lua_pop(L, 1); // the entry
            lua_pushnil(L);
            lua_rawseti(L, -2, i);
        } else {
            lua_pop(L, 1); // the entry
        }
    }
    lua_pop(L, 1);
}

void run_render_steps(lua_State* L, double dt) {
    push_render_steps_table(L);
    const int n = int(lua_objlen(L, -1));
    struct Binding {
        double priority = 0.0;
        int index = 0;
    };
    // Rebinding a name replaces the previous function. The latest array slot
    // wins, matching the native "one binding per name" contract.
    std::map<std::string, Binding> latest;
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, i);
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "name");
            const char* name = lua_tostring(L, -1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "priority");
            Binding b;
            b.priority = lua_tonumber(L, -1);
            b.index = i;
            lua_pop(L, 1);
            if (name)
                latest[name] = b;
        }
        lua_pop(L, 1);
    }

    std::vector<Binding> order;
    order.reserve(latest.size());
    for (const auto& [name, b] : latest)
        order.push_back(b);
    std::stable_sort(order.begin(), order.end(),
                     [](const Binding& a, const Binding& b) {
                         if (a.priority != b.priority)
                             return a.priority < b.priority;
                         return a.index < b.index;
                     });

    for (const Binding& b : order) {
        lua_rawgeti(L, -1, b.index);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        lua_getfield(L, -1, "fn");
        lua_remove(L, -2); // [table, fn]
        if (lua_isfunction(L, -1)) {
            lua_pushnumber(L, dt);
            if (lua_pcall(L, 1, 0, 0) != 0)
                lua_pop(L, 1); // swallow handler errors, like signals
        } else {
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

void register_instance_methods() {
    // Universal Instance surface (inherited by every class via the chain).
    attach_method("Instance", "WaitForChild", m_wait_for_child);
    attach_method("Instance", "GetDescendants", m_get_descendants);
    attach_method("Instance", "Clone", m_clone);
    attach_method("Instance", "ClearAllChildren", m_clear_all_children);
    attach_method("Instance", "IsAncestorOf", m_is_ancestor_of);
    attach_method("Instance", "IsDescendantOf", m_is_descendant_of);
    attach_method("Instance", "findFirstChild", m_find_first_child_alias);
    attach_method("Instance", "getChildren", m_get_children_alias);
    attach_method("Instance", "isA", m_is_a_alias);
    attach_method("Instance", "isDescendantOf", m_is_descendant_of);
    attach_method("Instance", "FindFirstChildWhichIsA",
                  m_find_first_child_which_is_a_method);
    attach_method("Instance", "findFirstChildWhichIsA",
                  m_find_first_child_which_is_a_method);
    attach_method("Instance", "FindFirstDescendant", m_find_first_descendant);
    attach_method("Instance", "findFirstDescendant", m_find_first_descendant);
    attach_method("Instance", "FindFirstAncestor", m_find_first_ancestor);
    attach_method("Instance", "FindFirstAncestorOfClass", m_find_first_ancestor_of_class);
    attach_method("Instance", "FindFirstAncestorWhichIsA", m_find_first_ancestor_which_is_a);
    attach_method("Instance", "GetFullName", m_get_full_name);
    attach_method("Instance", "GetDebugId", m_get_debug_id);
    attach_method("Instance", "IsPropertyModified", m_is_property_modified);
    attach_method("Instance", "ResetPropertyToDefault",
                  m_reset_property_to_default);
    attach_method("Instance", "GetAttribute", m_get_attribute);
    attach_method("Instance", "SetAttribute", m_set_attribute);
    attach_method("Instance", "GetAttributes", m_get_attributes);
    attach_method("Instance", "GetAttributeChangedSignal", m_get_attribute_changed_signal);
    attach_method("DataModel", "GetService", m_datamodel_get_service);
    attach_method("LocalScript", "AwaitPersistentModels",
                  m_localscript_await_persistent_models);
    attach_method("Players", "GetPlayers", m_players_get_players);
    attach_method("Players", "GetPlayerByUserId",
                  m_players_get_player_by_user_id);
    attach_method("Players", "GetPlayerFromCharacter",
                  m_players_get_player_from_character);
    attach_method("Player", "GetNetworkPing", m_player_get_network_ping);
    attach_method("Player", "DistanceFromCharacter",
                  m_player_distance_from_character);
    attach_method("RunService", "BindToRenderStep",
                  m_runservice_bind_render_step);
    attach_method("RunService", "UnbindFromRenderStep",
                  m_runservice_unbind_render_step);
    attach_method("Workspace", "FindPartOnRay",
                  m_workspace_find_part_on_ray);
    attach_method("Workspace", "FindPartOnRayWithIgnoreList",
                  m_workspace_find_part_on_ray);
    attach_method("Workspace", "FindPartOnRayWithWhitelist",
                  m_workspace_find_part_on_ray);
    attach_method("Workspace", "FindPartsInRegion3",
                  m_workspace_find_parts_in_region3);
    attach_method("Workspace", "FindPartsInRegion3WithIgnoreList",
                  m_workspace_find_parts_in_region3);
    attach_method("Workspace", "FindPartsInRegion3WithWhiteList",
                  m_workspace_find_parts_in_region3);
    attach_method("Workspace", "IsRegion3Empty",
                  m_workspace_is_region3_empty);
    attach_method("Workspace", "IsRegion3EmptyWithIgnoreList",
                  m_workspace_is_region3_empty);
    attach_method("Workspace", "Raycast", m_workspace_raycast);
    attach_method("Workspace", "GetRealPhysicsFPS",
                  m_workspace_get_real_physics_fps);
    attach_method("Workspace", "GetNumAwakeParts",
                  m_workspace_get_num_awake_parts);
    attach_method("Workspace", "GetPhysicsThrottling",
                  m_workspace_get_physics_throttling);
    attach_method("Workspace", "GetServerTimeNow",
                  m_workspace_get_server_time_now);
    attach_method("Workspace", "CalculateJumpHeight",
                  m_workspace_calc_jump_height);
    attach_method("Workspace", "CalculateJumpDistance",
                  m_workspace_calc_jump_distance);
    attach_method("Workspace", "CalculateJumpPower",
                  m_workspace_calc_jump_power);
    attach_method("RunService", "IsClient", m_runservice_is_client);
    attach_method("RunService", "IsServer", m_runservice_is_server);
    attach_method("BindableEvent", "Fire", m_bindable_event_fire);
    attach_method("Humanoid", "TakeDamage", m_take_damage);
    attach_method("ReplicatedFirst", "RemoveDefaultLoadingScreen",
                  m_replicated_first_remove_default_loading_screen);
    attach_method("ReplicatedFirst", "IsDefaultLoadingGuiRemoved",
                  m_replicated_first_is_default_loading_gui_removed);
    attach_method("ReplicatedFirst", "SetDefaultLoadingGuiRemoved",
                  m_replicated_first_set_default_loading_gui_removed);
    attach_method("ReplicatedFirst", "IsFinishedReplicating",
                  m_replicated_first_is_finished_replicating);
    attach_method("StarterGui", "SetCoreGuiEnabled",
                  m_starter_gui_set_core_gui_enabled);
    attach_method("StarterGui", "GetCoreGuiEnabled",
                  m_starter_gui_get_core_gui_enabled);
    attach_method("PolicyService", "GetPolicyInfoForPlayerAsync",
                  m_policy_service_get_policy_info);
    attach_method("CollectionService", "GetTagged",
                  m_collection_service_get_tagged);
    attach_method("CollectionService", "AddTag",
                  m_collection_service_add_tag);
    attach_method("CollectionService", "RemoveTag",
                  m_collection_service_remove_tag);
    attach_method("CollectionService", "HasTag",
                  m_collection_service_has_tag);
    attach_method("CollectionService", "GetTags",
                  m_collection_service_get_tags);
    attach_method("CollectionService", "GetInstanceAddedSignal",
                  m_collection_service_get_added_signal);
    attach_method("CollectionService", "GetInstanceRemovedSignal",
                  m_collection_service_get_removed_signal);
    attach_method("GuiService", "IsTenFootInterface",
                  m_gui_service_is_ten_foot_interface);
    // TweenService:Create + the TweenBase playback surface (tween.cpp).
    attach_method("TweenService", "Create", m_tween_service_create);
    attach_method("Tween", "Play", m_tween_play);
    attach_method("Tween", "Pause", m_tween_pause);
    attach_method("Tween", "Cancel", m_tween_cancel);
    install_remote_methods();
}

} // namespace rbx
