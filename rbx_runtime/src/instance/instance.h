// instance.h — the base Instance: identity, name, tree, and Lua userdata glue.
//
// Tree model (matches Roblox semantics closely enough for the engine path):
//   * parent holds children STRONGLY; children hold the parent WEAKLY
//     (no ref cycles; Destroy detaches predictably).
//   * Parent = nil detaches; cycles are rejected.
//   * Destroy detaches from the parent, destroys children recursively, and
//     locks the instance (Parent becomes read-only, like Roblox).
//
// Everything here is Engine-profile only — the challenge sandbox never sees
// these globals (api/registry.cpp tags the engine module kEngineOnly; the
// Challenge surface is pinned by tests/unit/unit_profile.cpp).
#pragma once

#include "instance/class_registry.h"
#include "instance/refcount.h"

#include <map>
#include <string>
#include <vector>

struct lua_State;

namespace rbx {

class Signal;

class Instance : public RefCounted {
  public:
    Instance(const ClassInfo* cls);

    const ClassInfo* class_info() const {
        return cls_;
    }
    const std::string& class_name() const {
        return cls_->name;
    }

    // -- identity ------------------------------------------------------------
    const std::string& name() const {
        return name_;
    }
    void set_name(std::string n) {
        name_ = std::move(n);
    }
    bool destroyed() const {
        return destroyed_;
    }

    // -- tree ----------------------------------------------------------------
    Instance* parent() const {
        return parent_;
    }
    // Detach + recursively Destroy children + lock (Roblox Destroy).
    void destroy();
    // Env teardown only (teardown.cpp): drop every cross-reference so force
    // deletion order can't touch an already-freed object. After this the
    // instance is an isolated node — use destroy() for the Roblox semantic.
    void detach_all();
    // Reparent (null detaches). Rejects cycles and locked instances.
    bool reparent(Instance* new_parent, std::string* err);
    const std::vector<Ref<Instance>>& children() const {
        return children_;
    }
    Instance* find_first_child(const std::string& name) const;
    Instance* find_first_child_of_class(const std::string& cls) const;
    bool is_a(const std::string& cls) const {
        return class_isa(cls_, cls);
    }

    // -- reflected properties -------------------------------------------------
    // Effective value (declared default unless overridden). Unset non-default
    // access on an undeclared name returns false.
    bool get_prop(const std::string& name, Variant& out) const;
    // Set with type/readonly enforcement. false + *err on refusal.
    bool set_prop(const std::string& name, const Variant& v, std::string* err);
    // The GetPropertyChangedSignal(name) signal (created on first use).
    Signal* changed_signal(const std::string& name);

    // -- Lua userdata glue (instance.cpp) --------------------------------------
    // Push a userdata wrapping this instance (strong ref held by userdata).
    static void push(lua_State* L, Instance* inst); // adds a ref for the userdata
    // Check the userdata at idx; raises a Lua error on type mismatch.
    static Instance* check(lua_State* L, int idx);
    // Non-raising variant; nullptr if not an instance userdata.
    static Instance* get(lua_State* L, int idx);
    // Install the shared "Instance" metatable (idempotent).
    static void create_metatable(lua_State* L);

  private:
    const ClassInfo* cls_;
    std::string name_ = "Instance";
    Instance* parent_ = nullptr; // weak
    std::vector<Ref<Instance>> children_;
    bool destroyed_ = false;
    std::map<std::string, Variant> props_; // overrides of declared defaults
    std::map<std::string, Ref<Signal>> changed_;
};

} // namespace rbx
