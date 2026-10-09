// class_registry.h — class metadata + typed property reflection.
//
// Every Instance subclass is described by a ClassInfo: name, superclass
// chain, a table of declared properties (name/type/default/readonly), and a
// factory. One .cpp per surface area declares its classes (see
// instance/classes/) and is listed in classes.cpp — the same explicit-list
// discipline as api/registry.cpp (static-lib linkers drop unreferenced TUs).
//
// Property VALUES live on the instance (name -> Variant map) so any class
// can declare properties declaratively with zero per-class marshalling code;
// if per-class layout matters later, PropInfo can grow getter/setter
// function pointers without touching the classes.
#pragma once

#include "instance/variant.h"

#include "instance/refcount.h"

#include <functional>
#include <string>
#include <vector>

struct lua_State;

namespace rbx {

class Instance;

struct PropInfo {
    std::string name;
    PropType type = PropType::Double;
    Variant default_value;
    bool readonly = false;
};

struct ClassInfo {
    std::string name;
    const ClassInfo* super = nullptr;
    std::vector<PropInfo> props; // declared by THIS class only
    // factory: construct a fresh, UNPARENTED instance of this exact class.
    // Parenting is a tree operation: Instance.new attaches it afterwards
    // (keeps tree logic in one place, factories pure).
    std::function<Ref<Instance>()> factory;
};

// Process-wide class metadata. Registration happens once at engine init
// (classes.cpp list); lookups are read-only afterwards.
void register_class(ClassInfo info);
const ClassInfo* find_class(const std::string& name);
// IsA: walks the super chain of `derived` looking for `base`.
bool class_isa(const ClassInfo* derived, const std::string& base);
// Effective property lookup across the super chain (base -> derived order).
// Returns nullptr if no class in the chain declares `name`.
const PropInfo* find_property(const ClassInfo* cls, const std::string& name);
// All effective properties, base-most first (for docs/introspection).
std::vector<PropInfo> effective_properties(const ClassInfo* cls);

// Register the built-in classes (Instance base + engine classes). Idempotent;
// called by engine environment setup.
void register_builtin_classes();

// ---- Lua marshalling (one implementation, used by every class) -------------
// Push a Variant as its Lua value (Instance* becomes an instance userdata).
void push_variant(lua_State* L, const Variant& v);
// Read a Variant of the given type at index; raises a Lua error on mismatch.
Variant check_variant(lua_State* L, int idx, PropType type);

} // namespace rbx
