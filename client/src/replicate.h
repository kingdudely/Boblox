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

// ---- JoinData snapshot: schema + instance records --------------------------
// Provenance: the 0x97 inner (352276B frozen Crossroads join) holds the
// class schema, two string tables, then per-blob instance records carried
// inside 0x83/0x16 batch inners. Every layout below is pinned against the
// native deserializer in libroblox.so (see FINDINGS.md "JoinData record
// codec") and cross-checked with the native's own ReplicationDataCaching
// log (record boundaries match to the byte on 7/7 Sound records, then on
// all 2911 snapshot records across 8 batches, each EOF-exact).
//
// Inner layout (byte-oriented readers: u8/LEB128/u16/u32):
//   [u8 version=3][u64 flags=15][varint enumcount][enums: 602x
//     [varint len][bytes][0x07]][varint classcount=630][varint propbound]
//     [varint evbound][classes]
//   [varint urlcount=26][urls][varint sharedcount=127][shared]
//   [opaque tail: per-class/id tables, extent to EOF]
// class = [string name][varint membercount][members][u16 flags]
//         [varint eventcount][events: [string][u8 nparams][params 3B]]
// member = [string name][u8 tag]([varint] if tag==60, [u16] if tag==7)
//   tag 60 = ref (varint), tag 7 = enum-table index (u16, 0xFFFF=none).
//   NO other tag carries a schema-level extra (writer-verified).
// record = [varint lo][u32 id][u16 class][props][varint plo][u32 parent]
//   lo/plovarint 0 = sentinel (no u32 follows; parent -1 = graft to root).
//   Observed lo/plo values are {1,4} (semantics HYPOTHESIS: snapshot/live).
//   props = ([u8 delta][value])* [0xFF]; delta indexes the class's member
//   list (range-checked only, like the native: no order enforcement).
//   value shapes keyed by member tag (tools/fuzz + unit goldens pin these):
//     2,8:string [varint len][bytes]  3:empty  6,46,47:16B  7,34,44:varint
//     9:u8  10:zigzag varint  11,16,17:u32/f32  12,48,55,62:u64/f64
//     13:8B(UDim f32+s32)  14:16B(UDim2 f32,s32,f32,s32)  15:24B(2xVec3)
//     18:u16  19,22,24,36:12B(Vec3/Color3)  20:3B  21,25,37:8B(2xf32)
//     26:CFrameA  27:CFrameB  28,69:ref [varint][u32] (0=null, no u32)
//     38:sequence [u32 N][N x 20B keypoints]  39:20B  40:16B
//     41:PhysicalProperties [u8][20B][4B?]  49:Name (u8-len/shared/extended)
//     51:optional CFrame  52:16B(8+4+4)  54:FontFace  61:Content  65:13B
//     66:3B  67:16B  68:4x varint
//   CFrameA(tag26, UNUSED in 2.739 schema — IDA-only, HYPOTHESIS):
//     [3xf32 pos][u8 kind](kind==0 ? 9xf32 matrix : table lookup).
//   CFrameB(tag27): [3xf32 pos][u8 kind](kind==0 ? [u16][u32] quantized
//     (33BEB0A quat math) : table lookup (kind-1)/6,(kind-1)%6 over the
//     six unit axes, third column = cross).
//   Name(tag49): u8 L: high-bit set = shared-table index (low 7 bits;
//     127 = varint+127 extended, HYPOTHESIS — never observed); L==127 =
//     varint-len string; else L literal bytes. The shared table is the
//     0x97's 127-string table (all 348 snapshot uses resolve to sane
//     avatar/instance names: Handle, idle, Weight, ...).
//   Content(tag61): [varint form] 0:empty 1:[varint len][bytes]
//     2:ref (only form 1 observed in snapshot; others fail loudly).
//   Tags(tag46)/bytecode(tag47) parse byte-exact (16B) but resolve to
//   nothing locally (shared/hash tables live in native state) — the
//   applier skips them with counts (documented, never guessed).
//   Unhandled tags (29-33,35,50,53,56-60,63,64,70 — none occur in the
//   Crossroads snapshot) fail loudly with class/member context.
struct SchemaMember {
    std::string name;
    uint8_t tag = 0;
    uint64_t extra = 0; // tag 60: varint; tag 7: u16; else 0
    bool has_extra = false;
};
struct SchemaClass {
    std::string name;
    std::vector<SchemaMember> members;
};
struct JoinSchema {
    uint8_t version = 0;
    uint64_t flags = 0;
    std::vector<std::string> enums;  // 602 names (items are client-known)
    uint64_t prop_bound = 0; // member prealloc bound (10145 frozen)
    uint64_t ev_bound = 0;   // event prealloc bound (6937 frozen)
    std::vector<SchemaClass> classes; // 630 (index == class network id)
    std::vector<std::string> urls;    // 26 content-URL prefixes
    std::vector<std::string> shared;  // 127 shared Name strings
    size_t tail_off = 0; // offset where the opaque tail starts
};
// Parse the 0x97 inner prefix (through the shared table). The tail extent
// is reported, not parsed (grammar HYPOTHESIS: per-class/id tables).
bool parseJoinSchema(const uint8_t* d, size_t n, JoinSchema& out,
                     std::string* err);
struct JoinProp {
    std::string name; // schema member name (wire name, e.g. "size")
    uint8_t tag = 0;
    std::vector<uint8_t> value; // raw value bytes (typed at apply)
};
struct JoinRecord {
    uint64_t lo = 0;
    uint32_t id = 0;   // (uint32_t)-1 when lo == 0 (sentinel)
    uint16_t cls = 0;  // index into JoinSchema::classes
    std::vector<JoinProp> props;
    uint64_t plo = 0;
    int64_t parent = -1; // -1 when plo == 0 (graft to root)
};
// One record at [d, d+n). *consumed = bytes used. False + *err on any
// violation (truncation, bad class/delta/tag, overrun).
bool parseJoinRecord(const uint8_t* d, size_t n, const JoinSchema& schema,
                     JoinRecord& out, size_t* consumed, std::string* err);
// Chained records to EOF (batch inners hold pure record runs). False on
// the first violation; *err carries the record index + offset.
bool parseJoinRecords(const uint8_t* d, size_t n, const JoinSchema& schema,
                      std::vector<JoinRecord>& out, std::string* err);

} // namespace rbxclient
