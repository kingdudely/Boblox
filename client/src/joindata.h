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

#include "replicate.h"

#include "instance/refcount.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

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
};

// Decode one raw property value into its Luau Variant alternative,
// following the class's DECLARED property type (registry). Returns false
// + reason when the value cannot materialize (unresolvable/unsupported);
// refs resolve in pass 2 (out_refs collects (prop, lo, id)).

ApplyReport applyJoinSnapshot(const JoinSchema& schema,
                              const std::vector<JoinRecord>& records,
                              std::string* err);

} // namespace rbxclient
