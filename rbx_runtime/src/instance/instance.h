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
#include <memory>
#include <set>
#include <string>
#include <vector>

struct lua_State;

namespace rbx {

class Signal;

// Signed-in account for this DataModel, set by the live client before the
// world applies. 0 means "no local player" (offline/playback). Players
// methods use it to select LocalPlayer exactly like the real engine, which
// keys off the authenticated UserId rather than an array position.
void set_local_user_id(long long user_id);
void set_local_user_name(const std::string& name);
const std::string& local_user_name();
long long local_user_id();
// Account name of the signed-in user, installed next to the id. A replicated
// Player whose spawn record carries no UserId is still recognisable as ours by
// this name, which is what keeps LocalPlayer single (the native has one Player
// instance per account). Empty when the name is unknown.
void set_local_user_name(const std::string& name);
const std::string& local_user_name();

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
    // Names truncate at 100 chars (native behavior — verified in the 2016
    // reference: setName substrs). Single enforcement point: set_prop routes
    // Name writes through here.
    void set_name(std::string n) {
        name_ = n.size() > 100 ? n.substr(0, 100) : std::move(n);
    }
    bool destroyed() const {
        return destroyed_;
    }
    // Replication identity (network ref lo + instance id), installed when
    // the instance is materialized from the join snapshot or a live spawn.
    // Client-created instances stay 0; remote serialization rejects those
    // because the server has no ref to resolve.
    void set_network_identity(uint64_t lo, uint32_t id) {
        net_lo_ = lo;
        net_id_ = id;
    }
    uint64_t network_lo() const {
        return net_lo_;
    }
    uint32_t network_id() const {
        return net_id_;
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
    // Stored/declared value only, bypassing dynamic views (Players/CFrame
    // overrides). Used by replication/ownership probes.
    bool get_prop_base(const std::string& name, Variant& out) const;
    // Set with type/readonly enforcement. false + *err on refusal.
    bool set_prop(const std::string& name, const Variant& v, std::string* err);
    // Engine-authored value for a readonly dump property (DataModel.JobId,
    // PlaceId, Workspace). Scripts still cannot write it; the embedder can.
    void set_internal_prop(const std::string& name, const Variant& v);
    // Local headless character override. The server remains authoritative,
    // but scripts need a stable local CFrame while network ownership is
    // unavailable; replication skips this property once set.
    void set_local_cframe(const CFrame& cf);
    bool has_local_cframe() const { return local_cframe_ != nullptr; }
    // The GetPropertyChangedSignal(name) signal (created on first use).
    Signal* changed_signal(const std::string& name);
    // The signal behind a dump Event member (Touched, Died, ...), created on
    // first access. Behavior code fires it when the real condition happens;
    // until then it connects/waits normally and fires never.
    Signal* event_signal(const std::string& name);

    // -- dynamic attributes (SetAttribute/GetAttribute) -----------------------
    // Undeclared name -> value pairs outside the reflection system. false on
    // unknown name; SetAttribute(name, nil) deletes.
    bool get_attr(const std::string& name, Variant& out) const;
    void set_attr(const std::string& name, const Variant& v);
    void remove_attr(const std::string& name);
    std::vector<std::string> attr_names() const;
    // GetAttributeChangedSignal(name), created on first use.
    Signal* attr_changed_signal(const std::string& name);

    // -- CollectionService tags ----------------------------------------------
    // Tags are a string set, separate from attributes (Roblox parity: they do
    // not show up in GetAttributes). Replicated Tags arrive as content hashes
    // and are resolved by the replication layer; script-side AddTag/RemoveTag
    // operate here directly. The set is allocated only when a tag is added.
    void add_tag(const std::string& tag);
    bool remove_tag(const std::string& tag);
    bool has_tag(const std::string& tag) const;
    std::vector<std::string> tags() const;

    // -- clone ----------------------------------------------------------------
    // Deep copy (props, attrs, recursive children). The copy is UNPARENTED
    // (Roblox semantic); non-archivable instances refuse with *err.
    Ref<Instance> clone(std::string* err) const;

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
    uint64_t net_lo_ = 0;
    uint32_t net_id_ = 0;
    // Empty containers are the common case; allocate each on first use so a
    // bare Part does not pay six map/set headers (was ~288 bytes/instance).
    std::unique_ptr<std::map<std::string, Variant>> props_; // declared overrides
    std::unique_ptr<std::map<std::string, Ref<Signal>>> changed_;
    std::unique_ptr<std::map<std::string, Ref<Signal>>> events_; // dump events
    std::unique_ptr<std::map<std::string, Ref<Signal>>> attr_changed_;
    std::unique_ptr<std::map<std::string, Variant>> attrs_; // dynamic attrs
    std::unique_ptr<std::set<std::string>> tags_; // CollectionService tags
    std::unique_ptr<CFrame> local_cframe_; // local character override
};

// Lua callback slots for classes whose API is Callback-typed rather than an
// Event (BindableFunction.OnInvoke, RemoteFunction.OnClientInvoke/OnServerInvoke).
// Stored in the Lua registry keyed by instance, because Variant has no function.
void set_instance_callback(lua_State* L, Instance* inst, const char* name,
                           int value_idx);
void push_instance_callback(lua_State* L, Instance* inst, const char* name);

} // namespace rbx
