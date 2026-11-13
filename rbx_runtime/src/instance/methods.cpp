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
#include "instance/api_audit.h"
#include "instance/assembly.h"
#include "instance/audio_host.h"
#include "instance/class_registry.h"
#include "instance/host_input.h"
#include "instance/frame_clock.h"
#include "instance/gui_input.h"
#include "instance/instance.h"
#include "instance/input_object.h"
#include "instance/remote.h"
#include "instance/scheduler.h"
#include "instance/signal.h"
#include "instance/surface_joints.h"
#include "instance/tween.h"
#include "instance/value_types.h"
#include "instance/vector3.h"

#include "lua.h"
#include "lualib.h"

#include <curl/curl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace rbx {

namespace {

// Strict mode turns a compatibility fallback that would otherwise invent a
// value into an explicit error. Compatibility mode still keeps scripts alive,
// but every such call is recorded in the API audit.
bool strict_api() {
    const char* v = std::getenv("RBX_STRICT_API");
    return v && *v && *v != '0';
}

void note_unsupported(lua_State* L, const char* cls, const char* method) {
    rbx::audit::note_fallback_call(cls, method);
    if (strict_api())
        luaL_error(L, "%s:%s is not implemented in this client; set "
                       "RBX_STRICT_API=0 for compatibility behavior",
                   cls, method);
}

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

void collect_base_parts(Instance* inst, std::vector<Instance*>* out);
bool part_cframe_size(Instance* part, rbx::CFrame* cf, rbx::Vector3* size);

void model_world_bounds(Instance* model, bool* any, double mn[3],
                        double mx[3]) {
    std::vector<Instance*> parts;
    collect_base_parts(model, &parts);
    *any = false;
    for (Instance* part : parts) {
        rbx::CFrame cf;
        rbx::Vector3 sz;
        if (!part_cframe_size(part, &cf, &sz))
            continue;
        const double r[3][3] = {
            {cf.r00, cf.r01, cf.r02},
            {cf.r10, cf.r11, cf.r12},
            {cf.r20, cf.r21, cf.r22}};
        double h[3];
        for (int a = 0; a < 3; ++a)
            h[a] = std::fabs(r[0][a]) * sz.x * 0.5 +
                   std::fabs(r[1][a]) * sz.y * 0.5 +
                   std::fabs(r[2][a]) * sz.z * 0.5;
        const double p[3] = {cf.x, cf.y, cf.z};
        for (int a = 0; a < 3; ++a) {
            if (!*any || p[a] - h[a] < mn[a]) mn[a] = p[a] - h[a];
            if (!*any || p[a] + h[a] > mx[a]) mx[a] = p[a] + h[a];
        }
        *any = true;
    }
}

// --- CFrame / model transform helpers -------------------------------------
// The value type keeps its operators private to cframe.cpp; these are the
// three operations the instance methods need (composition, inverse, and a
// point transform), kept local so the behavior stays in this file.
rbx::CFrame cf_mul(const rbx::CFrame& a, const rbx::CFrame& b) {
    rbx::CFrame o;
    o.r00 = a.r00 * b.r00 + a.r01 * b.r10 + a.r02 * b.r20;
    o.r01 = a.r00 * b.r01 + a.r01 * b.r11 + a.r02 * b.r21;
    o.r02 = a.r00 * b.r02 + a.r01 * b.r12 + a.r02 * b.r22;
    o.r10 = a.r10 * b.r00 + a.r11 * b.r10 + a.r12 * b.r20;
    o.r11 = a.r10 * b.r01 + a.r11 * b.r11 + a.r12 * b.r21;
    o.r12 = a.r10 * b.r02 + a.r11 * b.r12 + a.r12 * b.r22;
    o.r20 = a.r20 * b.r00 + a.r21 * b.r10 + a.r22 * b.r20;
    o.r21 = a.r20 * b.r01 + a.r21 * b.r11 + a.r22 * b.r21;
    o.r22 = a.r20 * b.r02 + a.r21 * b.r12 + a.r22 * b.r22;
    o.x = a.r00 * b.x + a.r01 * b.y + a.r02 * b.z + a.x;
    o.y = a.r10 * b.x + a.r11 * b.y + a.r12 * b.z + a.y;
    o.z = a.r20 * b.x + a.r21 * b.y + a.r22 * b.z + a.z;
    return o;
}

rbx::CFrame cf_inv(const rbx::CFrame& c) {
    rbx::CFrame o;
    o.r00 = c.r00; o.r01 = c.r10; o.r02 = c.r20;
    o.r10 = c.r01; o.r11 = c.r11; o.r12 = c.r21;
    o.r20 = c.r02; o.r21 = c.r12; o.r22 = c.r22;
    o.x = -(o.r00 * c.x + o.r01 * c.y + o.r02 * c.z);
    o.y = -(o.r10 * c.x + o.r11 * c.y + o.r12 * c.z);
    o.z = -(o.r20 * c.x + o.r21 * c.y + o.r22 * c.z);
    return o;
}

rbx::Vector3 cf_point(const rbx::CFrame& c, const rbx::Vector3& p) {
    return rbx::Vector3{
        c.r00 * p.x + c.r01 * p.y + c.r02 * p.z + c.x,
        c.r10 * p.x + c.r11 * p.y + c.r12 * p.z + c.y,
        c.r20 * p.x + c.r21 * p.y + c.r22 * p.z + c.z};
}

bool model_pivot_cf(Instance* model, rbx::CFrame* out) {
    rbx::Variant pv, cfv;
    if (model->get_prop("PrimaryPart", pv))
        if (const auto* p = std::get_if<Instance*>(&pv); p && *p)
            if ((*p)->get_prop("CFrame", cfv))
                if (const auto* cf = std::get_if<rbx::CFrame>(&cfv)) {
                    *out = *cf;
                    return true;
                }
    if (model->get_prop("WorldPivot", cfv))
        if (const auto* cf = std::get_if<rbx::CFrame>(&cfv)) {
            *out = *cf;
            return true;
        }
    *out = rbx::CFrame{};
    return false;
}

void apply_model_delta(Instance* model, const rbx::CFrame& delta) {
    std::vector<Instance*> parts;
    collect_base_parts(model, &parts);
    for (Instance* part : parts) {
        rbx::CFrame cf;
        rbx::Vector3 sz;
        if (!part_cframe_size(part, &cf, &sz))
            continue;
        const rbx::CFrame moved = cf_mul(delta, cf);
        std::string err;
        part->set_prop("CFrame", rbx::Variant(moved), &err);
    }
    // Keep the fallback pivot in step when the model has no PrimaryPart.
    rbx::Variant wp;
    if (model->get_prop("WorldPivot", wp))
        if (const auto* cf = std::get_if<rbx::CFrame>(&wp)) {
            std::string err;
            model->set_prop("WorldPivot", rbx::Variant(cf_mul(delta, *cf)),
                            &err);
        }
}

int m_model_get_extents_size(lua_State* L) {
    Instance* model = Instance::check(L, 1);
    double mn[3] = {0, 0, 0}, mx[3] = {0, 0, 0};
    bool any = false;
    model_world_bounds(model, &any, mn, mx);
    rbx::push_vector3(L, any ? rbx::Vector3{mx[0] - mn[0], mx[1] - mn[1],
                                           mx[2] - mn[2]}
                            : rbx::Vector3{});
    return 1;
}

int m_model_get_bounding_box(lua_State* L) {
    Instance* model = Instance::check(L, 1);
    double mn[3] = {0, 0, 0}, mx[3] = {0, 0, 0};
    bool any = false;
    model_world_bounds(model, &any, mn, mx);
    rbx::CFrame cf;
    cf.x = (mn[0] + mx[0]) * 0.5;
    cf.y = (mn[1] + mx[1]) * 0.5;
    cf.z = (mn[2] + mx[2]) * 0.5;
    rbx::push_cframe(L, cf);
    rbx::push_vector3(L, any ? rbx::Vector3{mx[0] - mn[0], mx[1] - mn[1],
                                           mx[2] - mn[2]}
                            : rbx::Vector3{});
    return 2;
}

int m_model_get_pivot(lua_State* L) {
    rbx::CFrame cf;
    model_pivot_cf(Instance::check(L, 1), &cf);
    rbx::push_cframe(L, cf);
    return 1;
}

int m_model_get_primary_part(lua_State* L) {
    Instance* model = Instance::check(L, 1);
    rbx::Variant pv;
    if (model->get_prop("PrimaryPart", pv))
        if (const auto* p = std::get_if<Instance*>(&pv); p && *p) {
            Instance::push(L, *p);
            return 1;
        }
    lua_pushnil(L);
    return 1;
}

int m_model_get_primary_cframe(lua_State* L) {
    Instance* model = Instance::check(L, 1);
    rbx::Variant pv, cfv;
    if (model->get_prop("PrimaryPart", pv))
        if (const auto* p = std::get_if<Instance*>(&pv); p && *p)
            if ((*p)->get_prop("CFrame", cfv))
                if (const auto* cf = std::get_if<rbx::CFrame>(&cfv)) {
                    rbx::push_cframe(L, *cf);
                    return 1;
                }
    luaL_error(L, "Model has no PrimaryPart");
    return 0;
}

int m_model_pivot_to(lua_State* L) {
    Instance* model = Instance::check(L, 1);
    const rbx::CFrame target = check_cframe(L, 2);
    rbx::CFrame current;
    if (!model_pivot_cf(model, &current))
        current = rbx::CFrame{};
    apply_model_delta(model, cf_mul(target, cf_inv(current)));
    return 0;
}

int m_model_set_primary_cframe(lua_State* L) {
    return m_model_pivot_to(L);
}

int m_model_translate_by(lua_State* L) {
    Instance* model = Instance::check(L, 1);
    const rbx::Vector3 v = check_vector3(L, 2);
    rbx::CFrame delta;
    delta.x = v.x;
    delta.y = v.y;
    delta.z = v.z;
    apply_model_delta(model, delta);
    return 0;
}

int m_model_get_scale(lua_State* L) {
    Instance* model = Instance::check(L, 1);
    // Model.Scale is NotScriptable in the API dump, so the runtime keeps it
    // as a hidden attribute; GetScale/ScaleTo are the script-visible surface.
    rbx::Variant v;
    if (model->get_attr("__ModelScale", v))
        if (const auto* d = std::get_if<double>(&v)) {
            lua_pushnumber(L, *d);
            return 1;
        }
    lua_pushnumber(L, 1.0);
    return 1;
}

int m_model_scale_to(lua_State* L) {
    Instance* model = Instance::check(L, 1);
    const double factor = luaL_checknumber(L, 2);
    if (!(factor > 0.0)) {
        luaL_error(L, "Scale must be greater than zero");
        return 0;
    }
    rbx::CFrame pivot;
    model_pivot_cf(model, &pivot);
    rbx::Variant sv;
    double old = 1.0;
    if (model->get_attr("__ModelScale", sv))
        if (const auto* d = std::get_if<double>(&sv)) old = *d > 0 ? *d : 1.0;
    const double ratio = factor / old;
    std::vector<Instance*> parts;
    collect_base_parts(model, &parts);
    for (Instance* part : parts) {
        rbx::CFrame cf;
        rbx::Vector3 sz;
        if (!part_cframe_size(part, &cf, &sz))
            continue;
        cf.x = pivot.x + (cf.x - pivot.x) * ratio;
        cf.y = pivot.y + (cf.y - pivot.y) * ratio;
        cf.z = pivot.z + (cf.z - pivot.z) * ratio;
        rbx::Vector3 ns{sz.x * ratio, sz.y * ratio, sz.z * ratio};
        std::string err;
        part->set_prop("CFrame", rbx::Variant(cf), &err);
        part->set_prop("Size", rbx::Variant(ns), &err);
    }
    rbx::Variant wp;
    if (model->get_prop("WorldPivot", wp))
        if (const auto* cf = std::get_if<rbx::CFrame>(&wp)) {
            std::string err;
            model->set_prop("WorldPivot", rbx::Variant(*cf), &err);
        }
    model->set_attr("__ModelScale", rbx::Variant(factor));
    return 0;
}

int m_model_move_to(lua_State* L) {
    Instance* model = Instance::check(L, 1);
    const rbx::Vector3 pos = check_vector3(L, 2);
    rbx::CFrame current;
    model_pivot_cf(model, &current);
    rbx::CFrame target = current;
    target.x = pos.x;
    target.y = pos.y;
    target.z = pos.z;
    apply_model_delta(model, cf_mul(target, cf_inv(current)));
    return 0;
}

int m_model_get_model_cframe(lua_State* L) {
    return m_model_get_pivot(L);
}

int m_model_get_model_size(lua_State* L) {
    return m_model_get_extents_size(L);
}

int m_model_get_persistent_players(lua_State* L) {
    Instance::check(L, 1);
    lua_newtable(L);
    return 1;
}

// --- BasePart query surface ------------------------------------------------
// These methods report the replicated transform/velocity data.  Touch
// queries still use axis-aligned overlap until the contact system lands;
// joint queries below walk the active assembly graph instead.
bool part_world_aabb(Instance* part, double mn[3], double mx[3]) {
    rbx::CFrame cf;
    rbx::Vector3 sz;
    if (!part_cframe_size(part, &cf, &sz))
        return false;
    mn[0] = mn[1] = mn[2] = std::numeric_limits<double>::infinity();
    mx[0] = mx[1] = mx[2] = -std::numeric_limits<double>::infinity();
    const double h[3] = {sz.x * 0.5, sz.y * 0.5, sz.z * 0.5};
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
            for (int szn = -1; szn <= 1; szn += 2) {
                const rbx::Vector3 p = cf_point(
                    cf, rbx::Vector3{sx * h[0], sy * h[1], szn * h[2]});
                const double q[3] = {p.x, p.y, p.z};
                for (int a = 0; a < 3; ++a) {
                    if (q[a] < mn[a]) mn[a] = q[a];
                    if (q[a] > mx[a]) mx[a] = q[a];
                }
            }
    return true;
}

bool aabb_overlap(const double amn[3], const double amx[3],
                  const double bmn[3], const double bmx[3]) {
    for (int a = 0; a < 3; ++a)
        if (amx[a] < bmn[a] - 1e-6 || bmx[a] < amn[a] - 1e-6)
            return false;
    return true;
}

Instance* top_ancestor(Instance* inst) {
    while (inst->parent()) inst = inst->parent();
    return inst;
}

void collect_overlapping_parts(Instance* part, std::vector<Instance*>* out) {
    double amn[3], amx[3];
    if (!part_world_aabb(part, amn, amx))
        return;
    std::vector<Instance*> parts;
    collect_base_parts(top_ancestor(part), &parts);
    for (Instance* other : parts) {
        if (other == part)
            continue;
        double bmn[3], bmx[3];
        if (!part_world_aabb(other, bmn, bmx))
            continue;
        if (aabb_overlap(amn, amx, bmn, bmx))
            out->push_back(other);
    }
}

void push_part_list(lua_State* L, const std::vector<Instance*>& parts) {
    lua_createtable(L, int(parts.size()), 0);
    for (size_t i = 0; i < parts.size(); ++i) {
        Instance::push(L, parts[i]);
        lua_rawseti(L, -2, int(i + 1));
    }
}

bool get_bool_prop(Instance* inst, const char* name, bool def) {
    rbx::Variant v;
    if (!inst->get_prop(name, v)) return def;
    if (const auto* b = std::get_if<bool>(&v)) return *b;
    return def;
}

int m_part_get_mass(lua_State* L) {
    Instance* part = Instance::check(L, 1);
    rbx::Variant mv;
    if (part->get_prop("Mass", mv))
        if (const auto* d = std::get_if<double>(&mv); d && *d > 0) {
            lua_pushnumber(L, *d);
            return 1;
        }
    rbx::CFrame cf;
    rbx::Vector3 sz;
    double mass = 0.0;
    if (part_cframe_size(part, &cf, &sz)) {
        double gravity = 0.7;
        rbx::Variant gv;
        if (part->get_prop("SpecificGravity", gv))
            if (const auto* d = std::get_if<double>(&gv); d && *d > 0)
                gravity = *d;
        // Native BasePart:GetMass uses the primitive volume, not the
        // bounding box: Ball (min extent sphere), Cylinder (X-axis), Wedge
        // (half box) and CornerWedge (the five-vertex pyramid) are measured
        // by the 1818 mass probe.  The default Block is the full box.
        double volume = sz.x * sz.y * sz.z;
        int shape = 1;
        rbx::Variant shape_value;
        if (part->get_prop("Shape", shape_value))
            if (const auto* item = std::get_if<rbx::EnumItem>(&shape_value))
                shape = item->value;
        switch (shape) {
        case 0: { // Ball
            const double radius =
                std::min({sz.x, sz.y, sz.z}) * 0.5;
            volume = 4.0 / 3.0 * 3.14159265358979323846 * radius * radius *
                     radius;
            break;
        }
        case 2: { // Cylinder is X-axis
            const double radius = std::min(sz.y, sz.z) * 0.5;
            volume = 3.14159265358979323846 * radius * radius * sz.x;
            break;
        }
        case 3:
            volume *= 0.5;
            break;
        case 4:
            volume /= 3.0;
            break;
        default:
            break;
        }
        // Native BasePart:GetMass is a float; round-trip through float so
        // script-visible values (and live parity probes) match bit-for-bit.
        mass = double(static_cast<float>(volume * gravity));
    }
    lua_pushnumber(L, mass);
    return 1;
}

int m_part_get_touching_parts(lua_State* L) {
    Instance* part = Instance::check(L, 1);
    std::vector<Instance*> hits;
    collect_overlapping_parts(part, &hits);
    push_part_list(L, hits);
    return 1;
}

int m_part_get_connected_parts(lua_State* L) {
    Instance* part = Instance::check(L, 1);
    const bool recursive =
        !lua_isnoneornil(L, 2) && lua_toboolean(L, 2) != 0;
    std::vector<Instance*> hits;
    assembly::connected_parts(part, recursive, &hits);
    push_part_list(L, hits);
    return 1;
}

int m_part_get_joints(lua_State* L) {
    Instance* part = Instance::check(L, 1);
    std::vector<Instance*> out;
    assembly::joints(part, &out);
    push_part_list(L, out);
    return 1;
}

int m_part_get_root_part(lua_State* L) {
    Instance* part = Instance::check(L, 1);
    Instance* root = assembly::root_part(part);
    if (root)
        Instance::push(L, root);
    else
        lua_pushnil(L);
    return 1;
}

int m_part_make_joints(lua_State* L) {
    surface_joints::make_joints(Instance::check(L, 1));
    return 0;
}

int m_part_break_joints(lua_State* L) {
    surface_joints::break_joints(Instance::check(L, 1));
    return 0;
}

int m_model_make_joints(lua_State* L) {
    // Native Model:MakeJoints walks its subtree and runs each BasePart's
    // surface pass; it is not a no-op.  The earlier Hinge-only probe missed
    // this because its parts were interpenetrating, which native rejects.
    surface_joints::make_joints_tree(Instance::check(L, 1));
    return 0;
}

int m_model_break_joints(lua_State* L) {
    surface_joints::break_joints_tree(Instance::check(L, 1));
    return 0;
}

// Workspace:MakeJoints/BreakJoints take a table of BaseParts (native
// reflection says "Expected table argument, got ...").  Each listed part
// runs the same surface/rigid pass as its BasePart method; the rest of the
// Workspace is available as contact geometry.
int m_workspace_joints(lua_State* L, bool make) {
    // Method call layout is (self, partsTable); native reports the missing
    // table, not the self userdata, so the argument lives at index 2.
    if (lua_type(L, 2) == LUA_TNONE)
        luaL_error(L, "Argument 1 missing or nil");
    if (lua_type(L, 2) != LUA_TTABLE)
        luaL_error(L, "Expected table argument, got %s",
                   lua_typename(L, lua_type(L, 2)));
    const int n = int(lua_objlen(L, 2));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        Instance* inst = Instance::check(L, -1);
        if (inst->is_a("BasePart")) {
            if (make)
                surface_joints::make_joints(inst);
            else
                surface_joints::break_joints(inst);
        }
        lua_pop(L, 1);
    }
    return 0;
}

