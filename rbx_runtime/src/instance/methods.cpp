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
// `return luaL_error(...)`). Yielding loops cannot live in C (Luau has no
// continuations: lua_yield only propagates outward, never re-enters) —
// poll-and-yield logic like WaitForChild is implemented in Lua instead
// (install_lua_methods in lua_methods.cpp).
#include "instance/class_registry.h"
#include "instance/instance.h"
#include "instance/signal.h"
#include "instance/value_types.h"
#include "instance/vector3.h"

#include "lua.h"
#include "lualib.h"

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
    if (type == "Vector3")
        out = Variant(check_vector3(L, idx));
    else if (type == "Color3")
        out = Variant(check_color3(L, idx));
    else if (type == "CFrame")
        out = Variant(check_cframe(L, idx));
    else if (type == "Vector2")
        out = Variant(check_vector2(L, idx));
    else if (type == "BrickColor")
        out = Variant(check_brickcolor(L, idx));
    else if (type == "UDim")
        out = Variant(check_udim(L, idx));
    else if (type == "UDim2")
        out = Variant(check_udim2(L, idx));
    else if (type == "Rect")
        out = Variant(check_rect(L, idx));
    else if (type == "NumberRange")
        out = Variant(check_numberrange(L, idx));
    else if (type == "NumberSequence")
        out = Variant(check_numbersequence(L, idx));
    else if (type == "ColorSequence")
        out = Variant(check_colorsequence(L, idx));
    else if (type == "Content")
        out = Variant(check_content(L, idx));
    else if (type == "PhysicalProperties")
        out = Variant(check_physicalproperties(L, idx));
    else if (type == "Ray")
        out = Variant(check_ray(L, idx));
    else if (type == "Region3")
        out = Variant(check_region3(L, idx));
    else if (type == "DateTime")
        out = Variant(check_datetime(L, idx));
    else if (type == "EnumItem")
        out = Variant(check_enumitem(L, idx)); // any enum (attributes accept all)
    else if (type == "Instance")
        out = Variant(Instance::check(L, idx));
    else
        return false;
    return true;
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

void register_instance_methods() {
    // Universal Instance surface (inherited by every class via the chain).
    // NOTE: no WaitForChild here — a C polling loop cannot re-enter after
    // lua_yield (Luau has no continuations: yield only propagates outward).
    // WaitForChild lives in the per-state Lua methods table instead
    // (install_lua_methods), where loops and yields compose naturally.
    attach_method("Instance", "GetDescendants", m_get_descendants);
    attach_method("Instance", "Clone", m_clone);
    attach_method("Instance", "ClearAllChildren", m_clear_all_children);
    attach_method("Instance", "IsAncestorOf", m_is_ancestor_of);
    attach_method("Instance", "IsDescendantOf", m_is_descendant_of);
    attach_method("Instance", "FindFirstAncestor", m_find_first_ancestor);
    attach_method("Instance", "FindFirstAncestorOfClass", m_find_first_ancestor_of_class);
    attach_method("Instance", "FindFirstAncestorWhichIsA", m_find_first_ancestor_which_is_a);
    attach_method("Instance", "GetFullName", m_get_full_name);
    attach_method("Instance", "GetAttribute", m_get_attribute);
    attach_method("Instance", "SetAttribute", m_set_attribute);
    attach_method("Instance", "GetAttributes", m_get_attributes);
    attach_method("Instance", "GetAttributeChangedSignal", m_get_attribute_changed_signal);
    attach_method("Humanoid", "TakeDamage", m_take_damage);
}

} // namespace rbx
