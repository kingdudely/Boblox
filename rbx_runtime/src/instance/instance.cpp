// instance.cpp — base Instance: tree operations + the shared Lua glue.
#include "instance/instance.h"

#include "instance/signal.h"
#include "instance/teardown.h"

#include "lua.h"
#include "lualib.h"

#include <cstring>

namespace rbx {

// ---- C++ side ----------------------------------------------------------------

Instance::Instance(const ClassInfo* cls) : cls_(cls) {
    // Seed declared properties (base -> derived so derived defaults win).
    for (const PropInfo& p : effective_properties(cls))
        props_[p.name] = p.default_value;
    if (auto* n = std::get_if<std::string>(&props_["Name"]))
        name_ = *n;
}

void Instance::destroy() {
    if (destroyed_)
        return;
    destroyed_ = true;
    if (parent_) {
        auto& sibs = parent_->children_;
        for (auto it = sibs.begin(); it != sibs.end(); ++it) {
            if (it->get() == this) {
                sibs.erase(it);
                break;
            }
        }
        parent_ = nullptr;
    }
    // Roblox Destroy tears down the subtree.
    std::vector<Ref<Instance>> kids = std::move(children_);
    children_.clear();
    for (auto& k : kids) {
        k->parent_ = nullptr;
        k->destroy();
    }
}

void Instance::detach_all() {
    children_.clear(); // releases child refs (children are pinned for delete)
    changed_.clear();  // releases owned signals (also pinned for delete)
    parent_ = nullptr;
}

bool Instance::reparent(Instance* np, std::string* err) {
    if (destroyed_) {
        if (err)
            *err = "cannot set Parent of a destroyed instance";
        return false;
    }
    for (Instance* a = np; a; a = a->parent_) {
        if (a == this) {
            if (err)
                *err = "cannot cycle the instance tree";
            return false;
        }
    }
    if (parent_) {
        auto& sibs = parent_->children_;
        for (auto it = sibs.begin(); it != sibs.end(); ++it) {
            if (it->get() == this) {
                sibs.erase(it);
                break;
            }
        }
    }
    parent_ = np;
    if (np)
        np->children_.push_back(Ref<Instance>(this));
    return true;
}

Instance* Instance::find_first_child(const std::string& n) const {
    for (const auto& c : children_) {
        if (c->name_ == n)
            return c.get();
    }
    return nullptr;
}

Instance* Instance::find_first_child_of_class(const std::string& cls) const {
    for (const auto& c : children_) {
        if (c->is_a(cls))
            return c.get();
    }
    return nullptr;
}

bool Instance::get_prop(const std::string& n, Variant& out) const {
    auto it = props_.find(n);
    if (it == props_.end())
        return false;
    out = it->second;
    return true;
}

bool Instance::set_prop(const std::string& n, const Variant& v, std::string* err) {
    const PropInfo* p = find_property(cls_, n);
    if (!p) {
        if (err)
            *err = n + " is not a valid property of " + cls_->name;
        return false;
    }
    if (p->readonly) {
        if (err)
            *err = "Property " + n + " is read only";
        return false;
    }
    if (v.index() != p->default_value.index()) {
        if (err)
            *err = "wrong type for property " + n;
        return false;
    }
    props_[n] = v;
    if (n == "Name")
        name_ = std::get<std::string>(v);
    return true;
}

Signal* Instance::changed_signal(const std::string& n) {
    auto it = changed_.find(n);
    if (it == changed_.end()) {
        Ref<Instance> self(this); // keep alive across the call
        Ref<Signal> sig(new Signal(cls_->name + "." + n));
        changed_[n] = sig;
        return sig.get();
    }
    return it->second.get();
}

// ---- Lua userdata glue --------------------------------------------------------

namespace {

struct InstUD {
    Ref<Instance> inst;
};

constexpr const char* kMT = "Instance";

int inst_index(lua_State* L);
int inst_newindex(lua_State* L);

int inst_get_children(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    lua_newtable(L);
    int i = 1;
    for (const auto& c : inst->children()) {
        Instance::push(L, c.get());
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

int inst_find_first_child(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    Instance* c = inst->find_first_child(luaL_checkstring(L, 2));
    if (c)
        Instance::push(L, c);
    else
        lua_pushnil(L);
    return 1;
}

int inst_find_first_child_of_class(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    Instance* c = inst->find_first_child_of_class(luaL_checkstring(L, 2));
    if (c)
        Instance::push(L, c);
    else
        lua_pushnil(L);
    return 1;
}

int inst_is_a(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    lua_pushboolean(L, inst->is_a(luaL_checkstring(L, 2)) ? 1 : 0);
    return 1;
}

int inst_destroy(lua_State* L) {
    Instance::check(L, 1)->destroy();
    return 0;
}

int inst_get_changed_signal(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    const char* prop = luaL_checkstring(L, 2);
    if (!find_property(inst->class_info(), prop)) {
        luaL_error(L, "%s is not a valid property of %s", prop, inst->class_name().c_str());
        return 0;
    }
    Signal::push(L, inst->changed_signal(prop));
    return 1;
}

int inst_tostring(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    lua_pushfstring(L, "%s (%s)", inst->name().c_str(), inst->class_name().c_str());
    return 1;
}

int inst_eq(lua_State* L) {
    lua_pushboolean(L, Instance::get(L, 1) == Instance::get(L, 2) ? 1 : 0);
    return 1;
}

void methods_table(lua_State* L) { // pushes the shared methods table
    lua_getmetatable(L, 1);
    lua_getfield(L, -1, "Methods");
    lua_remove(L, -2);
}

int inst_index(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    const char* k = luaL_checkstring(L, 2);

    methods_table(L);
    lua_getfield(L, -1, k);
    if (!lua_isnil(L, -1))
        return 1;
    lua_pop(L, 2);

    if (std::strcmp(k, "Parent") == 0) {
        if (inst->parent())
            Instance::push(L, inst->parent());
        else
            lua_pushnil(L);
        return 1;
    }
    if (std::strcmp(k, "ClassName") == 0) { // dynamic: the concrete class name
        lua_pushlstring(L, inst->class_name().data(), inst->class_name().size());
        return 1;
    }
    Variant v;
    if (inst->get_prop(k, v)) {
        push_variant(L, v);
        return 1;
    }
    luaL_error(L, "%s is not a valid member of %s", k, inst->class_name().c_str()); return 0;
}

int inst_newindex(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    const char* k = luaL_checkstring(L, 2);

    if (std::strcmp(k, "Parent") == 0) {
        Instance* np = lua_isnil(L, 3) ? nullptr : Instance::check(L, 3);
        std::string err;
        if (!inst->reparent(np, &err)) {
            luaL_error(L, "%s", err.c_str());
            return 0;
        }
        return 0;
    }
    if (std::strcmp(k, "ClassName") == 0) {
        luaL_error(L, "Property %s is read only", k);
        return 0;
    }
    const PropInfo* p = find_property(inst->class_info(), k);
    if (!p) {
        luaL_error(L, "%s is not a valid member of %s", k, inst->class_name().c_str());
        return 0;
    }
    if (p->readonly) {
        luaL_error(L, "Property %s is read only", k);
        return 0;
    }

    Variant v = check_variant(L, 3, p->type);
    std::string err;
    if (!inst->set_prop(k, v, &err)) {
        luaL_error(L, "%s", err.c_str());
        return 0;
    }
    inst->changed_signal(k)->fire(L); // GetPropertyChangedSignal("k")
    return 0;
}

} // namespace

void Instance::create_metatable(lua_State* L) {
    // Always leaves the metatable on top: luaL_newmetatable pushes it whether
    // it created the metatable or found the existing one.
    if (!luaL_newmetatable(L, kMT))
        return; // already filled
    lua_pushcfunction(L, inst_index, "__index");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, inst_newindex, "__newindex");
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, inst_eq, "__eq");
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, inst_tostring, "__tostring");
    lua_setfield(L, -2, "__tostring");
    // NOTE: no __gc — Luau's VM never invokes userdata finalizers (see
    // teardown.h); engine objects are released by teardown_alive at close.
    // __type drives Luau's typeof(): every instance reports "Instance"
    // regardless of class — exact Roblox parity (luaT_objtypenamestr).
    lua_pushstring(L, "Instance");
    lua_setfield(L, -2, "__type");