int m_workspace_make_joints(lua_State* L) {
    return m_workspace_joints(L, true);
}

int m_workspace_break_joints(lua_State* L) {
    return m_workspace_joints(L, false);
}

int m_part_get_network_owner(lua_State* L) {
    Instance::check(L, 1);
    // Native client error, captured on place 189707 (2026-11-13): ownership
    // is server state, and the client-side API refuses to expose it.  Boblox
    // runs replicated scripts as a client, so match that exactly instead of
    // returning a fabricated owner.
    luaL_error(L, "Network Ownership API can only be called from the Server.");
    return 0;
}

int m_part_get_network_ownership_auto(lua_State* L) {
    Instance::check(L, 1);
    // The native client disables the whole network-ownership surface, not only
    // GetNetworkOwner (the same server-only error is raised by both getters).
    luaL_error(L, "Network Ownership API can only be called from the Server.");
    return 0;
}

int m_part_is_grounded(lua_State* L) {
    Instance::check(L, 1);
    lua_pushboolean(L, 0);
    return 1;
}

int m_part_can_collide_with(lua_State* L) {
    Instance* part = Instance::check(L, 1);
    Instance* other = Instance::check(L, 2);
    const bool can = part != other && get_bool_prop(part, "CanCollide", true) &&
                     get_bool_prop(other, "CanCollide", true);
    lua_pushboolean(L, can ? 1 : 0);
    return 1;
}

int m_part_get_closest_point(lua_State* L) {
    Instance* part = Instance::check(L, 1);
    const rbx::Vector3 p = check_vector3(L, 2);
    rbx::CFrame cf;
    rbx::Vector3 sz;
    if (!part_cframe_size(part, &cf, &sz)) {
        rbx::push_vector3(L, p);
        return 1;
    }
    const rbx::CFrame inv = cf_inv(cf);
    const rbx::Vector3 local = cf_point(inv, p);
    const rbx::Vector3 clamped{
        std::max(-sz.x * 0.5, std::min(sz.x * 0.5, local.x)),
        std::max(-sz.y * 0.5, std::min(sz.y * 0.5, local.y)),
        std::max(-sz.z * 0.5, std::min(sz.z * 0.5, local.z))};
    rbx::push_vector3(L, cf_point(cf, clamped));
    return 1;
}

int m_part_get_render_cframe(lua_State* L) {
    Instance* part = Instance::check(L, 1);
    rbx::Variant v;
    if (part->get_prop("CFrame", v))
        if (const auto* cf = std::get_if<rbx::CFrame>(&v)) {
            rbx::push_cframe(L, *cf);
            return 1;
        }
    rbx::push_cframe(L, rbx::CFrame{});
    return 1;
}

int m_part_get_velocity_at_position(lua_State* L) {
    Instance* part = Instance::check(L, 1);
    (void)lua_isnoneornil(L, 2);
    rbx::Variant v;
    if (part->get_prop("AssemblyLinearVelocity", v) ||
        part->get_prop("Velocity", v))
        if (const auto* vel = std::get_if<rbx::Vector3>(&v)) {
            rbx::push_vector3(L, *vel);
            return 1;
        }
    rbx::push_vector3(L, rbx::Vector3{});
    return 1;
}

int m_part_no_constraints(lua_State* L) {
    Instance::check(L, 1);
    lua_newtable(L);
    return 1;
}

// --- Camera projection -----------------------------------------------------
// The native camera uses the render viewport, but a headless session still
// has a valid camera CFrame/FOV. Fall back to 1280x720 when a replicated
// camera has not been sized yet, so GUI/camera scripts get finite results.
bool camera_params(Instance* camera, rbx::CFrame* cf, double* w, double* h,
                   double* fov) {
    rbx::Variant cv, vv, fv;
    if (!camera->get_prop("CFrame", cv))
        return false;
    const auto* c = std::get_if<rbx::CFrame>(&cv);
    if (!c)
        return false;
    *cf = *c;
    *w = 1280.0;
    *h = 720.0;
    if (camera->get_prop("ViewportSize", vv))
        if (const auto* v = std::get_if<rbx::Vector2>(&vv); v && v->x > 0 && v->y > 0) {
            *w = v->x;
            *h = v->y;
        }
    *fov = 70.0;
    if (camera->get_prop("FieldOfView", fv))
        if (const auto* d = std::get_if<double>(&fv); d && *d > 0 && *d < 179)
            *fov = *d;
    return true;
}

bool camera_project(Instance* camera, const rbx::Vector3& world, double* sx,
                    double* sy, double* depth, bool* on_screen) {
    rbx::CFrame cf;
    double w, h, fov;
    if (!camera_params(camera, &cf, &w, &h, &fov))
        return false;
    const double dx = world.x - cf.x;
    const double dy = world.y - cf.y;
    const double dz = world.z - cf.z;
    const double vx = cf.r00 * dx + cf.r10 * dy + cf.r20 * dz;
    const double vy = cf.r01 * dx + cf.r11 * dy + cf.r21 * dz;
    const double vz = cf.r02 * dx + cf.r12 * dy + cf.r22 * dz;
    const double z = -vz; // camera looks down local -Z
    const double tan_half = std::tan(fov * 3.14159265358979323846 / 360.0);
    const double aspect = w / h;
    if (std::fabs(z) < 1e-9) {
        *sx = w * 0.5;
        *sy = h * 0.5;
        *depth = 0.0;
        *on_screen = false;
        return true;
    }
    *sx = w * 0.5 + (vx / z) / (tan_half * aspect) * w * 0.5;
    *sy = h * 0.5 - (vy / z) / tan_half * h * 0.5;
    *depth = z;
    *on_screen = z > 0 && *sx >= 0 && *sx <= w && *sy >= 0 && *sy <= h;
    return true;
}

