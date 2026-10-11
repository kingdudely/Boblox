// replicate.h — pure-function parsers for RbxTransport replication frames.
//
// Provenance (see FINDINGS.md "Replication capture + frame survey"): every
// layout below is verified against live captures (our rbxplay session +
// a native mocktail session on Crossroads 1818) and the native's own
// disassembly (libroblox.so, md5-verified). Open hypotheses are marked
// HYPOTHESIS and never presented as parity.
//
// Wire: compactVarint-length framing (util.h), first body byte = message ID:
//   0x97 JoinDataItemV2 snapshot: [97][a3 cf][00 00][u32 LE size @5:9]
//        [zstd @9:] running to the end of the frame.
//   0x81 peer init (byte-oriented): [ena][x][isrR][isrW][isrLeg][v53?]
//        [v58][peer:LEB128][v60?][tail...]. v53/v60 are conditional on
//        replicator config trapped in native state; BOTH observed sessions
//        match the v53-present shape (native peer 31 == its log line,
//        ours 696). Tail carried raw for later mapping.
//   0x83 deltas, second byte = inner type: 0x16 batches
//        [16][flags][u32 comp][u32 decomp][zstd][1 trailing byte] (both
//        lengths assert exactly); 0x07 = 19B records
//        [01 90 23 00 00 2f 09][owner u32 LE][22][val u32 LE][x]
//        (autocorrelation period 19; owner/val endianness + x semantics
//        are HYPOTHESIS); 0x02 = player records with (url, entity, name)
//        anchors (see parsePlayerRecs); 0x23/0x1b/others raw for now.
//
// No I/O, no state: every function is fuzz-friendly. Callers handle the
// DataModel apply; this header only turns bytes into typed events.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rbxclient {

struct ReplMessage {
    uint8_t id = 0;             // first body byte (0x83, 0x97, ...)
    std::vector<uint8_t> body;  // body INCLUDES the id byte
};

// Walk a varint-framed replication stream (e.g. saved chan1 bytes).
// Returns false + *err on the first framing violation.
bool walkReplFrames(const uint8_t* d, size_t n,
                    std::vector<ReplMessage>& out, std::string* err);

// 0x97 outer -> decompressed JoinDataItemV2 inner.
bool parseJoinOuter(const uint8_t* body, size_t len,
                    std::vector<uint8_t>& inner, std::string* err);

// 0x81 peer init. Tail (everything after the peer id) carried verbatim.
struct PeerInit {
    uint8_t ena = 0, x = 0, isr_read = 0, isr_write = 0, isr_legacy = 0;
    uint8_t v53 = 0, v58 = 0;
    uint32_t peer = 0;
    std::vector<uint8_t> tail;
};
bool parsePeerInit(const uint8_t* body, size_t len, PeerInit& out,
                   std::string* err);

// 0x83/0x16 batch -> decompressed inner (lengths asserted exactly).
bool parseDeltaBatch(const uint8_t* body, size_t len,
                     std::vector<uint8_t>& inner, std::string* err);

// 0x83/0x07 fixed records. Owner/val are u32 LE (HYPOTHESIS); x unknown.
struct PropUpdate {
    uint32_t owner = 0;
    uint32_t val = 0;
    uint8_t x = 0;
};
bool parsePropUpdates(const uint8_t* body, size_t len,
                      std::vector<PropUpdate>& out, std::string* err);

// 0x83/0x02 player record: avatar URL (u8-len prefixed), entity id
// (0x0c + u32 LE), display name (0x18 + u8 len). Records that miss the
// url+entity anchors are skipped (counted in *skipped, never fatal:
// the 0x02 vocabulary is still being mapped).
struct PlayerRec {
    std::string url;
    uint32_t entity = 0;
    std::string name; // empty when the name anchor is absent
};
bool parsePlayerRecs(const uint8_t* body, size_t len,
                     std::vector<PlayerRec>& out, size_t* skipped,
                     std::string* err);

} // namespace rbxclient
