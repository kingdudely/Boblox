// repl_attributes.h — the per-instance attribute section of a 0xA1 record.
//
// A 0xA1 record's mask has two regions:
//   [0, named_slots)                 writer member slots (ordinary properties)
//   named_slots + 2*id               attribute-schema entry
//   named_slots + 2*id + 1           attribute value
//
// A schema entry is `00 <tag> [<u32 enum-name length><enum name> if tag 7]
// <u32 name length> <name>`. A value entry is the ordinary tagged value for
// that schema's tag. Schema and value arrive as
// independent mask bits, so a record may announce an attribute without a
// value; the schema is kept per entity and the value applies later. This is
// the native layout, confirmed against 2,743's InstanceStateSerialization
// deserializer (`deserializeAttributeSchema` / `deserializeAttributeValue`)
// and measured over 2,000 live frames.
#pragma once

#include "solve/replicate.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rbxclient {

// Decode the attribute section starting at `d` (the bytes immediately after
// the named member values). `named` is the writer's named-slot count, i.e.
// the first attribute bit. `entity_id` keys schema persistence, including the
// null-ref sentinel: the live null records are the local instance's updates
// and their schemas must survive across records just like a normal entity's.
//
// Returns false only for a structurally invalid section. `updates` receives
// value entries in wire order; schema-only entries produce no update.
// `consumed` is the number of attribute-section bytes read. If a value has no
// retained schema, native does not read it: `dropped` is set and the caller
// must end-seek the rest of the body, exactly like sub_6431154's LABEL_99
// branch. Values decoded before that point remain in `updates`.
bool parseA1Attributes(const uint8_t* d, size_t len, uint32_t entity_id,
                       size_t named, const std::vector<uint64_t>& mask_words,
                       A1AttributeSchemas* schemas,
                       std::vector<A1AttributeUpdate>* updates,
                       std::string* err, size_t* consumed = nullptr,
                       bool* dropped = nullptr,
                       const uint8_t* guard = nullptr,
                       size_t guard_len = 0);

} // namespace rbxclient