int m_camera_world_to_viewport(lua_State* L) {
    Instance* camera = Instance::check(L, 1);
    const rbx::Vector3 p = check_vector3(L, 2);
    double sx = 0, sy = 0, depth = 0;
    bool on = false;
    camera_project(camera, p, &sx, &sy, &depth, &on);
    rbx::push_vector3(L, rbx::Vector3{sx, sy, depth});
    return 1;
}

int m_camera_world_to_screen(lua_State* L) {
    Instance* camera = Instance::check(L, 1);
    const rbx::Vector3 p = check_vector3(L, 2);
    double sx = 0, sy = 0, depth = 0;
    bool on = false;
    camera_project(camera, p, &sx, &sy, &depth, &on);
    rbx::push_vector3(L, rbx::Vector3{sx, sy, depth});
    lua_pushboolean(L, on ? 1 : 0);
    return 2;
}

rbx::Ray camera_ray(Instance* camera, double x, double y) {
    rbx::CFrame cf;
    double w, h, fov;
    if (!camera_params(camera, &cf, &w, &h, &fov)) {
        rbx::Ray r;
        r.origin = rbx::Vector3{cf.x, cf.y, cf.z};
        r.direction = rbx::Vector3{0, 0, -1};
        return r;
    }
    const double tan_half = std::tan(fov * 3.14159265358979323846 / 360.0);
    const double aspect = w / h;
    const double lx = (x / w * 2.0 - 1.0) * tan_half * aspect;
    const double ly = (1.0 - y / h * 2.0) * tan_half;
    const double lz = -1.0;
    const double len = std::sqrt(lx * lx + ly * ly + lz * lz);
    const double nx = lx / len, ny = ly / len, nz = lz / len;
    rbx::Ray r;
    r.origin = rbx::Vector3{cf.x, cf.y, cf.z};
    r.direction = rbx::Vector3{
        cf.r00 * nx + cf.r01 * ny + cf.r02 * nz,
        cf.r10 * nx + cf.r11 * ny + cf.r12 * nz,
        cf.r20 * nx + cf.r21 * ny + cf.r22 * nz};
    return r;
}

int m_camera_screen_point_to_ray(lua_State* L) {
    Instance* camera = Instance::check(L, 1);
    const double x = luaL_checknumber(L, 2);
    const double y = luaL_checknumber(L, 3);
    rbx::push_ray(L, camera_ray(camera, x, y));
    return 1;
}

int m_camera_viewport_point_to_ray(lua_State* L) {
    return m_camera_screen_point_to_ray(L);
}

int m_camera_get_render_cframe(lua_State* L) {
    Instance* camera = Instance::check(L, 1);
    rbx::Variant v;
    if (camera->get_prop("CFrame", v))
        if (const auto* cf = std::get_if<rbx::CFrame>(&v)) {
            rbx::push_cframe(L, *cf);
            return 1;
        }
    rbx::push_cframe(L, rbx::CFrame{});
    return 1;
}

int m_camera_get_parts_obscuring_target(lua_State* L) {
    Instance::check(L, 1);
    lua_newtable(L);
    return 1;
}

bool query_token_match(Instance* inst, const std::string& token) {
    if (token.empty() || token == "*")
        return true;
    if (token[0] == '#')
        return inst->name() == token.substr(1);
    if (token[0] == '.')
        return inst->is_a(token.substr(1));
    if (token.front() == '[' && token.back() == ']') {
        const std::string body = token.substr(1, token.size() - 2);
        const size_t eq = body.find('=');
        const std::string key = body.substr(0, eq);
        std::string want = eq == std::string::npos ? "" : body.substr(eq + 1);
        if (!want.empty() && (want.front() == '"' || want.front() == '\''))
            want = want.substr(1, want.size() - 2);
        if (key == "Name") {
            Variant v;
            if (!inst->get_prop("Name", v) || !std::get_if<std::string>(&v))
                return false;
            return std::get<std::string>(v) == want;
        }
        if (key == "ClassName")
            return inst->class_name() == want;
        Variant v;
        if (!inst->get_attr(key, v))
            return false;
        if (const auto* s = std::get_if<std::string>(&v))
            return *s == want;
        if (const auto* b = std::get_if<bool>(&v))
            return (*b ? "true" : "false") == want;
        if (const auto* i = std::get_if<int64_t>(&v))
            return std::to_string(*i) == want;
        if (const auto* d = std::get_if<double>(&v))
            return std::to_string(*d) == want;
        return false;
    }
    return inst->is_a(token);
}

int m_query_descendants(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    const char* selector = luaL_checkstring(L, 2);
    std::vector<std::string> selectors;
    std::string cur;
    for (const char* p = selector; ; ++p) {
        if (*p == ',' || *p == '\0') {
            if (!cur.empty())
                selectors.push_back(cur);
            cur.clear();
            if (*p == '\0')
                break;
        } else {
            cur.push_back(*p);
        }
    }
    std::vector<Instance*> all;
    for (const auto& c : inst->children()) {
        all.push_back(c.get());
        collect_descendants(c.get(), all);
    }
    lua_newtable(L);
    int n = 0;
    for (Instance* d : all) {
        bool ok = false;
        for (const std::string& sel : selectors) {
            std::vector<std::string> tokens;
            std::string t;
            for (char ch : sel) {
                if (ch == ' ') {
                    if (!t.empty()) { tokens.push_back(t); t.clear(); }
                } else {
                    t.push_back(ch);
                }
            }
            if (!t.empty()) tokens.push_back(t);
            bool match = true;
            for (const auto& tok : tokens)
                if (!query_token_match(d, tok)) { match = false; break; }
            if (match) { ok = true; break; }
        }
        if (ok) {
            Instance::push(L, d);
            lua_rawseti(L, -2, ++n);
        }
    }
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

// Player:GetMouse() returns one stable Mouse object per Player. Its state is
// the host feed published by the presentation layer; headless runs therefore
// have no position until a real pointer moves. Never seed a center coordinate:
// scripts branch on X/Y, and an invented position is a false observation.
void player_mouse_table(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "rbx.PlayerMouse");
    if (lua_istable(L, -1))
        return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, "rbx.PlayerMouse");
}

Instance* push_player_mouse(lua_State* L, Instance* player) {
    player_mouse_table(L); // [table]
    lua_pushlightuserdata(L, player);
    lua_rawget(L, -2);
    if (Instance::get(L, -1)) {
        Instance* mouse = Instance::get(L, -1);
        lua_remove(L, -2); // [mouse]
        return mouse;
    }
    lua_pop(L, 1);
    const rbx::ClassInfo* mc = find_class("Mouse");
    if (!mc) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return nullptr;
    }
    rbx::Ref<Instance> mouse = rbx::instantiate(mc);
    const rbx::input::MouseState ms = rbx::input::mouse();
    mouse->set_internal_prop("ViewSizeX", rbx::Variant(int64_t(ms.view_width)));
    mouse->set_internal_prop("ViewSizeY", rbx::Variant(int64_t(ms.view_height)));
    mouse->set_internal_prop("X", rbx::Variant(int64_t(ms.x)));
    mouse->set_internal_prop("Y", rbx::Variant(int64_t(ms.y)));
    Instance::push(L, mouse.get()); // [table, mouse]
    lua_pushlightuserdata(L, player);
    lua_pushvalue(L, -2);
    lua_rawset(L, -4); // table[player] = mouse
    lua_remove(L, -2); // [mouse]
    return mouse.get();
}

int m_player_get_mouse(lua_State* L) {
    Instance* player = Instance::check(L, 1);
    if (!push_player_mouse(L, player)) {
        lua_pushnil(L);
        return 1;
    }
    return 1;
}

// Group and friend data are web/account facts. This build has no authenticated
// GroupService/Players web client, so compatibility mode returns the values
// the API documents for "no relationship", but records the call and strict
// mode raises instead of silently telling a script a false fact.
int m_player_is_in_group(lua_State* L) {
    Instance::check(L, 1);
    (void)luaL_checknumber(L, 2);
    note_unsupported(L, "Player", "IsInGroup");
    lua_pushboolean(L, 0);
    return 1;
}

int m_player_group_rank(lua_State* L) {
    Instance::check(L, 1);
    (void)luaL_checknumber(L, 2);
    note_unsupported(L, "Player", "GetRankInGroup");
    lua_pushinteger(L, 0);
    return 1;
}

int m_player_group_role(lua_State* L) {
    Instance::check(L, 1);
    (void)luaL_checknumber(L, 2);
    note_unsupported(L, "Player", "GetRoleInGroup");
    lua_pushstring(L, "Guest");
    return 1;
}

int m_player_empty_table(lua_State* L) {
    Instance::check(L, 1);
    note_unsupported(L, "Player", "GetFriendsOnline");
    lua_newtable(L);
    return 1;
}

void set_mouse_props(Instance* mouse, const rbx::input::MouseState& ms) {
    mouse->set_internal_prop("ViewSizeX", rbx::Variant(int64_t(ms.view_width)));
    mouse->set_internal_prop("ViewSizeY", rbx::Variant(int64_t(ms.view_height)));
    mouse->set_internal_prop("X", rbx::Variant(int64_t(ms.x)));
    mouse->set_internal_prop("Y", rbx::Variant(int64_t(ms.y)));
}

void fire_mouse_signal(lua_State* L, Instance* mouse, const char* name) {
    if (mouse && find_event(mouse->class_info(), name))
        mouse->event_signal(name)->fire(L);
}

// Build the transient InputObject the native passes to UserInputService. It
// must be a real Instance: scripts read UserInputType, UserInputState,
// KeyCode, Position and Delta from the argument.
void fire_input_signal(lua_State* L, const char* event, int input_type,
                       int state, int keycode, double x, double y) {
    if (!rbxch::api::push_service(L, "UserInputService"))
        return;
    Instance* uis = Instance::get(L, -1);
    if (!uis || !find_event(uis->class_info(), event)) {
        lua_pop(L, 1);
        return;
    }
    Ref<Instance> obj =
        input_objects::make(L, input_type, state, keycode, x, y);
    if (!obj) {
        lua_pop(L, 1);
        return;
    }
    Variant arg(static_cast<Instance*>(obj.get()));
    uis->event_signal(event)->fire(L, {arg});
    lua_pop(L, 1);
}

Instance* local_player_for_input(lua_State* L) {
    if (!rbxch::api::push_service(L, "Players"))
        return nullptr;
    Instance* players = Instance::get(L, -1);
    Instance* player = nullptr;
    if (players) {
        Variant v;
        if (players->get_prop("LocalPlayer", v))
            if (const auto* p = std::get_if<Instance*>(&v)) player = *p;
    }
    lua_pop(L, 1);
    return player;
}

void dispatch_host_input(lua_State* L) {
    const std::vector<rbx::input::Event> events = rbx::input::take_events();
    if (events.empty())
        return;
    const rbx::input::MouseState ms = rbx::input::mouse();
    Instance* player = local_player_for_input(L);
    Instance* mouse = nullptr;
    if (player) {
        mouse = push_player_mouse(L, player);
        if (mouse) set_mouse_props(mouse, ms);
        lua_pop(L, 1);
    }
    for (const rbx::input::Event& e : events) {
        // The ScreenGui tree gets first refusal in the same event pass.  UIS
        // still receives the transition for game-wide observers, while the
        // 3D camera/movement paths consult input::pointer_captured().
        rbx::gui::dispatch(L, e);
        switch (e.type) {
        case rbx::input::EventType::MouseMove:
            if (mouse) set_mouse_props(mouse, ms);
            fire_mouse_signal(L, mouse, "Move");
            fire_input_signal(L, "InputChanged", 4, 1, 0, e.x, e.y);
            break;
        case rbx::input::EventType::MouseButtonDown:
            if (mouse) set_mouse_props(mouse, ms);
            fire_mouse_signal(L, mouse,
                              e.button == 1 ? "Button1Down"
                              : e.button == 2 ? "Button2Down"
                                              : "Button3Down");
            fire_input_signal(L, "InputBegan", e.button - 1, 0, 0, e.x, e.y);
            break;
        case rbx::input::EventType::MouseButtonUp:
            if (mouse) set_mouse_props(mouse, ms);
            fire_mouse_signal(L, mouse,
                              e.button == 1 ? "Button1Up"
                              : e.button == 2 ? "Button2Up"
                                              : "Button3Up");
            fire_input_signal(L, "InputEnded", e.button - 1, 2, 0, e.x, e.y);
            break;
        case rbx::input::EventType::KeyDown:
            fire_input_signal(L, "InputBegan", 8, 0, e.keycode, 0, 0);
            break;
        case rbx::input::EventType::KeyUp:
            fire_input_signal(L, "InputEnded", 8, 2, e.keycode, 0, 0);
            break;
        case rbx::input::EventType::MouseWheel:
        case rbx::input::EventType::TextInput:
        case rbx::input::EventType::TouchTap:
            break; // consumed by the GUI dispatcher; UIS has no equivalent
        }
    }
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

void datastore_registry(lua_State* L, const char* field);

// ---- animation surface (client-local tracks) ------------------------------
// There is no animation evaluator/renderer here, but scripts still construct
// tracks, Play/Stop them and query the playing set. Keep that state real.
void animation_tracks_table(lua_State* L, Instance* owner) {
    datastore_registry(L, "rbx.AnimationTracks");
    lua_pushlightuserdata(L, owner);
    lua_rawget(L, -2);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushlightuserdata(L, owner);
        lua_pushvalue(L, -2);
        lua_rawset(L, -4);
    }
    lua_remove(L, -2);
}

