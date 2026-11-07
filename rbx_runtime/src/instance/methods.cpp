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

#include <curl/curl.h>

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
    Instance* model = Instance::check(L, 1);
    rbx::Variant pv, cfv;
    if (model->get_prop("PrimaryPart", pv))
        if (const auto* p = std::get_if<Instance*>(&pv); p && *p)
            if ((*p)->get_prop("CFrame", cfv))
                if (const auto* cf = std::get_if<rbx::CFrame>(&cfv)) {
                    rbx::push_cframe(L, *cf);
                    return 1;
                }
    if (model->get_prop("WorldPivot", cfv))
        if (const auto* cf = std::get_if<rbx::CFrame>(&cfv)) {
            rbx::push_cframe(L, *cf);
            return 1;
        }
    rbx::push_cframe(L, rbx::CFrame{});
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

int m_datastore_get_async(lua_State* L) {
    Instance* store = Instance::check(L, 1);
    const char* key = luaL_checkstring(L, 2);
    datastore_data(L, store); // [data]
    lua_getfield(L, -1, key);
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
    return 0;
}

int m_datastore_remove_async(lua_State* L) {
    Instance* store = Instance::check(L, 1);
    const char* key = luaL_checkstring(L, 2);
    datastore_data(L, store); // [data]
    lua_getfield(L, -1, key);
    lua_pushnil(L);
    lua_setfield(L, -3, key);
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
    lua_pop(L, 1);
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
    attach_method("Instance", "QueryDescendants", m_query_descendants);
    attach_method("Model", "GetExtentsSize", m_model_get_extents_size);
    attach_method("Model", "GetBoundingBox", m_model_get_bounding_box);
    attach_method("Model", "GetPivot", m_model_get_pivot);
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
    attach_method("BindableEvent", "Fire", m_bindable_event_fire);
    attach_method("BindableFunction", "Invoke", m_bindable_function_invoke);
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
