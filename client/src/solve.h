// solve.h — full autonomous 0x9B path: message -> answer, in-process.
//
//   [9b][u1][u2][len][blob]
//     -> RSB1 xor + xxhash32 + zstd        (blob.cpp)
//     -> opcode standardization            (standardize.cpp)
//     -> Luau VM under production sandbox  (rbx_runtime challenge_core)
//     -> uint32 answer
//
// Mirrors py/solve9b.py exactly (including the u1/u2 argument swap for the
// runner and the bit-exact sandbox profile); validated by test/regress against
// the native's own answers on all captured datasets.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rbxclient {

// Solve a raw 0x9B message for the given JobId. Returns false + *err.
bool solveMessage(const uint8_t* msg, size_t len, const std::string& job, uint32_t& answer,
                  std::string* err);

// Build the 9-byte response: [9b][u2][answer].
std::vector<uint8_t> buildResponse(uint32_t u2, uint32_t answer);

} // namespace rbxclient