    lua_newtable(L); // Methods
    lua_pushcfunction(L, inst_get_children, "GetChildren");
    lua_setfield(L, -2, "GetChildren");
    lua_pushcfunction(L, inst_find_first_child, "FindFirstChild");
    lua_setfield(L, -2, "FindFirstChild");
    lua_pushcfunction(L, inst_find_first_child_of_class, "FindFirstChildOfClass");
    lua_setfield(L, -2, "FindFirstChildOfClass");
    lua_pushcfunction(L, inst_is_a, "IsA");
    lua_setfield(L, -2, "IsA");
    lua_pushcfunction(L, inst_destroy, "Destroy");
    lua_setfield(L, -2, "Destroy");
    lua_pushcfunction(L, inst_get_changed_signal, "GetPropertyChangedSignal");
    lua_setfield(L, -2, "GetPropertyChangedSignal");
    lua_setfield(L, -2, "Methods");
}

void Instance::push(lua_State* L, Instance* inst) {
    auto* ud = static_cast<InstUD*>(lua_newuserdata(L, sizeof(InstUD)));
    new (&ud->inst) Ref<Instance>(inst); // userdata holds a strong ref
    track_alive(L, inst, kAliveInstance); // deterministic teardown (teardown.h)
    create_metatable(L);                  // pushes the metatable
    lua_setmetatable(L, -2);
}

Instance* Instance::check(lua_State* L, int idx) {
    Instance* inst = get(L, idx);
    if (!inst)
        luaL_error(L, "expected an Instance (argument #%d)", idx);
    return inst;
}

Instance* Instance::get(lua_State* L, int idx) {
    if (!lua_isuserdata(L, idx) || !lua_getmetatable(L, idx))
        return nullptr;
    luaL_getmetatable(L, kMT); // registry["Instance"]
    const bool ok = lua_rawequal(L, -1, -2) != 0;
    lua_pop(L, 2);
    return ok ? static_cast<InstUD*>(lua_touserdata(L, idx))->inst.get() : nullptr;
}

} // namespace rbx
