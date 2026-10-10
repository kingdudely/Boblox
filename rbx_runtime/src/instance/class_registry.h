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

#include <string>
#include <vector>

struct lua_State;

namespace rbx {

class Instance;
struct ClassInfo; // factory signatures (below) precede the definition

struct PropInfo {
    std::string name;
    PropType type = PropType::Double;
    Variant default_value;
    bool readonly = false;
};

// A script-callable method. fn == nullptr means "generated stub": the member
// exists (typeof gives "function") but calling it raises a legible
// "not implemented" error. Hand behavior attaches real fns by name
// (attach_method) — same registry, no second hierarchy, Luau can't tell.
struct MethodInfo {
    std::string name;
    int (*fn)(lua_State*) = nullptr; // lua_CFunction, spelled without lua.h
};

// A script signal member (Touched, Died, ...). Resolves to a per-instance
// Signal (created on first access); behavior code fires it when the real
// condition happens. Until then it connects/waits normally, firing never.
struct EventInfo {
    std::string name;
};

// Factory: construct a fresh, UNPARENTED instance of the given class.
// One shared implementation (generic_factory) serves every generated class;
// hand-behavior classes may set a custom one. A plain function pointer keeps
// 936 classes' registrations template-free (one function, zero per-class
// glue for the compiler to instantiate). See instantiate() below.
using FactoryFn = Ref<Instance> (*)(const ClassInfo*);
Ref<Instance> generic_factory(const ClassInfo* cls);

struct ClassInfo {
    std::string name;
    const ClassInfo* super = nullptr;
    std::vector<PropInfo> props;   // declared by THIS class only
    std::vector<MethodInfo> methods; // declared by THIS class only
    std::vector<EventInfo> events;   // declared by THIS class only
    // false for NotCreatable dump classes (abstracts, services): registered
    // (IsA/reflection work) but Instance.new rejects them with the native
    // "Unable to create an Instance of type 'X'" error.
    bool creatable = true;
    FactoryFn factory = nullptr; // null = generic_factory (see instantiate)
};

// Construct via the class's factory (or the generic one when null).
// Parenting stays a tree operation outside this call (see instance_new).
Ref<Instance> instantiate(const ClassInfo* cls);

// Process-wide class metadata. Registration happens once at engine init
// (classes.cpp list); lookups are read-only afterwards.
//
// Two-phase by design: register_class stores the shell (super may be null),
// then link_super wires child -> parent BY NAME. Name-based linking means
// registration ORDER NEVER MATTERS — hand classes can subclass generated
// ones and vice versa (Seat:Part while Part:FormFactorPart), which eager
// super pointers cannot express without a fragile global order.
void register_class(ClassInfo info);
bool link_super(const std::string& child, const std::string& parent);
const ClassInfo* find_class(const std::string& name);
// IsA: walks the super chain of `derived` looking for `base`.
bool class_isa(const ClassInfo* derived, const std::string& base);
// Effective property lookup across the super chain (base -> derived order).
// Returns nullptr if no class in the chain declares `name`.
const PropInfo* find_property(const ClassInfo* cls, const std::string& name);
// Method/event lookup, derived-first (a derived member shadows its base).
// Returns nullptr when no class in the chain declares the member.
const MethodInfo* find_method(const ClassInfo* cls, const std::string& name);
const EventInfo* find_event(const ClassInfo* cls, const std::string& name);
// Attach hand-written behavior to a method slot (generated or hand alike).
// Upserts: a missing method is created, so behavior never silently goes
// missing when the dump renames something (tests pin every behavior).
void attach_method(const char* cls, const char* method, int (*fn)(lua_State*));
// All effective properties, base-most first (for docs/introspection).
std::vector<PropInfo> effective_properties(const ClassInfo* cls);

// Register the built-in classes (Instance base + engine classes). Idempotent;
// called by engine environment setup.
void register_builtin_classes();

// Engine-profile service singletons (emitted by gen_instances.py into
// generated.cpp): one stable userdata per Service-tagged class, registered
// into rbx.Services. Called from register_engine ONLY (never the Challenge
// profile — its fresh-table fallback is frozen behavior).
void register_service_singletons(lua_State* L);

// ---- Lua marshalling (one implementation, used by every class) -------------
// Push a Variant as its Lua value (Instance* becomes an instance userdata).
void push_variant(lua_State* L, const Variant& v);
// Read a Variant of the given type at index; raises a Lua error on mismatch.
Variant check_variant(lua_State* L, int idx, PropType type);

} // namespace rbx
