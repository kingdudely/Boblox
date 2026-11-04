// joindata.h — JoinData snapshot -> live DataModel instances.
//
// Engine-profile only: builds an rbx::Instance world from parsed JoinData
// (rbxclient::JoinSchema + JoinRecords). Never reachable from the Challenge
// (0x9B solve) path — the challenge sandbox never sees these symbols.
//
// Two passes: (1) spawn every record (class by schema name) + set scalar
// props; (2) resolve Instance refs + parents. Records whose parent id is
// unknown (id 13 = the implicit DataModel root, plus live-dynamic grafts)
// parent under a fresh DataModel root. Anything unmappable is counted in
// ApplyStats, never guessed: Tags (16B guid), bytecode Source (16B hash),
// undeclared/version-skew props, null refs, and exotic tags (which fail at
// parse time instead).
#pragma once

#include "solve/replicate.h"

#include "instance/refcount.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct lua_State;

namespace rbx {
class Instance;
}

namespace rbxclient {

struct ApplyStats {
    int spawned = 0;
    int skipped_records = 0; // unknown class
    int props_set = 0;
    int props_skipped_undeclared = 0; // incl. wire-only (Capabilities, ...)
    int props_skipped_readonly = 0;
    int props_skipped_typemismatch = 0;
    int props_skipped_unresolvable = 0; // Tags guid, Source hash, ...
    int refs_missing = 0;               // ref/parent id not in snapshot
    int grafted_to_root = 0;            // parents unknown (incl. id 13)
    int deferred_to_parent = 0;         // parent not seen yet (native defers)
    // wire-name -> registry-name aliases taken (case-insensitive match).
    std::map<std::string, std::string> aliases;
    // (class.prop -> count) for undeclared skips (audit surface).
    std::map<std::string, int> skip_names;
};

struct ApplyReport {
    rbx::Ref<rbx::Instance> root; // fresh DataModel holding the snapshot
    std::vector<rbx::Ref<rbx::Instance>> instances; // all spawned (id order)
    ApplyStats stats;
    // Wire id -> instance index in `instances` (-1 = record skipped).
    std::map<uint32_t, int> by_id;
    // Child id -> parent id whose record had not arrived yet. The native
    // defers these instead of grafting; the parent can arrive in a later
    // batch, so grafting to root loses the tree permanently.
    std::map<uint32_t, uint32_t> pending_parent;
};

// Decode one raw property value into its Luau Variant alternative,
// following the class's DECLARED property type (registry). Returns false
// + reason when the value cannot materialize (unresolvable/unsupported);
// refs resolve in pass 2 (out_refs collects (prop, lo, id)).

ApplyReport applyJoinSnapshot(const JoinSchema& schema,
                              const std::vector<JoinRecord>& records,
                              std::string* err);

// Attach every record still waiting on a parent that the snapshot never
// defined to the DataModel root. The native client creates the services
// (Players/Workspace/…) locally and streams their children; a parent that is
// absent from the snapshot is exactly one of those client-side services, so
// leaving the record detached produces a world whose root has no children
// (no Players -> no PlayerAdded, no avatar link). The queue is kept, so a
// parent that later streams in still re-parents its children (attachPending).
void flushDeferredToRoot(ApplyReport& rep);

// ---- Streaming apply (live 0x02 spawns + 0x0d entry records) ------------
// Incremental spawn-or-update onto an existing ApplyReport (typically the
// snapshot world). Props reuse the exact snapshot decoders (same tags,
// same aliases, same skip counters in rep.stats). Missing parents are queued
// in pending_parent, matching the native deferral, and attach when the parent
// streams in; root-only records attach to the root immediately. Refs
// resolve-or-nil immediately (same). Events have no Luau fire yet (scheduler
// bridge is follow-up): they are only logged/counted. Never fails for data
// reasons (unknown classes/records count, like the snapshot path).
struct StreamStats {
    int spawned = 0;        // new instances from stream records
    int updated = 0;        // records for already-known ids
    // Root attachments/reparent failures; missing parents defer.
    int stream_grafted = 0;
    int events_logged = 0;  // 0x07 invocations counted (no fire yet)
    std::map<std::string, int> events_by_name; // class.event -> count
    int entries_applied = 0; // 0x0d/0x0b/0x25 records folded in
    int prop_updates = 0;   // 0x03 triples parsed
    int prop_applied = 0;   // triples resolved through the schema table
    int prop_missing = 0;   // triples for an entity/name not in the world
    int events_fired = 0;   // 0x07 invocations delivered to a Signal
    int bulks = 0;          // 0x78 script-data bulks (bytecode carried)
};
// One record (spawn loops concatenated by the caller, or one entry).
void applyStreamRecord(ApplyReport& rep, const JoinSchema& schema,
                       uint32_t id, uint16_t cls,
                       const std::vector<JoinProp>& props, int64_t parent,
                       StreamStats& st);
// One 0x03 property update onto an existing instance. prop_tag/table
// resolution is the caller's (schema-derived PropTagTable); this applies
// exactly one JoinProp and never reparents. Returns false when the entity
// is unknown or the property cannot materialize.
bool applyStreamProp(ApplyReport& rep, const JoinSchema& schema, uint32_t id,
                     uint16_t cls, const JoinProp& prop, StreamStats& st);
// One event invocation (count only).
void logStreamEvent(const JoinSchema& schema, const EventItem& ev,
                    StreamStats& st);
// One event invocation delivered to the target instance's Signal. Args are
// not decoded yet (the parser carries them; Variant conversion is follow-up),
// so handlers run with no arguments. Returns false when the entity is not in
// the world.
bool fireStreamEvent(ApplyReport& rep, const JoinSchema& schema,
                     const EventItem& ev, lua_State* L, StreamStats& st);

} // namespace rbxclient