Instance* new_animation_track(lua_State* L, Instance* animation) {
    const rbx::ClassInfo* cls = find_class("AnimationTrack");
    if (!cls)
        return nullptr;
    rbx::Ref<Instance> track = rbx::instantiate(cls);
    if (animation)
        track->set_internal_prop("Animation", Variant(animation));
    Instance::push(L, track.get());
    return track.get(); // userdata now owns the ref
}

int m_animator_load_animation(lua_State* L) {
    Instance* owner = Instance::check(L, 1);
    Instance* animation = lua_isnoneornil(L, 2) ? nullptr : Instance::check(L, 2);
    if (!new_animation_track(L, animation))
        lua_pushnil(L);
    // [track]
    animation_tracks_table(L, owner); // [track, tracks]
    const int n = int(lua_objlen(L, -1)) + 1;
    lua_pushvalue(L, -2);
    lua_rawseti(L, -2, n);
    lua_pop(L, 1); // [track]
    return 1;
}

int m_animation_track_play(lua_State* L) {
    Instance* track = Instance::check(L, 1);
    track->set_internal_prop("IsPlaying", Variant(true));
    return 0;
}

int m_animation_track_stop(lua_State* L) {
    Instance* track = Instance::check(L, 1);
    track->set_internal_prop("IsPlaying", Variant(false));
    return 0;
}

int m_animation_track_adjust_speed(lua_State* L) {
    Instance* track = Instance::check(L, 1);
    track->set_internal_prop("Speed", Variant(luaL_checknumber(L, 2)));
    return 0;
}

int m_animation_track_adjust_weight(lua_State* L) {
    Instance* track = Instance::check(L, 1);
    const double weight = luaL_checknumber(L, 2);
    track->set_internal_prop("WeightTarget", Variant(weight));
    track->set_internal_prop("WeightCurrent", Variant(weight));
    return 0;
}

int m_animator_get_playing_tracks(lua_State* L) {
    Instance* owner = Instance::check(L, 1);
    animation_tracks_table(L, owner); // [tracks]
    lua_newtable(L);                  // [tracks, out]
    int n = 0;
    const int count = int(lua_objlen(L, -2));
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, -2, i);
        Instance* track = Instance::get(L, -1);
        bool playing = false;
        if (track) {
            Variant v;
            if (track->get_prop("IsPlaying", v))
                if (const auto* b = std::get_if<bool>(&v))
                    playing = *b;
        }
        if (playing) {
            lua_pushvalue(L, -1);
            lua_rawseti(L, -3, ++n);
        }
        lua_pop(L, 1);
    }
    lua_remove(L, -2); // [out]
    return 1;
}

int m_animator_get_track_by_animation_id(lua_State* L) {
    Instance* owner = Instance::check(L, 1);
    const char* want = luaL_checkstring(L, 2);
    animation_tracks_table(L, owner); // [tracks]
    const int count = int(lua_objlen(L, -1));
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, -1, i);
        Instance* track = Instance::get(L, -1);
        if (track) {
            Variant av;
            if (track->get_prop("Animation", av))
                if (const auto* a = std::get_if<Instance*>(&av); a && *a) {
                    Variant id;
                    if ((*a)->get_prop("AnimationId", id))
                        if (const auto* s = std::get_if<std::string>(&id);
                            s && *s == want) {
                            lua_remove(L, -2); // tracks
                            return 1;          // [track]
                        }
                }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_pushnil(L);
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

// JSON wrappers are defined after the parser below; DataStore uses them for
// optional local-file persistence.
bool json_dump_lua(lua_State* L, int idx, std::string& out, std::string& err);
bool json_load_lua(lua_State* L, const std::string& text, std::string& err);

// ---- in-memory DataStore (client-local only) ------------------------------
// Real DataStore persistence is server-side. The loader/dependency scripts
// still call GetAsync/SetAsync/UpdateAsync synchronously; keep a per-store Lua
// table so a session can run its save-loading code instead of aborting on nil.
void datastore_registry(lua_State* L, const char* field) {
    lua_getfield(L, LUA_REGISTRYINDEX, field);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, field);
    }
}

void datastore_cache(lua_State* L, const char* field) {
    datastore_registry(L, field);
}

void datastore_data(lua_State* L, Instance* store) {
    datastore_registry(L, "rbx.DataStoreData");
    lua_pushlightuserdata(L, store);
    lua_rawget(L, -2);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushlightuserdata(L, store);
        lua_pushvalue(L, -2);
        lua_rawset(L, -4);
    }
    lua_remove(L, -2); // [data]
}

std::string datastore_path(Instance* store, const char* key) {
    const char* dir = std::getenv("RBX_DATASTORE_DIR");
    if (!dir || !*dir)
        return {};
    auto safe = [](std::string s) {
        for (char& c : s)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
                c = '_';
        return s;
    };
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string name =
        safe(store->class_name() + "_" + store->name() + "_" + key) + ".json";
    return (std::filesystem::path(dir) / name).string();
}

void datastore_save(Instance* store, const char* key, lua_State* L,
                    int value_idx) {
    const std::string path = datastore_path(store, key);
    if (path.empty())
        return;
    std::string out, err;
    if (!json_dump_lua(L, value_idx, out, err))
        return;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f)
        f.write(out.data(), std::streamsize(out.size()));
}

bool datastore_file_load(lua_State* L, Instance* store, const char* key) {
    const std::string path = datastore_path(store, key);
    if (path.empty())
        return false;
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::string text((std::istreambuf_iterator<char>(f)), {});
    std::string err;
    if (!json_load_lua(L, text, err))
        return false;
    return true;
}

int m_datastore_get_async(lua_State* L) {
    Instance* store = Instance::check(L, 1);
    const char* key = luaL_checkstring(L, 2);
    datastore_data(L, store); // [data]
    lua_getfield(L, -1, key);
    if (!lua_isnil(L, -1)) {
        lua_remove(L, -2);
        return 1;
    }
    lua_pop(L, 1); // [data]
    if (datastore_file_load(L, store, key)) { // [data, value]
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, key);
        lua_remove(L, -2);
        return 1;
    }
    lua_pop(L, 1); // [data]
    lua_pushnil(L);
    lua_remove(L, -2);
    return 1;
}

int m_datastore_set_async(lua_State* L) {
    Instance* store = Instance::check(L, 1);
    const char* key = luaL_checkstring(L, 2);
    datastore_data(L, store); // [data]
    lua_pushvalue(L, 3);
    lua_setfield(L, -2, key);
    lua_pop(L, 1);
    datastore_save(store, key, L, 3);
    return 0;
}

int m_datastore_remove_async(lua_State* L) {
    Instance* store = Instance::check(L, 1);
    const char* key = luaL_checkstring(L, 2);
    datastore_data(L, store); // [data]
    lua_getfield(L, -1, key);
    lua_pushnil(L);
    lua_setfield(L, -3, key);
    const std::string path = datastore_path(store, key);
    if (!path.empty()) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    lua_remove(L, -2);
    return 1; // old value, like the native
}

int m_datastore_update_async(lua_State* L) {
    Instance* store = Instance::check(L, 1);
    const char* key = luaL_checkstring(L, 2);
    if (!lua_isfunction(L, 3)) {
        luaL_error(L, "UpdateAsync expects a function");
        return 0;
    }
    datastore_data(L, store); // [data]
    lua_getfield(L, -1, key); // [data, old]
    lua_pushvalue(L, 3);     // [data, old, fn]
    lua_pushvalue(L, -2);    // [data, old, fn, old]
    if (lua_pcall(L, 1, 1, 0) != 0) {
        lua_error(L);
        return 0;
    }
    // [data, old, new]
    if (lua_isnil(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -4, key);
        lua_pushnil(L);
    } else {
        lua_pushvalue(L, -1);
        lua_setfield(L, -4, key);
        datastore_save(store, key, L, -1);
    }
    lua_remove(L, -3); // old
    lua_remove(L, -2); // data
    return 1;          // new
}

int pages_get_current(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    return 1;
}

// Input stack: [array]. Output: Pages object with GetCurrentPage().
void make_pages(lua_State* L) {
    lua_newtable(L);
    lua_pushvalue(L, -2);
    lua_pushcclosure(L, pages_get_current, "GetCurrentPage", 1);
    lua_setfield(L, -2, "GetCurrentPage");
    lua_remove(L, -2);
}

int m_datastore_list_keys(lua_State* L) {
    Instance* store = Instance::check(L, 1);
    const char* prefix = luaL_optstring(L, 2, "");
    datastore_data(L, store); // [data]
    lua_newtable(L);          // [data, keys]
    int n = 0;
    lua_pushnil(L);
    while (lua_next(L, -3)) {
        const char* k = lua_tostring(L, -2);
        if (k && std::strncmp(k, prefix, std::strlen(prefix)) == 0) {
            lua_pushvalue(L, -2);
            lua_rawseti(L, -4, ++n);
        }
        lua_pop(L, 1);
    }
    lua_remove(L, -2); // drop data, leave [keys]
    make_pages(L);     // [pages]
    return 1;
}

int m_datastore_get_version(lua_State* L) {
    Instance::check(L, 1);
    (void)luaL_checkstring(L, 2);
    lua_pushnil(L);
    return 1;
}

int m_datastore_list_versions(lua_State* L) {
    Instance::check(L, 1);
    (void)luaL_checkstring(L, 2);
    lua_newtable(L);
    make_pages(L);
    return 1;
}

int m_datastore_remove_version(lua_State* L) {
    Instance::check(L, 1);
    return 0;
}

int m_ordered_get_sorted(lua_State* L) {
    Instance* store = Instance::check(L, 1);
    const bool ascending = lua_toboolean(L, 2) != 0;
    datastore_data(L, store); // [data]
    struct Entry { double value; std::string key; };
    std::vector<Entry> entries;
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        const char* k = lua_tostring(L, -2);
        if (k) {
            const double v = lua_tonumber(L, -1);
            entries.push_back({v, k});
        }
        lua_pop(L, 1);
    }
    std::sort(entries.begin(), entries.end(),
              [&](const Entry& a, const Entry& b) {
                  return ascending ? a.value < b.value : a.value > b.value;
              });
    lua_newtable(L); // [data, arr]
    int n = 0;
    for (const Entry& e : entries) {
        lua_newtable(L);
        lua_pushstring(L, e.key.c_str());
        lua_setfield(L, -2, "key");
        lua_getfield(L, -3, e.key.c_str());
        lua_setfield(L, -2, "value");
        lua_rawseti(L, -2, ++n);
    }
    lua_remove(L, -2); // drop data, leave [arr]
    make_pages(L);     // [pages]
    return 1;
}

int m_remotefunction_invoke_client(lua_State* L) {
    Instance* rf = Instance::check(L, 1);
    if (lua_gettop(L) < 2)
        luaL_error(L, "InvokeClient expects a Player and arguments");
    push_instance_callback(L, rf, "OnClientInvoke");
    if (!lua_isfunction(L, -1))
        luaL_error(L, "RemoteFunction.OnClientInvoke is not set");
    lua_insert(L, 1);  // [fn, self, player, args...]
    lua_remove(L, 2);  // drop self
    lua_remove(L, 2);  // drop player; OnClientInvoke gets user args
    const int nargs = lua_gettop(L) - 1;
    if (lua_pcall(L, nargs, LUA_MULTRET, 0) != 0)
        lua_error(L);
    return lua_gettop(L);
}

