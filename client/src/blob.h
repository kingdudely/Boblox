// blob.h — 0x9B challenge message parsing and blob decode.
//
// Reversed from libroblox.so sub_274FA64 (challenge script loader); the
// Python reference is tools/challenge_blob.py — this is a faithful port.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rbxclient {

struct Challenge {
    uint32_t u1 = 0;
    uint32_t u2 = 0;
    std::vector<uint8_t> blob;
};

// [9b][u1 u32][u2 u32][len u32][blob] -> Challenge. Returns false + *err.
bool extractChallenge(const uint8_t* msg, size_t len, Challenge& out, std::string* err);

// Scan a stream of compactVarint-framed messages (the chan1_rx walk in
// session.cpp) for the first 0x9B message with a complete 13-byte header.
// Returns true + the message offset/length on hit, false when no complete
// challenge frame is present yet (incomplete prefix/body included — the
// caller waits for more data). Pure: no I/O, no solving.
bool findChallengeFrame(const uint8_t* d, size_t n, size_t* msg_off, size_t* msg_len);

// RSB1 xor-decrypt + xxhash32(seed=0x2A) verify + optional zstd -> wire
// program. Returns false + *err on checksum/format failure.
bool decodeBlob(const uint8_t* blob, size_t len, std::vector<uint8_t>& wire, std::string* err);

} // namespace rbxclient
