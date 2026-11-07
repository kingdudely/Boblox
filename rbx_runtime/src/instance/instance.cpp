// instance.cpp — base Instance: tree operations + the shared Lua glue.
#include "instance/instance.h"

#include "instance/scheduler.h"
#include "instance/signal.h"
#include "instance/teardown.h"

#include "lua.h"
#include "lualib.h"

#include <cctype>
#include <cstring>
#include <string>

namespace rbx {

namespace {
long long g_local_user_id = 0;
std::string g_local_user_name;

// ASCII case-insensitive compare: the DataModel shorthands differ from their
// service class only by case (workspace/Workspace, lighting/Lighting).
bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}
}

void set_local_user_id(long long user_id) { g_local_user_id = user_id; }
void set_local_user_name(const std::string& name) { g_local_user_name = name; }
const std::string& local_user_name() { return g_local_user_name; }
long long local_user_id() { return g_local_user_id; }

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
    events_.clear();   // dump Event signals (same ownership as changed_)
    attr_changed_.clear();
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
    // Players.LocalPlayer / NumPlayers are live views over the service's
    // children, not stored values. Server replication can spawn the local
    // Player after attach, so a cached null would stay wrong forever.
    if (cls_ && class_isa(cls_, "Players")) {
        if (n == "LocalPlayer" || n == "localPlayer") {
            Instance* fallback = nullptr;
            const long long self = local_user_id();
            for (const auto& c : children_) {
                if (!c->is_a("Player"))
                    continue;
                if (!fallback)
                    fallback = c.get();
                if (self != 0) {
                    Variant uid;
                    if (c->get_prop("UserId", uid))
                        if (const auto* id = std::get_if<int64_t>(&uid);
                            id && *id == self) {
                            out = Variant(c.get());
                            return true;
                        }
                }
                // UserId is not always replicated; the name is what the join
                // script gives us, so fall back to it before picking anyone.
                if (!g_local_user_name.empty() && c->name() == g_local_user_name) {
                    out = Variant(c.get());
                    return true;
                }
            }
            // Nothing matched. The real engine returns nil here: the local
            // Player is created by the join, never replicated to us, so it is
            // nil until it exists. Returning the first Player in the list made
            // "not there yet" indistinguishable from "there" and handed every
            // caller a stranger's body (measured 2026-11-05: LocalPlayer was
            // LeOnChIk_ponchik10 while the join named poopnestomegahater).
            // Keep the old fallback only when nothing identifies us at all.
            if (self == 0 && g_local_user_name.empty())
                out = Variant(fallback);
            else
                out = Variant(static_cast<Instance*>(nullptr));
            return true;
        }
        if (n == "NumPlayers" || n == "numPlayers") {
            int64_t count = 0;
            for (const auto& c : children_)
                if (c->is_a("Player"))
                    ++count;
            out = Variant(count);
            return true;
        }
    }
    auto it = props_.find(n);
    if (it == props_.end())
        return false;
    // The dump declares DataModel's service shorthands as readonly Instance
    // properties (Workspace, workspace, lighting, RunService). Native resolves
    // them to the service children; the client creates those services after
    // the tree is attached, so a stored null would stay nil forever.
    if (cls_ && class_isa(cls_, "DataModel")) {
        const auto* stored = std::get_if<Instance*>(&it->second);
        if (stored && !*stored) {
            for (const auto& c : children_) {
                if (iequals(c->name_, n) || iequals(c->class_name(), n)) {
                    out = Variant(c.get());
                    return true;
                }
            }
        }
    }
    out = it->second;
    return true;
}