int m_datastore_increment_async(lua_State* L) {
    Instance* store = Instance::check(L, 1);
    const char* key = luaL_checkstring(L, 2);
    const double delta = luaL_optnumber(L, 3, 1.0);
    datastore_data(L, store);
    lua_getfield(L, -1, key);
    const double old = lua_tonumber(L, -1);
    lua_pop(L, 1);
    const double now = old + delta;
    lua_pushnumber(L, now);
    lua_setfield(L, -2, key);
    lua_pushnumber(L, now);
    datastore_save(store, key, L, -1);
    lua_pop(L, 2);
    lua_pushnumber(L, now);
    return 1;
}

int m_datastore_service_get_store(lua_State* L, const char* cls) {
    Instance* service = Instance::check(L, 1);
    const char* name = luaL_checkstring(L, 2);
    const char* scope = luaL_optstring(L, 3, "global");
    datastore_cache(L, "rbx.DataStoreCache"); // [cache]
    lua_pushfstring(L, "%s\1%s", name, scope);
    lua_rawget(L, -2); // [cache, store?]
    if (!lua_isnil(L, -1)) {
        lua_remove(L, -2);
        return 1;
    }
    lua_pop(L, 1); // [cache]
    const rbx::ClassInfo* c = find_class(cls);
    if (!c) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return 1;
    }
    rbx::Ref<Instance> store = rbx::instantiate(c);
    store->set_prop("Name", Variant(std::string(name)), nullptr);
    Instance::push(L, store.get()); // [cache, store]
    lua_pushfstring(L, "%s\1%s", name, scope);
    lua_pushvalue(L, -2);
    lua_rawset(L, -4); // cache[key]=store
    lua_remove(L, -2); // store
    (void)service;
    return 1;
}

int m_datastore_service_get_data_store(lua_State* L) {
    return m_datastore_service_get_store(L, "DataStore");
}

int m_datastore_service_get_global_store(lua_State* L) {
    Instance::check(L, 1);
    lua_pushstring(L, "global"); // implicit store name for the no-arg form
    return m_datastore_service_get_store(L, "DataStore");
}

int m_datastore_service_get_ordered_store(lua_State* L) {
    return m_datastore_service_get_store(L, "OrderedDataStore");
}

int m_datastore_service_budget(lua_State* L) {
    Instance::check(L, 1);
    lua_pushinteger(L, 1000);
    return 1;
}

// ---- HttpService network requests -----------------------------------------
struct HttpResult {
    bool ok = false;
    long status = 0;
    std::string body;
    std::string headers;
    std::string error;
};

size_t http_write_body(char* p, size_t sz, size_t n, void* ud) {
    auto* out = static_cast<std::string*>(ud);
    out->append(p, sz * n);
    return sz * n;
}

size_t http_write_header(char* p, size_t sz, size_t n, void* ud) {
    auto* out = static_cast<std::string*>(ud);
    out->append(p, sz * n);
    return sz * n;
}

bool http_do(const std::string& method, const std::string& url,
             const std::string& body, const std::string& headers,
             HttpResult* out) {
    CURL* c = curl_easy_init();
    if (!c) {
        out->error = "curl_easy_init failed";
        return false;
    }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Roblox/WinInet");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, http_write_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out->body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, http_write_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &out->headers);
    struct curl_slist* hs = nullptr;
    if (!headers.empty()) {
        std::string line;
        for (size_t i = 0; i <= headers.size(); ++i) {
            if (i == headers.size() || headers[i] == '\n') {
                if (!line.empty()) hs = curl_slist_append(hs, line.c_str());
                line.clear();
            } else if (headers[i] != '\r') {
                line.push_back(headers[i]);
            }
        }
        if (hs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hs);
    }
    if (method == "POST" || method == "PUT" || method == "PATCH") {
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    } else if (method != "GET") {
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
    }
    const CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &out->status);
        out->ok = true;
    } else {
        out->error = curl_easy_strerror(rc);
    }
    if (hs) curl_slist_free_all(hs);
    curl_easy_cleanup(c);
    return out->ok;
}

int m_httpservice_get_async(lua_State* L) {
    Instance::check(L, 1);
    const char* url = luaL_checkstring(L, 2);
    HttpResult r;
    http_do("GET", url, "", "", &r);
    if (!r.ok)
        luaL_error(L, "GetAsync failed: %s", r.error.c_str());
    lua_pushlstring(L, r.body.data(), r.body.size());
    return 1;
}

int m_httpservice_post_async(lua_State* L) {
    Instance::check(L, 1);
    const char* url = luaL_checkstring(L, 2);
    const char* body = luaL_optstring(L, 3, "");
    HttpResult r;
    http_do("POST", url, body, "", &r);
    if (!r.ok)
        luaL_error(L, "PostAsync failed: %s", r.error.c_str());
    lua_pushlstring(L, r.body.data(), r.body.size());
    return 1;
}

int m_httpservice_request_async(lua_State* L) {
    Instance::check(L, 1);
    if (!lua_istable(L, 2))
        luaL_error(L, "RequestAsync expects an options table");
    lua_getfield(L, 2, "Url");
    const char* url = luaL_checkstring(L, -1);
    lua_getfield(L, 2, "Method");
    const char* method = lua_isnil(L, -1) ? "GET" : luaL_checkstring(L, -1);
    lua_getfield(L, 2, "Body");
    const char* body = lua_isnil(L, -1) ? "" : luaL_checkstring(L, -1);
    std::string headers;
    lua_getfield(L, 2, "Headers");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            const char* k = lua_tostring(L, -2);
            const char* v = lua_tostring(L, -1);
            if (k && v) {
                headers += k;
                headers += ": ";
                headers += v;
                headers += "\n";
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 4);
    HttpResult r;
    http_do(method, url, body, headers, &r);
    lua_newtable(L);
    lua_pushboolean(L, r.ok ? 1 : 0);
    lua_setfield(L, -2, "Success");
    lua_pushinteger(L, r.status);
    lua_setfield(L, -2, "StatusCode");
    lua_pushstring(L, r.ok ? "OK" : r.error.c_str());
    lua_setfield(L, -2, "StatusMessage");
    lua_pushlstring(L, r.body.data(), r.body.size());
    lua_setfield(L, -2, "Body");
    lua_newtable(L);
    lua_setfield(L, -2, "Headers");
    return 1;
}

// ---- minimal JSON (HttpService:JSONEncode/Decode) -------------------------
std::string json_escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\b': o += "\\b"; break;
        case '\f': o += "\\f"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                o += b;
            } else {
                o.push_back(char(c));
            }
        }
    }
    return o;
}

bool json_encode_value(lua_State* L, int idx, std::string& out,
                       std::string& err, int depth) {
    if (depth > 64) {
        err = "JSONEncode: nesting too deep";
        return false;
    }
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        out += "null";
        return true;
    case LUA_TBOOLEAN:
        out += lua_toboolean(L, idx) ? "true" : "false";
        return true;
    case LUA_TNUMBER: {
        char b[64];
        std::snprintf(b, sizeof b, "%.17g", lua_tonumber(L, idx));
        out += b;
        return true;
    }
    case LUA_TSTRING: {
        size_t n = 0;
        const char* s = lua_tolstring(L, idx, &n);
        out += '"';
        out += json_escape(std::string(s ? s : "", n));
        out += '"';
        return true;
    }
    case LUA_TTABLE: {
        const int n = int(lua_objlen(L, idx));
        bool array = true;
        lua_pushnil(L);
        int seen = 0;
        while (lua_next(L, idx)) {
            ++seen;
            if (lua_type(L, -2) != LUA_TNUMBER) {
                array = false;
            } else {
                const int k = int(lua_tointeger(L, -2));
                if (k < 1 || k > n)
                    array = false;
            }
            lua_pop(L, 1);
            if (!array) {
                lua_pop(L, 1); // drop the current key before a fresh iteration
                break;
            }
        }
        if (array && seen == n) {
            out += '[';
            for (int i = 1; i <= n; ++i) {
                if (i > 1) out += ',';
                lua_rawgeti(L, idx, i);
                if (!json_encode_value(L, -1, out, err, depth + 1)) {
                    lua_pop(L, 1);
                    return false;
                }
                lua_pop(L, 1);
            }
            out += ']';
            return true;
        }
        out += '{';
        bool first = true;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            if (!first) out += ',';
            first = false;
            if (lua_type(L, -2) == LUA_TSTRING || lua_type(L, -2) == LUA_TNUMBER) {
                size_t kn = 0;
                const char* ks = lua_tolstring(L, -2, &kn);
                out += '"';
                out += json_escape(std::string(ks ? ks : "", kn));
                out += '"';
            } else {
                lua_pop(L, 2);
                err = "JSONEncode: table key must be string or number";
                return false;
            }
            out += ':';
            if (!json_encode_value(L, -1, out, err, depth + 1)) {
                lua_pop(L, 2);
                return false;
            }
            lua_pop(L, 1);
        }
        out += '}';
        return true;
    }
    default:
        err = "JSONEncode: unsupported value type";
        return false;
    }
}

struct JsonParser {
    const char* p;
    const char* e;
    std::string err;

