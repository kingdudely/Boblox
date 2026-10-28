// standardize.h — wire bytecode -> standard Luau bytecode (opcode rewrite).
//
// Faithful port of tools/standardize_wire.py; the opcode tables
// (REMAP/OPLEN) are embedded from tools/roblox_bc_tables.json.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rbxclient {

struct Stats {
    int protos = 0;
    int starts = 0;      // instruction-start words walked
    bool bad_walk = false;
    int bad_proto = -1;  // first proto whose walk did not consume sizecode
};

// Rewrite wire opcodes to standard Luau opcodes in place.
// Returns false (+ *err) on parse failure or a non-consuming walk.
bool standardize(std::vector<uint8_t>& buf, Stats* stats, std::string* err);

} // namespace rbxclient