void Instance::set_internal_prop(const std::string& n, const Variant& v) {
    if (!find_property(cls_, n))
        return;
    if (v.index() != find_property(cls_, n)->default_value.index())
        return;
    props_[n] = v;
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
    if (n == "Name") {
        set_name(std::get<std::string>(v)); // truncates name_ ...
        props_[n] = name_; // ... and the stored value (read paths must agree)
    }
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

Signal* Instance::event_signal(const std::string& n) {
    auto it = events_.find(n);
    if (it == events_.end()) {
        Ref<Signal> sig(new Signal(cls_->name + "." + n));
        events_[n] = sig;
        return sig.get();
    }
    return it->second.get();
}

bool Instance::get_attr(const std::string& n, Variant& out) const {
    auto it = attrs_.find(n);
    if (it == attrs_.end())
        return false;
    out = it->second;
    return true;
}

void Instance::set_attr(const std::string& n, const Variant& v) {
    attrs_[n] = v;
}

void Instance::remove_attr(const std::string& n) {
    attrs_.erase(n);
}

std::vector<std::string> Instance::attr_names() const {
    std::vector<std::string> out;
    for (const auto& kv : attrs_)
        out.push_back(kv.first);
    return out; // map order = alphabetical (documented, deterministic)
}

Signal* Instance::attr_changed_signal(const std::string& n) {
    auto it = attr_changed_.find(n);
    if (it == attr_changed_.end()) {
        Ref<Signal> sig(new Signal(cls_->name + ".AttrChanged:" + n));
        attr_changed_[n] = sig;
        return sig.get();
    }
    return it->second.get();
}

Ref<Instance> Instance::clone(std::string* err) const {
    Variant arch;
    if ((get_prop("Archivable", arch) && std::get_if<bool>(&arch) && !std::get<bool>(arch))) {
        if (err)
            *err = std::string("cannot clone ") + cls_->name + ": Archivable is false";
        return Ref<Instance>();
    }
    Ref<Instance> c(new Instance(cls_));
    c->props_ = props_;
    c->attrs_ = attrs_;
    c->tags_ = tags_;
    if (auto* n = std::get_if<std::string>(&c->props_["Name"]))
        c->name_ = *n;
    for (const auto& k : children_) {
        std::string child_err;
        Ref<Instance> kc = k->clone(&child_err);
        if (!kc.get()) {
            if (err)
                *err = child_err;
            return Ref<Instance>();
        }
        kc->parent_ = c.get();
        c->children_.push_back(kc);
    }
    return c; // UNPARENTED (Roblox semantic): the caller reparents explicitly
}

// ---- Lua userdata glue --------------------------------------------------------

namespace {

struct InstUD {
    Ref<Instance> inst;
};

constexpr const char* kMT = "Instance";

int inst_index(lua_State* L);
int inst_newindex(lua_State* L);

// Generated method stub: upvalue(1) = class name, upvalue(2) = method name.
// Reads "Cls.Member is not implemented" — legible, greppable, distinct from
// unknown-member errors (those never reach a callable).
int stub_unimplemented(lua_State* L) {
    const char* cls = lua_tostring(L, lua_upvalueindex(1));
    const char* member = lua_tostring(L, lua_upvalueindex(2));
    luaL_error(L, "%s.%s is not implemented",
               cls ? cls : "?", member ? member : "?");
    return 0;
}

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
    const char* name = luaL_checkstring(L, 2);
    const bool recursive = lua_toboolean(L, 3) != 0;
    Instance* c = inst->find_first_child(name);
    if (!c && recursive) { // depth-first, document order (push reversed)
        const auto& kids0 = inst->children();
        std::vector<Instance*> stack;
        for (size_t i = kids0.size(); i-- > 0;)
            stack.push_back(kids0[i].get());
        while (!c && !stack.empty()) {
            Instance* cur = stack.back();
            stack.pop_back();
            if (cur->name() == name) {
                c = cur;
                break;
            }
            const auto& kids = cur->children();
            for (size_t i = kids.size(); i-- > 0;)
                stack.push_back(kids[i].get());
        }
    }
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

    // Per-class methods (walk the chain: derived shadows base). Real fns push
    // directly; generated stubs arrive as a closure carrying their identity.
    if (const MethodInfo* m = find_method(inst->class_info(), k)) {
        if (m->fn) {
            lua_pushcfunction(L, m->fn, k);
            return 1;
        }
        lua_pushlstring(L, inst->class_name().data(), inst->class_name().size());
        lua_pushstring(L, k);
        lua_pushcclosure(L, stub_unimplemented, "unimplemented", 2);
        return 1;
    }
    // Per-class events resolve to the instance's signal (created on access).
    if (find_event(inst->class_info(), k)) {
        Signal::push(L, inst->event_signal(k));
        return 1;
    }
    // Instance.Changed is a base event every real instance has, but it is not
    // in the generated per-class tables. LocalScripts use it constantly; without
    // it they die with "Changed is not a valid member of Humanoid" (measured on
    // 189707 with replicated LocalScripts).
    if (std::strcmp(k, "Changed") == 0) {
        Signal::push(L, inst->event_signal("Changed"));
        return 1;
    }

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
    // Children by name, the way every real script indexes the tree:
    // game.Players, workspace.Baseplate, gui.Frame. Declared members win over
    // children (native lookup order), so this is the last fallback.
    if (Instance* child = inst->find_first_child(k)) {
        Instance::push(L, child);
        return 1;
    }
    luaL_error(L, "%s is not a valid member of %s", k, inst->class_name().c_str()); return 0;
}

int inst_newindex(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    const char* k = luaL_checkstring(L, 2);

    if (std::strcmp(k, "Parent") == 0) {
        Instance* np = lua_isnil(L, 3) ? nullptr : Instance::check(L, 3);
        Instance* old_parent = inst->parent();
        std::string err;
        if (!inst->reparent(np, &err)) {
            luaL_error(L, "%s", err.c_str());
            return 0;
        }
        if (np) // wake matching WaitForChild parks (event-driven, exact)
            scheduler(L)->notify_child_added(L, np, inst);
        // Universal Instance ancestry signals. Fire after the tree change so
        // handlers observe the new state, matching the native ordering.
        if (old_parent && find_event(old_parent->class_info(), "ChildRemoved"))
            old_parent->event_signal("ChildRemoved")->fire(L, {Variant(inst)});
        if (np && find_event(np->class_info(), "ChildAdded"))
            np->event_signal("ChildAdded")->fire(L, {Variant(inst)});
        for (Instance* a = old_parent ? old_parent->parent() : nullptr; a;
             a = a->parent())
            if (find_event(a->class_info(), "DescendantRemoving"))
                a->event_signal("DescendantRemoving")->fire(L, {Variant(inst)});
        for (Instance* a = np ? np->parent() : nullptr; a; a = a->parent())
            if (find_event(a->class_info(), "DescendantAdded"))
                a->event_signal("DescendantAdded")->fire(L, {Variant(inst)});
        if (find_event(inst->class_info(), "AncestryChanged"))
            inst->event_signal("AncestryChanged")
                ->fire(L, {Variant(inst),
                           np ? Variant(np)
                              : Variant(static_cast<Instance*>(nullptr))});
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

    Variant v = check_variant(L, 3, p->type, p->enum_type);
    std::string err;
    if (!inst->set_prop(k, v, &err)) {
        luaL_error(L, "%s", err.c_str());
        return 0;
    }
    inst->changed_signal(k)->fire(L); // GetPropertyChangedSignal("k")
    // Instance.Changed(property) fires for every set, in addition to the
    // per-property signal above.
    inst->event_signal("Changed")->fire(L, {Variant(std::string(k))});
    // Renames wake WaitForChild parks too: scripts set Name AFTER parenting
    // (Instance.new("Part", p) then .Name = "X"), so add-notify alone — which
    // sees only the default name — would miss every such waiter.
    if (std::strcmp(k, "Name") == 0 && inst->parent())
        scheduler(L)->notify_child_added(L, inst->parent(), inst);
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