    void ws() {
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            ++p;
    }
    bool lit(const char* s) {
        const char* save = p;
        const char* q = s;
        while (*q) {
            if (p >= e || *p++ != *q++) {
                p = save;
                return false;
            }
        }
        return true;
    }
    static void utf8(std::string& o, uint32_t cp) {
        if (cp <= 0x7F) o.push_back(char(cp));
        else if (cp <= 0x7FF) {
            o.push_back(char(0xC0 | (cp >> 6)));
            o.push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            o.push_back(char(0xE0 | (cp >> 12)));
            o.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            o.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            o.push_back(char(0xF0 | (cp >> 18)));
            o.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            o.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            o.push_back(char(0x80 | (cp & 0x3F)));
        }
    }
    bool string(std::string& out) {
        if (p >= e || *p++ != '"')
            return false;
        while (p < e) {
            unsigned char c = (unsigned char)*p++;
            if (c == '"')
                return true;
            if (c != '\\') {
                out.push_back(char(c));
                continue;
            }
            if (p >= e) return false;
            const char esc = *p++;
            switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (e - p < 4) return false;
                uint32_t cp = 0;
                for (int i = 0; i < 4; ++i) {
                    char h = *p++;
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= uint32_t(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= uint32_t(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= uint32_t(h - 'A' + 10);
                    else return false;
                }
                utf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool value(lua_State* L, int depth) {
        if (depth > 64) { err = "JSONDecode: nesting too deep"; return false; }
        ws();
        if (p >= e) { err = "JSONDecode: unexpected end"; return false; }
        if (*p == '"') {
            std::string s;
            if (!string(s)) { err = "JSONDecode: bad string"; return false; }
            lua_pushlstring(L, s.data(), s.size());
            return true;
        }
        if (*p == '{') {
            ++p;
            lua_newtable(L);
            ws();
            if (p < e && *p == '}') { ++p; return true; }
            while (true) {
                ws();
                std::string k;
                if (!string(k)) { err = "JSONDecode: bad object key"; return false; }
                ws();
                if (p >= e || *p++ != ':') { err = "JSONDecode: expected ':'"; return false; }
                if (!value(L, depth + 1)) return false;
                lua_setfield(L, -2, k.c_str());
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; return true; }
                err = "JSONDecode: expected ',' or '}'";
                return false;
            }
        }
        if (*p == '[') {
            ++p;
            lua_newtable(L);
            int n = 0;
            ws();
            if (p < e && *p == ']') { ++p; return true; }
            while (true) {
                if (!value(L, depth + 1)) return false;
                lua_rawseti(L, -2, ++n);
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; return true; }
                err = "JSONDecode: expected ',' or ']'";
                return false;
            }
        }
        if (lit("true")) { lua_pushboolean(L, 1); return true; }
        if (lit("false")) { lua_pushboolean(L, 0); return true; }
        if (lit("null")) { lua_pushnil(L); return true; }
        char* end = nullptr;
        const char* start = p;
        const double d = std::strtod(p, &end);
        if (end == start) { err = "JSONDecode: bad value"; return false; }
        p = end;
        lua_pushnumber(L, d);
        return true;
    }
};

bool json_dump_lua(lua_State* L, int idx, std::string& out, std::string& err) {
    return json_encode_value(L, idx, out, err, 0);
}

bool json_load_lua(lua_State* L, const std::string& text, std::string& err) {
    JsonParser jp{text.data(), text.data() + text.size(), ""};
    if (!jp.value(L, 0)) {
        err = jp.err;
        return false;
    }
    jp.ws();
    if (jp.p != jp.e) {
        err = "JSONDecode: trailing data";
        return false;
    }
    return true;
}

int m_httpservice_json_encode(lua_State* L) {
    Instance::check(L, 1);
    std::string out, err;
    if (!json_encode_value(L, 2, out, err, 0))
        luaL_error(L, "%s", err.c_str());
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

int m_httpservice_json_decode(lua_State* L) {
    Instance::check(L, 1);
    size_t n = 0;
    const char* s = luaL_checklstring(L, 2, &n);
    JsonParser jp{s, s + n, ""};
    if (!jp.value(L, 0))
        luaL_error(L, "%s", jp.err.c_str());
    jp.ws();
    if (jp.p != jp.e)
        luaL_error(L, "JSONDecode: trailing data");
    return 1;
}

int m_gui_set_gameplay_notification(lua_State* L) {
    Instance::check(L, 1);
    return 0; // headless client has no gameplay-pause banner
}

int translator_translate(lua_State* L) {
    // Translator:Translate(key [, args]) — headless uses the source string.
    if (lua_isstring(L, 2))
        lua_pushvalue(L, 2);
    else
        lua_pushstring(L, "");
    return 1;
}

int m_localization_get_translator(lua_State* L) {
    Instance::check(L, 1);
    lua_newtable(L);
    lua_pushcfunction(L, translator_translate, "Translate");
    lua_setfield(L, -2, "Translate");
    return 1;
}

int m_content_provider_preload(lua_State* L) {
    Instance::check(L, 1);
    // No renderer/asset cache here: report immediate success through the
    // callback when one is supplied, matching a fully cached client.
    if (lua_isfunction(L, 2)) {
        for (int i = 3; i <= lua_gettop(L); ++i) {
            lua_pushvalue(L, 2);
            lua_pushvalue(L, i);
            lua_pushinteger(L, 1); // Enum.AssetFetchStatus.Success
            if (lua_pcall(L, 2, 0, 0) != 0)
                lua_pop(L, 1);
        }
    }
    return 0;
}

int m_httpservice_generate_guid(lua_State* L) {
    Instance::check(L, 1);
    const bool braces = lua_isnoneornil(L, 2) ? true : lua_toboolean(L, 2) != 0;
    unsigned char b[16];
    static uint64_t state = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < 16; ++i) {
        state ^= state << 7;
        state ^= state >> 9;
        b[i] = uint8_t(state >> (8 * (i % 8)));
    }
    b[6] = uint8_t((b[6] & 0x0F) | 0x40);
    b[8] = uint8_t((b[8] & 0x3F) | 0x80);
    char out[40];
    std::snprintf(out, sizeof out,
                  "%s%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                  "%02x%02x%02x%02x%02x%02x%s",
                  braces ? "{" : "", b[0], b[1], b[2], b[3], b[4], b[5],
                  b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14],
                  b[15], braces ? "}" : "");
    lua_pushstring(L, out);
    return 1;
}

int m_httpservice_url_encode(lua_State* L) {
    Instance::check(L, 1);
    size_t n = 0;
    const char* s = luaL_checklstring(L, 2, &n);
    static const char* h = "0123456789ABCDEF";
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char)s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out.push_back(char(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(h[c >> 4]);
            out.push_back(h[c & 15]);
        }
    }
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

int m_httpservice_user_agent(lua_State* L) {
    Instance::check(L, 1);
    lua_pushstring(L, "Roblox/WinInet");
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

int m_bindable_function_invoke(lua_State* L) {
    Instance* fnobj = Instance::check(L, 1);
    push_instance_callback(L, fnobj, "OnInvoke");
    if (!lua_isfunction(L, -1))
        luaL_error(L, "BindableFunction.OnInvoke is not set");
    lua_insert(L, 1); // [fn, self, args...]
    lua_remove(L, 2); // OnInvoke receives user args, not the BindableFunction
    const int nargs = lua_gettop(L) - 1;
    if (lua_pcall(L, nargs, LUA_MULTRET, 0) != 0)
        lua_error(L);
    return lua_gettop(L);
}

int m_bindable_event_fire(lua_State* L) {    Instance* self = Instance::check(L, 1);
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

int m_humanoid_move_to(lua_State* L) {
    Instance* hum = Instance::check(L, 1);
    const Variant target = check_variant(L, 2, PropType::Vector3);
    const Vector3& to = std::get<Vector3>(target);
    Variant rv;
    if (!hum->get_prop("RootPart", rv)) {
        luaL_error(L, "Humanoid has no RootPart");
        return 0;
    }
    auto* root = std::get_if<Instance*>(&rv);
    if (!root || !*root) {
        luaL_error(L, "Humanoid has no RootPart");
        return 0;
    }
    Variant cf;
    if (!(*root)->get_prop("CFrame", cf)) {
        luaL_error(L, "Humanoid RootPart has no CFrame");
        return 0;
    }
    auto* frame = std::get_if<CFrame>(&cf);
    if (!frame) {
        luaL_error(L, "Humanoid RootPart has invalid CFrame");
        return 0;
    }
    // Headless local controller: move the kinematic root toward the target
    // immediately. The server remains authoritative; this only makes the
    // local character state usable by scripts until ownership is available.
    frame->x = to.x;
    frame->y = to.y;
    frame->z = to.z;
    (*root)->set_local_cframe(*frame);
    return 0;
}

int m_humanoid_get_state(lua_State* L) {
    Instance* hum = Instance::check(L, 1);
    Variant v;
    if (hum->get_attr("__HumanoidState", v))
        if (const auto* item = std::get_if<EnumItem>(&v)) {
            push_variant(L, *item);
            return 1;
        }
    push_variant(L, Variant(EnumItem{"HumanoidStateType", 8})); // Running
    return 1;
}

int m_humanoid_change_state(lua_State* L) {
    Instance* hum = Instance::check(L, 1);
    const Variant v = check_variant(L, 2, PropType::Enum, "HumanoidStateType");
    hum->set_attr("__HumanoidState", v);
    if (find_event(hum->class_info(), "StateChanged"))
        hum->event_signal("StateChanged")->fire(L, {v});
    return 0;
}

int m_humanoid_get_state_enabled(lua_State* L) {
    Instance* hum = Instance::check(L, 1);
    const Variant v = check_variant(L, 2, PropType::Enum, "HumanoidStateType");
    const int state = std::get<EnumItem>(v).value;
    Variant enabled;
    if (hum->get_attr("__StateOff" + std::to_string(state), enabled))
        if (const auto* b = std::get_if<bool>(&enabled)) {
            lua_pushboolean(L, !*b);
            return 1;
        }
    lua_pushboolean(L, 1);
    return 1;
}

int m_humanoid_set_state_enabled(lua_State* L) {
    Instance* hum = Instance::check(L, 1);
    const Variant v = check_variant(L, 2, PropType::Enum, "HumanoidStateType");
    const int state = std::get<EnumItem>(v).value;
    const bool enabled = lua_toboolean(L, 3) != 0;
    hum->set_attr("__StateOff" + std::to_string(state), Variant(!enabled));
    return 0;
}

int m_humanoid_move(lua_State* L) {
    Instance* hum = Instance::check(L, 1);
    const Variant v = check_variant(L, 2, PropType::Vector3);
    hum->set_internal_prop("MoveDirection", v);
    return 0;
}

int m_humanoid_get_move_velocity(lua_State* L) {
    Instance* hum = Instance::check(L, 1);
    Variant mv;
    Vector3 dir{};
    if (hum->get_prop("MoveDirection", mv))
        if (const auto* d = std::get_if<Vector3>(&mv)) dir = *d;
    Variant speed;
    double ws = 0.0;
    if (hum->get_prop("WalkSpeed", speed))
        if (const auto* d = std::get_if<double>(&speed)) ws = *d;
    push_variant(L, Variant(Vector3{dir.x * ws, dir.y * ws, dir.z * ws}));
    return 1;
}

int m_humanoid_get_accessories(lua_State* L) {
    Instance* hum = Instance::check(L, 1);
    lua_newtable(L);
    int n = 0;
    for (const auto& c : hum->children())
        if (c->is_a("Accessory")) {
            Instance::push(L, c.get());
            lua_rawseti(L, -2, ++n);
        }
    return 1;
}

void set_sound_state(Instance* snd, bool playing, bool paused) {
    snd->set_internal_prop("IsPlaying", Variant(playing));
    snd->set_internal_prop("IsPaused", Variant(paused));
    snd->set_internal_prop("Playing", Variant(playing));
}

bool sound_bool(Instance* snd, const char* prop) {
    Variant v;
    if (snd->get_prop(prop, v))
        if (const auto* b = std::get_if<bool>(&v)) return *b;
    return false;
}

double sound_number(Instance* snd, const char* prop, double fallback) {
    Variant v;
    if (snd->get_prop(prop, v))
        if (const auto* n = std::get_if<double>(&v)) return *n;
    return fallback;
}

std::string sound_uri(Instance* snd) {
    Variant v;
    if (snd->get_prop("SoundId", v)) {
        if (const auto* c = std::get_if<Content>(&v)) return c->uri;
        if (const auto* s = std::get_if<std::string>(&v)) return *s;
    }
    if (snd->get_prop("AudioContent", v)) {
        if (const auto* c = std::get_if<Content>(&v)) return c->uri;
        if (const auto* s = std::get_if<std::string>(&v)) return *s;
    }
    return {};
}

int m_sound_play(lua_State* L) {
    Instance* snd = Instance::check(L, 1);
    rbx::audio::Backend* backend = rbx::audio::backend();
    if (!backend) {
        set_sound_state(snd, false, false);
        note_unsupported(L, "Sound", "Play");
        return 0;
    }
    const std::string uri = sound_uri(snd);
    if (uri.empty()) {
        set_sound_state(snd, false, false);
        std::fprintf(stderr, "sound: Play has no SoundId/AudioContent URI\n");
        return 0;
    }
    std::string why;
    const bool ok = backend->play(snd, uri, sound_bool(snd, "Looped"),
                                  sound_number(snd, "Volume", 0.5), &why);
    set_sound_state(snd, ok, false);
    if (!ok) {
        // Keep query strings (which may be signed tokens) out of diagnostics.
        const std::string safe = uri.substr(0, uri.find_first_of("?#"));
        std::fprintf(stderr, "sound: %s: %s\n", safe.c_str(), why.c_str());
    }
    return 0;
}

int m_sound_stop(lua_State* L) {
    Instance* snd = Instance::check(L, 1);
    if (rbx::audio::Backend* backend = rbx::audio::backend())
        backend->stop(snd);
    set_sound_state(snd, false, false);
    return 0;
}

int m_sound_pause(lua_State* L) {
    Instance* snd = Instance::check(L, 1);
    rbx::audio::Backend* backend = rbx::audio::backend();
    if (!backend) {
        set_sound_state(snd, false, false);
        note_unsupported(L, "Sound", "Pause");
        return 0;
    }
    // Pause only has an effect on an active voice. Without one, reporting
    // IsPlaying would be another fabricated state. Pausing an already-paused
    // sound is idempotent, not a reset.
    if (sound_bool(snd, "IsPaused"))
        return 0;
    if (!backend->is_playing(snd)) {
        set_sound_state(snd, false, false);
        return 0;
    }
    backend->pause(snd);
    // A paused voice is still an active sound; PlaybackState distinguishes it.
    set_sound_state(snd, true, true);
    return 0;
}

int m_sound_resume(lua_State* L) {
    Instance* snd = Instance::check(L, 1);
    rbx::audio::Backend* backend = rbx::audio::backend();
    if (!backend) {
        set_sound_state(snd, false, false);
        note_unsupported(L, "Sound", "Resume");
        return 0;
    }
    backend->resume(snd);
    set_sound_state(snd, backend->is_playing(snd), false);
    return 0;
}

int m_sound_get_playback_state(lua_State* L) {
    Instance* snd = Instance::check(L, 1);
    const bool isPaused = sound_bool(snd, "IsPaused");
    bool isPlaying = sound_bool(snd, "IsPlaying");
    if (rbx::audio::Backend* backend = rbx::audio::backend())
        isPlaying = backend->is_playing(snd);
    const int value = isPaused ? 3 : (isPlaying ? 2 : 4);
    push_variant(L, Variant(EnumItem{"PlaybackState", value}));
    return 1;
}

int m_sound_get_world_cframe(lua_State* L) {
    Instance* snd = Instance::check(L, 1);
    Variant v;
    if (snd->parent() && snd->parent()->get_prop("CFrame", v))
        if (const auto* cf = std::get_if<CFrame>(&v)) {
            push_cframe(L, *cf);
            return 1;
        }
    push_cframe(L, CFrame{});
    return 1;
}

int opt_enum_value(lua_State* L, int idx, const char* enum_name, int fallback) {
    if (lua_isnoneornil(L, idx))
        return fallback;
    const Variant v = check_variant(L, idx, PropType::Enum, enum_name);
    return std::get<EnumItem>(v).value;
}

bool opt_bool_value(lua_State* L, int idx, bool fallback) {
    if (lua_isnoneornil(L, idx))
        return fallback;
    luaL_checktype(L, idx, LUA_TBOOLEAN);
    return lua_toboolean(L, idx) != 0;
}

TweenInfo legacy_tween_info(lua_State* L, int direction_idx, int style_idx,
                            int time_idx) {
    TweenInfo info;
    info.easing_direction = EnumItem{
        "EasingDirection", opt_enum_value(L, direction_idx, "EasingDirection", 1)};
    info.easing_style =
        EnumItem{"EasingStyle", opt_enum_value(L, style_idx, "EasingStyle", 3)};
    info.time = lua_isnoneornil(L, time_idx)
                    ? 1.0
                    : luaL_checknumber(L, time_idx);
    if (info.time < 0.0)
        info.time = 0.0;
    return info;
}

int m_gui_tween_position(lua_State* L) {
    Instance* gui = Instance::check(L, 1);
    const Variant pos = check_variant(L, 2, PropType::UDim2);
    const TweenInfo info = legacy_tween_info(L, 3, 4, 5);
    const bool override = opt_bool_value(L, 6, false);
    std::string why;
    const bool ok = rbx::tween_gui_properties(
        L, gui, {{"Position", pos}}, info, override, &why);
    lua_pushboolean(L, ok);
    return 1;
}

int m_gui_tween_size(lua_State* L) {
    Instance* gui = Instance::check(L, 1);
    const Variant size = check_variant(L, 2, PropType::UDim2);
    const TweenInfo info = legacy_tween_info(L, 3, 4, 5);
    const bool override = opt_bool_value(L, 6, false);
    std::string why;
    const bool ok = rbx::tween_gui_properties(
        L, gui, {{"Size", size}}, info, override, &why);
    lua_pushboolean(L, ok);
    return 1;
}

int m_gui_tween_size_and_position(lua_State* L) {
    Instance* gui = Instance::check(L, 1);
    const Variant size = check_variant(L, 2, PropType::UDim2);
    const Variant pos = check_variant(L, 3, PropType::UDim2);
    const TweenInfo info = legacy_tween_info(L, 4, 5, 6);
    const bool override = opt_bool_value(L, 7, false);
    std::string why;
    const bool ok = rbx::tween_gui_properties(
        L, gui, {{"Size", size}, {"Position", pos}}, info, override, &why);
    lua_pushboolean(L, ok);
    return 1;
}

void push_input_object(lua_State* L, int input_type, int state, int keycode,
                       double x, double y) {
    const ClassInfo* ic = find_class("InputObject");
    if (!ic) {
        lua_newtable(L);
        return;
    }
    Ref<Instance> obj = instantiate(ic);
    obj->set_prop("UserInputType",
                  Variant(EnumItem{"UserInputType", input_type}), nullptr);
    obj->set_prop("UserInputState",
                  Variant(EnumItem{"UserInputState", state}), nullptr);
    if (keycode != 0)
        obj->set_prop("KeyCode", Variant(EnumItem{"KeyCode", keycode}),
                      nullptr);
    obj->set_prop("Position", Variant(Vector3{x, y, 0.0}), nullptr);
    obj->set_prop("Delta", Variant(Vector3{0.0, 0.0, 0.0}), nullptr);
    Instance::push(L, obj.get());
}

int m_uis_is_key_down(lua_State* L) {
    Instance::check(L, 1);
    const Variant v = check_variant(L, 2, PropType::Enum, "KeyCode");
    const int key = std::get<EnumItem>(v).value;
    lua_pushboolean(L, rbx::input::key_down(key));
    return 1;
}

int m_uis_is_mouse_button_pressed(lua_State* L) {
    Instance::check(L, 1);
    const Variant v = check_variant(L, 2, PropType::Enum, "UserInputType");
    const int button = std::get<EnumItem>(v).value;
    const rbx::input::MouseState ms = rbx::input::mouse();
    lua_pushboolean(L, button == 0 ? ms.button1
                      : button == 1 ? ms.button2
                      : button == 2 ? ms.button3
                                    : false);
    return 1;
}

int m_uis_keys_pressed(lua_State* L) {
    Instance::check(L, 1);
    lua_newtable(L);
    int n = 0;
    for (int key : rbx::input::keys_down()) {
        push_input_object(L, 8, 0, key, 0, 0);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int m_uis_mouse_buttons_pressed(lua_State* L) {
    Instance::check(L, 1);
    const rbx::input::MouseState ms = rbx::input::mouse();
    lua_newtable(L);
    int n = 0;
    const bool buttons[3] = {ms.button1, ms.button2, ms.button3};
    for (int i = 0; i < 3; ++i) {
        if (!buttons[i])
            continue;
        push_input_object(L, i, 0, 0, ms.x, ms.y);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int m_uis_never(lua_State* L) {
    Instance::check(L, 1);
    lua_pushboolean(L, 0);
    return 1;
}

int m_uis_empty_table(lua_State* L) {
    Instance::check(L, 1);
    lua_newtable(L);
    return 1;
}

int m_uis_nil(lua_State* L) {
    Instance::check(L, 1);
    lua_pushnil(L);
    return 1;
}

int m_uis_mouse_location(lua_State* L) {
    Instance::check(L, 1);
    const rbx::input::MouseState ms = rbx::input::mouse();
    push_variant(L, Variant(Vector2{ms.x, ms.y}));
    return 1;
}

int m_uis_last_input_type(lua_State* L) {
    Instance::check(L, 1);
    push_variant(L, Variant(EnumItem{"UserInputType",
                                    rbx::input::last_input_type()}));
    return 1;
}

int m_uis_device_type(lua_State* L) {
    Instance::check(L, 1);
    push_variant(L, Variant(EnumItem{"DeviceType", 1})); // Desktop
    return 1;
}

int m_uis_platform(lua_State* L) {
    Instance::check(L, 1);
    push_variant(L, Variant(EnumItem{"Platform", 13})); // Linux
    return 1;
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

// Instance:AddTag/RemoveTag/HasTag/GetTags. These are the object-side forms
// of CollectionService's methods; both share the same tag set and signals.
int m_instance_add_tag(lua_State* L) {
    Instance* target = Instance::check(L, 1);
    const char* tag = luaL_checkstring(L, 2);
    const bool fresh = !target->has_tag(tag);
    target->add_tag(tag);
    if (fresh)
        if (Signal* sig = tag_signal(L, true, tag, false))
            sig->fire(L, {Variant(target)});
    return 0;
}

int m_instance_remove_tag(lua_State* L) {
    Instance* target = Instance::check(L, 1);
    const char* tag = luaL_checkstring(L, 2);
    if (target->remove_tag(tag))
        if (Signal* sig = tag_signal(L, false, tag, false))
            sig->fire(L, {Variant(target)});
    return 0;
}

int m_instance_has_tag(lua_State* L) {
    lua_pushboolean(L, Instance::check(L, 1)->has_tag(luaL_checkstring(L, 2)));
    return 1;
}

int m_instance_get_tags(lua_State* L) {
    const std::vector<std::string> tags = Instance::check(L, 1)->tags();
    lua_createtable(L, int(tags.size()), 0);
    for (size_t i = 0; i < tags.size(); ++i) {
        lua_pushlstring(L, tags[i].data(), tags[i].size());
        lua_rawseti(L, -2, int(i + 1));
    }
    return 1;
}

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

namespace input {
void dispatch(lua_State* L) { dispatch_host_input(L); }
} // namespace input

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

// --- TeleportService (client-local, non-teleporting session) ----------------
// A headless/embedded client has no place-transition backend. The query
// surface still has to behave like a client that is not arriving from a
// teleport, and the action surface must return a determinate result instead of
// a generated type default (the old stub returned nil for a TeleportResult).
int m_teleport_get_arriving_gui(lua_State* L) {
    Instance::check(L, 1);
    lua_pushnil(L);
    return 1;
}

int m_teleport_get_local_data(lua_State* L) {
    Instance::check(L, 1);
    lua_pushnil(L);
    return 1;
}

int m_teleport_get_setting(lua_State* L) {
    Instance::check(L, 1);
    const char* key = luaL_checkstring(L, 2);
    lua_getfield(L, LUA_REGISTRYINDEX, "rbx.TeleportSettings");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return 1;
    }
    lua_getfield(L, -1, key);
    lua_remove(L, -2);
    return 1;
}

int m_teleport_set_setting(lua_State* L) {
    Instance::check(L, 1);
    const char* key = luaL_checkstring(L, 2);
    lua_getfield(L, LUA_REGISTRYINDEX, "rbx.TeleportSettings");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "rbx.TeleportSettings");
    }
    lua_pushvalue(L, 3);
    lua_setfield(L, -2, key);
    lua_pop(L, 1);
    return 0;
}

int m_teleport_action(lua_State* L) {
    Instance::check(L, 1);
    // There is no teleport backend, so "Failure" is the documented result of
    // this call (not a fabricated success). It is still surfaced to the audit
    // and becomes an explicit error in strict mode.
    note_unsupported(L, "TeleportService", "Teleport");
    rbx::push_enumitem(L, rbx::EnumItem{"TeleportResult", 1 /* Failure */});
    return 1;
}

int m_teleport_async(lua_State* L) {
    Instance::check(L, 1);
    // No teleport transition can start; nil is the documented no-result path.
    note_unsupported(L, "TeleportService", "TeleportAsync");
    lua_pushnil(L);
    return 1;
}

int m_teleport_noop(lua_State* L) {
    Instance::check(L, 1);
    return 0;
}

int m_teleport_empty(lua_State* L) {
    Instance::check(L, 1);
    lua_newtable(L);
    return 1;
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
    attach_method("Instance", "QueryDescendants", m_query_descendants);
    attach_method("Model", "GetExtentsSize", m_model_get_extents_size);
    attach_method("Model", "GetBoundingBox", m_model_get_bounding_box);
    attach_method("Model", "GetPivot", m_model_get_pivot);
    attach_method("Model", "PivotTo", m_model_pivot_to);
    attach_method("Model", "SetPrimaryPartCFrame", m_model_set_primary_cframe);
    attach_method("Model", "GetPrimaryPartCFrame", m_model_get_primary_cframe);
    attach_method("Model", "GetPrimaryPart", m_model_get_primary_part);
    attach_method("Model", "TranslateBy", m_model_translate_by);
    attach_method("Model", "GetScale", m_model_get_scale);
    attach_method("Model", "ScaleTo", m_model_scale_to);
    attach_method("Model", "MoveTo", m_model_move_to);
    attach_method("Model", "moveTo", m_model_move_to);
    attach_method("Model", "Move", m_model_move_to);
    attach_method("Model", "move", m_model_move_to);
    attach_method("Model", "GetModelCFrame", m_model_get_model_cframe);
    attach_method("Model", "GetModelSize", m_model_get_model_size);
    attach_method("Model", "GetPersistentPlayers", m_model_get_persistent_players);
    attach_method("BasePart", "GetMass", m_part_get_mass);
    attach_method("BasePart", "GetTouchingParts", m_part_get_touching_parts);
    attach_method("BasePart", "GetConnectedParts", m_part_get_connected_parts);
    attach_method("BasePart", "GetJoints", m_part_get_joints);
    attach_method("BasePart", "MakeJoints", m_part_make_joints);
    attach_method("BasePart", "BreakJoints", m_part_break_joints);
    attach_method("BasePart", "makeJoints", m_part_make_joints);
    attach_method("BasePart", "breakJoints", m_part_break_joints);
    // The generated dump repeats these methods on TriangleMeshPart, which
    // shadows BasePart for MeshPart descendants.
    attach_method("TriangleMeshPart", "MakeJoints", m_part_make_joints);
    attach_method("TriangleMeshPart", "BreakJoints", m_part_break_joints);
    attach_method("TriangleMeshPart", "makeJoints", m_part_make_joints);
    attach_method("TriangleMeshPart", "breakJoints", m_part_break_joints);
    attach_method("Model", "MakeJoints", m_model_make_joints);
    attach_method("Model", "BreakJoints", m_model_break_joints);
    attach_method("Model", "makeJoints", m_model_make_joints);
    attach_method("Model", "breakJoints", m_model_break_joints);
    attach_method("Workspace", "MakeJoints", m_workspace_make_joints);
    attach_method("Workspace", "BreakJoints", m_workspace_break_joints);
    attach_method("Workspace", "makeJoints", m_workspace_make_joints);
    attach_method("Workspace", "breakJoints", m_workspace_break_joints);
    attach_method("BasePart", "GetNoCollisionConstraints", m_part_no_constraints);
    attach_method("BasePart", "GetRootPart", m_part_get_root_part);
    attach_method("BasePart", "GetNetworkOwner", m_part_get_network_owner);
    attach_method("BasePart", "GetNetworkOwnershipAuto",
                  m_part_get_network_ownership_auto);
    attach_method("BasePart", "IsGrounded", m_part_is_grounded);
    attach_method("BasePart", "CanCollideWith", m_part_can_collide_with);
    attach_method("BasePart", "GetClosestPointOnSurface",
                  m_part_get_closest_point);
    attach_method("BasePart", "GetRenderCFrame", m_part_get_render_cframe);
    attach_method("BasePart", "GetVelocityAtPosition",
                  m_part_get_velocity_at_position);
    attach_method("Camera", "WorldToViewportPoint", m_camera_world_to_viewport);
    attach_method("Camera", "WorldToScreenPoint", m_camera_world_to_screen);
    attach_method("Camera", "ScreenPointToRay", m_camera_screen_point_to_ray);
    attach_method("Camera", "ViewportPointToRay", m_camera_viewport_point_to_ray);
    attach_method("Camera", "GetRenderCFrame", m_camera_get_render_cframe);
    attach_method("Camera", "GetPartsObscuringTarget",
                  m_camera_get_parts_obscuring_target);
    attach_method("Instance", "IsPropertyModified", m_is_property_modified);
    attach_method("Instance", "ResetPropertyToDefault",
                  m_reset_property_to_default);
    attach_method("Instance", "GetAttribute", m_get_attribute);
    attach_method("Instance", "SetAttribute", m_set_attribute);
    attach_method("Instance", "GetAttributes", m_get_attributes);
    attach_method("Instance", "GetAttributeChangedSignal", m_get_attribute_changed_signal);
    attach_method("Instance", "AddTag", m_instance_add_tag);
    attach_method("Instance", "RemoveTag", m_instance_remove_tag);
    attach_method("Instance", "HasTag", m_instance_has_tag);
    attach_method("Instance", "GetTags", m_instance_get_tags);
    attach_method("DataModel", "GetService", m_datamodel_get_service);
    attach_method("LocalScript", "AwaitPersistentModels",
                  m_localscript_await_persistent_models);
    attach_method("Players", "GetPlayers", m_players_get_players);
    attach_method("Players", "GetPlayerByUserId",
                  m_players_get_player_by_user_id);
    attach_method("Players", "GetPlayerFromCharacter",
                  m_players_get_player_from_character);
    attach_method("Player", "GetNetworkPing", m_player_get_network_ping);
    attach_method("Player", "GetMouse", m_player_get_mouse);
    attach_method("Player", "IsInGroup", m_player_is_in_group);
    attach_method("Player", "GetRankInGroup", m_player_group_rank);
    attach_method("Player", "GetRankInGroupAsync", m_player_group_rank);
    attach_method("Player", "GetRoleInGroup", m_player_group_role);
    attach_method("Player", "GetRoleInGroupAsync", m_player_group_role);
    attach_method("Player", "GetFriendsOnline", m_player_empty_table);
    attach_method("Player", "GetFriendsOnlineAsync", m_player_empty_table);
    attach_method("Player", "GetFriendsInServerAsync", m_player_empty_table);
    attach_method("Player", "GetFriendsInUniverseAsync", m_player_empty_table);
    attach_method("Player", "DistanceFromCharacter",
                  m_player_distance_from_character);
    attach_method("RunService", "BindToRenderStep",
                  m_runservice_bind_render_step);
    attach_method("RunService", "UnbindFromRenderStep",
                  m_runservice_unbind_render_step);
    attach_method("Animator", "LoadAnimation", m_animator_load_animation);
    attach_method("Humanoid", "LoadAnimation", m_animator_load_animation);
    attach_method("Humanoid", "loadAnimation", m_animator_load_animation);
    attach_method("Animator", "GetPlayingAnimationTracks",
                  m_animator_get_playing_tracks);
    attach_method("Humanoid", "GetPlayingAnimationTracks",
                  m_animator_get_playing_tracks);
    attach_method("Animator", "GetTrackByAnimationId",
                  m_animator_get_track_by_animation_id);
    attach_method("Humanoid", "GetTrackByAnimationId",
                  m_animator_get_track_by_animation_id);
    attach_method("AnimationTrack", "Play", m_animation_track_play);
    attach_method("AnimationTrack", "Stop", m_animation_track_stop);
    attach_method("AnimationTrack", "AdjustSpeed",
                  m_animation_track_adjust_speed);
    attach_method("AnimationTrack", "AdjustWeight",
                  m_animation_track_adjust_weight);
    attach_method("GuiService", "SetGameplayPausedNotificationEnabled",
                  m_gui_set_gameplay_notification);
    attach_method("LocalizationService", "GetTranslatorForPlayer",
                  m_localization_get_translator);
    attach_method("LocalizationService", "GetTranslatorForPlayerAsync",
                  m_localization_get_translator);
    attach_method("HttpService", "GetAsync", m_httpservice_get_async);
    attach_method("HttpService", "PostAsync", m_httpservice_post_async);
    attach_method("HttpService", "RequestAsync", m_httpservice_request_async);
    attach_method("HttpService", "JSONEncode", m_httpservice_json_encode);
    attach_method("HttpService", "JSONEncodeAsync", m_httpservice_json_encode);
    attach_method("HttpService", "JSONDecode", m_httpservice_json_decode);
    attach_method("HttpService", "JSONDecodeAsync", m_httpservice_json_decode);
    attach_method("ContentProvider", "Preload", m_content_provider_preload);
    attach_method("ContentProvider", "PreloadAsync", m_content_provider_preload);
    attach_method("HttpService", "GenerateGUID", m_httpservice_generate_guid);
    attach_method("HttpService", "UrlEncode", m_httpservice_url_encode);
    attach_method("HttpService", "GetUserAgent", m_httpservice_user_agent);
    attach_method("DataStoreService", "GetDataStore",
                  m_datastore_service_get_data_store);
    attach_method("DataStoreService", "GetGlobalDataStore",
                  m_datastore_service_get_global_store);
    attach_method("DataStoreService", "GetOrderedDataStore",
                  m_datastore_service_get_ordered_store);
    attach_method("DataStoreService", "GetRequestBudgetForRequestType",
                  m_datastore_service_budget);
    attach_method("DataStoreService", "SetRateLimitForRequestType",
                  m_workspace_get_num_awake_parts); // returns 0, ignores args
    attach_method("GlobalDataStore", "GetAsync", m_datastore_get_async);
    attach_method("GlobalDataStore", "SetAsync", m_datastore_set_async);
    attach_method("GlobalDataStore", "RemoveAsync", m_datastore_remove_async);
    attach_method("GlobalDataStore", "UpdateAsync", m_datastore_update_async);
    attach_method("GlobalDataStore", "IncrementAsync",
                  m_datastore_increment_async);
    attach_method("GlobalDataStore", "ListKeysAsync",
                  m_datastore_list_keys);
    attach_method("GlobalDataStore", "GetVersionAsync",
                  m_datastore_get_version);
    attach_method("GlobalDataStore", "GetVersionAtTimeAsync",
                  m_datastore_get_version);
    attach_method("GlobalDataStore", "ListVersionsAsync",
                  m_datastore_list_versions);
    attach_method("GlobalDataStore", "RemoveVersionAsync",
                  m_datastore_remove_version);
    attach_method("DataStore", "ListKeysAsync", m_datastore_list_keys);
    attach_method("DataStore", "GetVersionAsync", m_datastore_get_version);
    attach_method("DataStore", "GetVersionAtTimeAsync",
                  m_datastore_get_version);
    attach_method("DataStore", "ListVersionsAsync",
                  m_datastore_list_versions);
    attach_method("DataStore", "RemoveVersionAsync",
                  m_datastore_remove_version);
    attach_method("OrderedDataStore", "GetSortedAsync",
                  m_ordered_get_sorted);
    attach_method("RemoteFunction", "InvokeClient",
                  m_remotefunction_invoke_client);
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
    attach_method("RunService", "IsStudio", m_runservice_is_server);
    attach_method("RunService", "IsEdit", m_runservice_is_server);
    attach_method("RunService", "IsRunMode", m_runservice_is_client);
    attach_method("RunService", "IsRunning", m_runservice_is_client);
    attach_method("RunService", "IsTeamTest", m_runservice_is_server);
    attach_method("RunService", "IsTeamCreateViewer", m_runservice_is_server);
    attach_method("Workspace", "PGSIsEnabled", m_runservice_is_client);
    attach_method("Workspace", "ExperimentalSolverIsEnabled", m_runservice_is_server);
    attach_method("GuiObject", "TweenPosition", m_gui_tween_position);
    attach_method("GuiObject", "TweenPositionInternal", m_gui_tween_position);
    attach_method("GuiObject", "TweenSize", m_gui_tween_size);
    attach_method("GuiObject", "TweenSizeInternal", m_gui_tween_size);
    attach_method("GuiObject", "TweenSizeAndPosition",
                  m_gui_tween_size_and_position);
    attach_method("GuiObject", "TweenSizeAndPositionInternal",
                  m_gui_tween_size_and_position);
    attach_method("UserInputService", "IsKeyDown", m_uis_is_key_down);
    attach_method("UserInputService", "IsMouseButtonPressed",
                  m_uis_is_mouse_button_pressed);
    attach_method("UserInputService", "IsGamepadButtonDown", m_uis_never);
    attach_method("UserInputService", "IsNavigationGamepad", m_uis_never);
    attach_method("UserInputService", "GetKeysPressed", m_uis_keys_pressed);
    attach_method("UserInputService", "GetMouseButtonsPressed",
                  m_uis_mouse_buttons_pressed);
    attach_method("UserInputService", "GetConnectedGamepads", m_uis_empty_table);
    attach_method("UserInputService", "GetNavigationGamepads", m_uis_empty_table);
    attach_method("UserInputService", "GetSupportedGamepadKeyCodes",
                  m_uis_empty_table);
    attach_method("UserInputService", "GetFocusedTextBox", m_uis_nil);
    attach_method("UserInputService", "GetMouseLocation", m_uis_mouse_location);
    attach_method("UserInputService", "GetLastInputType", m_uis_last_input_type);
    attach_method("UserInputService", "GetDeviceType", m_uis_device_type);
    attach_method("UserInputService", "GetPlatform", m_uis_platform);
    attach_method("BindableEvent", "Fire", m_bindable_event_fire);
    attach_method("BindableFunction", "Invoke", m_bindable_function_invoke);
    attach_method("Humanoid", "MoveTo", m_humanoid_move_to);
    attach_method("Humanoid", "Move", m_humanoid_move);
    attach_method("Humanoid", "GetState", m_humanoid_get_state);
    attach_method("Humanoid", "ChangeState", m_humanoid_change_state);
    attach_method("Humanoid", "GetStateEnabled", m_humanoid_get_state_enabled);
    attach_method("Humanoid", "SetStateEnabled", m_humanoid_set_state_enabled);
    attach_method("Humanoid", "GetMoveVelocity", m_humanoid_get_move_velocity);
    attach_method("Humanoid", "GetAccessories", m_humanoid_get_accessories);
    attach_method("Humanoid", "TakeDamage", m_take_damage);
    attach_method("Sound", "Play", m_sound_play);
    attach_method("Sound", "play", m_sound_play);
    attach_method("Sound", "Stop", m_sound_stop);
    attach_method("Sound", "stop", m_sound_stop);
    attach_method("Sound", "Pause", m_sound_pause);
    attach_method("Sound", "pause", m_sound_pause);
    attach_method("Sound", "Resume", m_sound_resume);
    attach_method("Sound", "GetPlaybackState", m_sound_get_playback_state);
    attach_method("Sound", "GetWorldCFrame", m_sound_get_world_cframe);
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
    attach_method("TeleportService", "GetArrivingTeleportGui",
                  m_teleport_get_arriving_gui);
    attach_method("TeleportService", "GetLocalPlayerTeleportData",
                  m_teleport_get_local_data);
    attach_method("TeleportService", "GetTeleportSetting",
                  m_teleport_get_setting);
    attach_method("TeleportService", "SetTeleportSetting",
                  m_teleport_set_setting);
    attach_method("TeleportService", "SetTeleportGui", m_teleport_noop);
    attach_method("TeleportService", "Block", m_teleport_noop);
    attach_method("TeleportService", "Teleport", m_teleport_action);
    attach_method("TeleportService", "TeleportToPlaceInstance",
                  m_teleport_action);
    attach_method("TeleportService", "TeleportToSpawnByName",
                  m_teleport_action);
    attach_method("TeleportService", "TeleportToPrivateServer",
                  m_teleport_action);
    attach_method("TeleportService", "TeleportReconnect", m_teleport_action);
    attach_method("TeleportService", "TeleportSwitchServer",
                  m_teleport_action);
    attach_method("TeleportService", "TeleportAsync", m_teleport_async);
    attach_method("TeleportService", "GetThirdPartyTeleportInfo",
                  m_teleport_get_local_data);
    attach_method("TeleportService", "TeleportedPlacesBackHistory",
                  m_teleport_empty);
    attach_method("TeleportService", "TeleportedUniversesBackHistory",
                  m_teleport_empty);
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
