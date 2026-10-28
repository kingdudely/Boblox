// challenge.h — the native's 0x9B anti-cheat exchange over RakNet.
//
// The RCC sends the challenge as one application message (2 fragments on the
// wire, 1.9-2.0 KB):
//
//   [9b][u1 u32][u2 u32][len u32][blob]
//
// and the client answers 9 bytes, reliable-ordered on the same session:
//
//   [9b][u2 u32][answer u32]
//
// `answer` is computed locally from the blob (solve/solve.h: RSB1 xor +
// xxhash32 + zstd -> opcode standardization -> Luau VM in the production
// sandbox), the same solver the RbxTransport session uses — the native's own
// answers are matched by tests/unit/regress, so no oracle is involved.
//
// RakNet-specific part is only the transport: the answer goes out as the next
// ordered message (the native's ordering index is 5, right after 0x8f).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace rbxclient {

class RaknetClient;

// True when `p` is the complete server form of the challenge (13-byte header
// plus a blob that fits). The client form ([9b][u2][answer], 9 bytes) is not.
bool isChallenge(const uint8_t* p, size_t n);

// Solve `p` for `job_id` and send the answer. Returns false when the solve
// fails (nothing is sent: a wrong answer gets the session kicked, a missing
// one just leaves the join gated).
bool answerChallenge(RaknetClient& client, const uint8_t* p, size_t n,
                     const std::string& job_id,
                     const std::function<void(const std::string&)>& log = {});

} // namespace rbxclient
