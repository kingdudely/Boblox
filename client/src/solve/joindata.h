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

#include "solve/repl_pv.h"
#include "solve/replicate.h"

#include "instance/refcount.h"
#include "instance/variant.h"

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
    int attrs_set = 0;                  // 0xA1 attributes applied
    int attrs_deferred = 0;             // attribute for an unspawned entity
    int attrs_removed = 0;              // schema type 0 (SetAttribute(nil))
    int attrs_unsupported = 0;          // value type not representable
    int pv_values = 0;                  // tag-57 ReplicationPV values decoded
    int pv_invalid = 0;                 // tag-57 values the codec rejected
    int refs_missing = 0;               // ref/parent id not in snapshot
    int grafted_to_root = 0;            // parents unknown (incl. id 13)
    int deferred_to_parent = 0;         // parent not seen yet (native defers)
    // wire-name -> registry-name aliases taken (case-insensitive match).
    std::map<std::string, std::string> aliases;
    // (class.prop -> count) for undeclared skips (audit surface).
    std::map<std::string, int> skip_names;
    int source_hashes = 0; // script Source content keys recorded
};

// One 0x03 triple that arrived before its entity did. The native replicator
// keeps the update and applies it on spawn; dropping it loses the property
// for the rest of the session (MM2's BallSocketConstraint attachments).
struct DeferredProp {
    uint16_t cls = 0;
    JoinProp prop;
};

// A 0xA1 attribute update that arrived before its entity did. Attribute
// values use the same wire value grammar as properties but are addressed by
// the entity's attribute schema rather than by property name.
struct DeferredAttr {
    A1AttributeUpdate attr;
};
// One replicated attribute write that landed on a live instance. The
// streaming layer drains this after the record and fires AttributeChanged
// under the Luau state, because ApplyReport has no access to a lua_State.
struct AttrSignal {
    rbx::Ref<rbx::Instance> instance;
    std::string name;
};

struct ApplyReport {
    rbx::Ref<rbx::Instance> root; // fresh DataModel holding the snapshot
    std::vector<rbx::Ref<rbx::Instance>> instances; // all spawned (id order)
    ApplyStats stats;
    // Wire id -> instance index in `instances` (-1 = record skipped).
    std::map<uint32_t, int> by_id;
    // Internal BasePart.ReplicationPV state keyed by wire entity id. The
    // property is Hidden/NotScriptable/RobloxSecurity in native, so this is
    // engine-side state: no Luau property is installed for it.
    std::map<uint32_t, ReplicationPV> replication_pv;
    // Child id -> parent id whose record had not arrived yet. The native
    // defers these instead of grafting; the parent can arrive in a later
    // batch, so grafting to root loses the tree permanently.
    std::map<uint32_t, uint32_t> pending_parent;
    // Script content key: the 16-byte Source hash a script spawn carries
    // (tag 47) -> the instance id that owns it. A 0x78 script-data bulk is
    // keyed by this hash (its trailing 8 bytes), not by adjacency to the
    // spawn, so big games deliver sources long after the spawn item.
    std::map<std::string, uint32_t> source_hash;   // 16 raw bytes -> id
    // Half of a Source hash -> id. A 0x78 bulk carries 8 key bytes (one half
    // of the 16B hash), and the hash is not adjacent to the spawn in big
    // games, so attribution goes through this index. A multimap because two
    // hashes can share one half.
    std::multimap<std::string, uint32_t> source_half;
    // Entity id -> triples waiting for that entity to spawn. Flushed by
    // applyStreamRecord when the id arrives; capped so a stale id cannot
    // grow without bound.
    std::map<uint32_t, std::vector<DeferredProp>> pending_props;
    // Entity id -> attribute values waiting for that entity to spawn.
    std::map<uint32_t, std::vector<DeferredAttr>> pending_attrs;
    // Changes applied during this stream record, fired by the caller after
    // the record is fully applied. Bounded so a stream cannot grow it forever.
    std::vector<AttrSignal> attr_signals;
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
// resolve-or-nil immediately (same). 0x07 event invocations are decoded onto
// the Lua stack and dispatched to the target Signal (typed arguments,
// including nested tables); unsupported wire shapes become nil in place and
// are counted, never silently shifted. Never fails for data reasons (unknown
// classes/records count, like the snapshot path).
struct StreamStats {
    int spawned = 0;        // new instances from stream records
    int updated = 0;        // records for already-known ids
    // Root attachments/reparent failures; missing parents defer.
    int stream_grafted = 0;
    int events_logged = 0;  // 0x07 invocations parsed/counted
    std::map<std::string, int> events_by_name; // class.event -> count
    int entries_applied = 0; // 0x0d/0x0b/0x25 records folded in
    int prop_updates = 0;   // 0x03 triples parsed
    int prop_applied = 0;   // triples resolved through the schema table
    int prop_missing = 0;   // triples for an entity/name not in the world
    int prop_deferred = 0;  // triples queued until their entity spawns
    int prop_dropped = 0;   // deferred triples discarded by the queue cap
    int events_fired = 0;   // 0x07 invocations delivered (typed args)
    int bulks = 0;          // 0x78 script-data bulks (bytecode carried)
    int acks = 0;           // 0x0a acknowledgement records
    int regions_removed = 0;      // 0x0e region-removal records
    int region_guids_removed = 0; // instances named by those records
    int client_raw_batches = 0;   // 0x1c inner chains kept byte-exact
    int client_unknown_events = 0; // raw chains stopped on an unrouted 0x07 id
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
// One replicated attribute value. Values for an unspawned entity are queued
// and flushed by applyStreamRecord when it appears. Returns false only for a
// value type the runtime cannot represent (counted in attrs_unsupported).
bool applyStreamAttribute(ApplyReport& rep, uint32_t id,
                          const A1AttributeUpdate& attr, StreamStats& st);
// Native prop updates whose id equals the schema's member-count bound are
// the out-of-band Parent property (ref-typed, 5 bytes), not a member. A
// negative parent detaches; an unspawned parent is queued in
// pending_parent exactly like a spawn parent, so the child attaches when
// the parent streams in. Returns false when the entity is unknown.
bool applyParentUpdate(ApplyReport& rep, uint32_t id, int64_t parent,
                       StreamStats& st);
// One event invocation (count only).
void logStreamEvent(const JoinSchema& schema, const EventItem& ev,
                    StreamStats& st);
// One event invocation delivered to the target instance's Signal. Arguments
// are decoded onto L's stack (typed values, nested tables, resolved refs) and
// also carried to Signal:Wait-ers. Unsupported shapes become nil in place.
// Returns false when the entity is not in the world.
bool fireStreamEvent(ApplyReport& rep, const JoinSchema& schema,
                     const EventItem& ev, lua_State* L, StreamStats& st);

} // namespace rbxclient
