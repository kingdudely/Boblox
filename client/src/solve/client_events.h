// client_events.h — the native client's own 0x07 event dictionary.
//
// The server schema does not describe the inner 0x1c chain. A client event
// carries [ref][u16 wire id] followed by argument values whose types come
// from the client EventDescriptor, not from the server class. This table is
// the narrow, evidence-backed subset needed by the native corpus; it is not a
// second server schema. Unknown wire ids fail explicitly (the caller keeps the
// body raw and records the id), never silently use the server table.
#pragma once

#include "solve/replicate.h"

#include <cstdint>

namespace rbxclient {

// Type byte stored in EventDescriptor entries at +1 (the first qword's low
// byte is a flag; the writer switches on byte +1). Values use the same tag
// numbering as the server replication codec — the shared writer is
// sub_64F0256 / sub_640BC1E and the reader is sub_340A3E4.
struct ClientEventDef {
    uint16_t id;
    const char* name; // EventDescriptor name (or the proven signature alias)
    uint8_t nargs;
    uint8_t args[5];
};

// nullptr for an id that is not in the proven dictionary.
const ClientEventDef* findClientEvent(uint16_t id);

// One client event invocation at [d, d+n). Sets *unknown_id when the wire id
// is absent from the table; the caller must preserve the body byte-exact.
bool parseClientEventItem(const uint8_t* d, size_t n, EventItem& out,
                          size_t* consumed, bool* unknown_id,
                          std::string* err);

} // namespace rbxclient
