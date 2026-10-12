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
//   0x83 deltas chain items [83][type][body]* [00] (see walk83Items):
//        0x16 batches [16][flags][u32 comp][u32 decomp][zstd][trailer]
//        (lengths assert exactly; flags=01 trailer is the 00 end; other
//        flags carry wider trailers — NetAsset shapes, deferred);
//        0x07 = event invocation [entity ref][u16 event][typed params]
//        (see parseEventItem); 0x02 = streaming spawn (see parseSpawnItem);
//        0x23 = clock tick [u32] (see walk83Items); 0x0d/0x1b/0x20/... are
//        deferred-type items with their own handlers (walk stops pinned).
//
// No I/O, no state: every function is fuzz-friendly. Callers handle the
// DataModel apply; this header only turns bytes into typed events.
#pragma once

#include <array>
#include <cstdint>
#include <map>
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
//     56:bulk blob [varint comp][varint decomp][comp bytes] (sub_340989C)
//     33:asset id — plain string [varint][bytes] in snapshot-shape reads,
//       prefix-compressed [u8 p]([varint id] | [varint][bytes]) in
//       event/loop-A reads (sub_3409332; preshared table never on wire)
//   Tags 29-32 (Tuple/ValueArray/ValueTable/ValueMap) and 57 fail loudly:
//   29-32 need compiled-in element types (not on the wire), 57 is never
//   sent server->client. Tags 4, 5, 45 have no native case at all
//   (default = "unsupported format" throw). Every other tag 0-70 parses
//   byte-exact.
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
//   Tags 4, 5, 45 have no native case (all fail loudly); 57 fails loudly
//   (client->server-only frustum). None of 4, 5, 45, 57 occur in the
//   Crossroads snapshot; tuple-family tags 29-32 use the same
//   self-describing wire shape in snapshot values as in event params.
struct SchemaMember {
    std::string name;
    uint8_t tag = 0;
    uint64_t extra = 0; // tag 60: varint; tag 7: u16; else 0
    bool has_extra = false;
};
// One RemoteEvent/RemoteFunction-style event descriptor: the wire carries
// [varint len][name bytes][u8 nparams][nparams x 3B descriptors]. The 3B
// payload is carried verbatim (native expands it to 16B param entries in
// sub_3425B7C; byte[1]==3 is rejected as "Invalid variant type").
struct SchemaEvent {
    std::string name;
    std::vector<std::array<uint8_t, 3>> params;
};
struct SchemaClass {
    std::string name;
    std::vector<SchemaMember> members;
    std::vector<SchemaEvent> events;
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

// ---- Live streaming items (0x83 chains after the snapshot) ----------------
// Provenance: IDA ground truth on libroblox.so + live Crossroads captures
// (see FINDINGS.md "Recv items"). A 0x83 message chains items back to
// back: [83][type][body]* [00] — every item handler leaves the cursor at
// the next type byte (or the 00 end marker); the message is also
// length-framed, so a missing end marker at EOF is fine, never an error.
// Any 32B3BA4-level type can chain (01/02/03/04/05/06/07/0d/10/13/15/
// 1b/20/23/29 via the inner/outer dispatchers); only those parse here,
// the rest stop the walk with the stop point pinned (see Walk83).
// Types 0x70/0xA3/0x97 and inner 8-12/14/16-18/20/22 throw natively
// ("Error while deserializing item") — a stop there is faithful
// replication (the native drops the packet), never a silent gap.
// Batches (0x16) never chain with other items (all 37 frozen messages are pure).
//
//   item 0x23 (Type-35, sub_3511D26): [23][u32 clock] (+4 monotonic live;
//     2034/2034 7B ticks read clock then a 00 byte). The clock is the only
//     field the native reads; anything after it is the next item.
//   item 0x02 (streaming spawn, sub_34956D2):
//     [ref lo+u32][u16 class][pad u8 iff replicator flag15562&1==0]
//     [prop loop][prop loop][parent ref lo+u32]
//     Both loops are the same ([u8 delta][value])* [0xFF] grammar as
//     snapshot records (sub_3493C2E modes (2,1) then (1,0); the mode only
//     selects value-reader variants/permission checks, never the framing).
//     Deltas are absolute member indexes, exactly like the snapshot.
//     Live sessions always show flag15562=0xb7 (no pad byte); the pad path
//     is IDA-only, so has_pad is an explicit parameter (default false).
//   item 0x07 (event invocation, sub_34822C6):
//     [07][entity ref lo+u32][u16 event][params...]
//     The event index is GLOBAL over the schema's concatenated per-class
//     event lists (schema order; total 6937 == ev_bound on the frozen
//     join — pinned by unit test). Param shapes come from the event's 3B
//     descriptors; descriptor byte[0] is the value tag in the same
//     numbering as snapshot member tags (native switches on it in
//     sub_340A3E4: 0x1c=ref, 0x02=string, 0x07=enum, 0x2c=int64 ...).
//     Tags 29-32 (Tuple/ValueArray/ValueTable/ValueMap) are self-describing:
//     [varint N][N x [u8 tag]([u16 index] iff tag==7)[payload]] (element
//     payloads use the same tag numbering; sub_340887C). Tuple (29) is
//     top-level-only (nested Tuple throws natively); ValueTable (31) /
//     ValueMap (32) entries are [string key][element]. Tag 57 fails loudly:
//     its CFrame-like shape is flag-variable (sub_3615946) and it is
//     client->server-only (Player.SendCameraFrustum), never observed
//     server->client.
//     Tag 56 (compressed bulk blob) parses: [varint comp][varint decomp]
//     [comp bytes] (sub_340989C).
struct SpawnRecord {
    uint64_t lo = 0;
    uint32_t id = 0; // (uint32_t)-1 when lo == 0 (sentinel)
    uint16_t cls = 0;
    std::vector<JoinProp> props_a; // first loop (mode (2,1))
    std::vector<JoinProp> props_b; // second loop (mode (1,0))
    uint64_t plo = 0;
    int64_t parent = -1; // -1 when plo == 0 (graft to root)
};
struct EventArg {
    uint8_t tag = 0;
    uint16_t extra = 0; // descriptor bytes [1..2] (enum table for tag 7)
    std::vector<uint8_t> value; // raw value bytes (typed at apply)
    // Tuple-family (tags 29-32, sub_340A3E4 cases 0x1D-0x20): elements are
    // self-describing on the wire ([varint N][N x [u8 tag]([u16] iff
    // tag==7)[payload]]), so a pure parser bounds them with no table.
    // 29 Tuple (top-level only) / 30 ValueArray: positional elements.
    // 31 ValueTable / 32 ValueMap: keys[i] is the string key of elements[i].
    std::vector<EventArg> elements;
    std::vector<std::string> keys; // tags 31/32 only, same length as elements
};
struct EventItem {
    uint64_t lo = 0;
    uint32_t entity = 0; // (uint32_t)-1 when lo == 0 (sentinel)
    uint16_t event = 0;  // global index over concatenated class events
    size_t cls_idx = 0;  // owning class (schema order)
    size_t ev_idx = 0;   // index within the owning class
    std::vector<EventArg> args;
};
// One spawn item at [d, d+n). *consumed = bytes used.
bool parseSpawnItem(const uint8_t* d, size_t n, const JoinSchema& schema,
                    bool has_pad, SpawnRecord& out, size_t* consumed,
                    std::string* err);
// One event invocation at [d, d+n). *consumed = bytes used.
bool parseEventItem(const uint8_t* d, size_t n, const JoinSchema& schema,
                    EventItem& out, size_t* consumed, std::string* err);

// One 0x0d entry frame (sub_353C434 head + N record entries).
struct DeltaEntries {
    uint32_t n = 0;
    std::vector<JoinRecord> records; // == n (the native loop runs N
                                     // times; trailing bytes are unread)
    std::vector<uint8_t> trail; // buffer bytes after the Nth record
};
// One 0x0d element (sub_353ECD2): [u8 flags][payload iff
// (flags&0xF7)==0: u8 flag + 3xu32 (flag!=0) or 3xu8 (flag==0)
// (sub_3617A08: 69F914C u32s / 69F948A u8s)]. Continue iff flags&8.
struct Elem0d {
    uint8_t flags = 0;
    std::vector<uint8_t> payload; // raw (empty unless (flags&0xF7)==0)
};
struct Item0d {
    DeltaEntries entries;
    std::vector<Elem0d> elems;
};
// [0d][u32 N][u32 comp][u32 decomp][zstd -> N records][elements].
// N==0 skips the frame fields entirely (native LABEL_112 path).
bool parseItem0d(const uint8_t* d, size_t n, const JoinSchema& schema,
                 Item0d& out, size_t* consumed, std::string* err);
// One 0x01 item (sub_3481460): [01][entity ref] (null refs are dropped
// natively; carried here with entity == (uint32_t)-1).
struct Item01 {
    uint64_t lo = 0;
    uint32_t entity = 0;
};
bool parseItem01(const uint8_t* d, size_t n, Item01& out, size_t* consumed,
                 std::string* err);
// One 0x1b item (sub_34F07AE): [1b][u32 N][N entity refs] (null refs
// are skipped natively; carried here with entity == (uint32_t)-1).
struct Item1b {
    std::vector<Item01> refs;
};
bool parseItem1b(const uint8_t* d, size_t n, Item1b& out, size_t* consumed,
                 std::string* err);
// One 0x05 timing item (sub_3486B9E, items 5/6): [u8 flags][u64 t]
// [u32 a][u32 b][u32 c][u8 tail] + state-conditional fields. The two
// state bits live in replicator state+15560 (not on the wire): bit
// 0x20000000000 gates a u32 when flags&4, bit 0x8000000000000 gates a u32
// when flags&0x10. The frozen session shows flags=0x10 with the second
// bit set (26B items); both bits are explicit parameters. flags&8 reads
// via sub_57D3718 (unmapped) and fails loudly.
struct Item05 {
    uint8_t flags = 0;
    uint64_t t = 0;
    uint32_t a = 0, b = 0, c = 0;
    uint8_t tail = 0;
    bool has_x = false;
    uint32_t x = 0;
    bool has_y = false;
    uint32_t y = 0;
};
bool parseItem05(const uint8_t* d, size_t n, bool st_bit11, bool st_bit15,
                 Item05& out, size_t* consumed, std::string* err);
// One 0x03 property update (sub_3404858, sole caller 3439A9E):
// [ref lo+u32][u16 prop id][value]. The prop id indexes the replicator's
// runtime prop table (32B entries, 10146 of them live == prop_bound+1;
// value decoded by sub_340A3E4 from the entry descriptor). One triple per
// 0x03 item: every triple repeats the explicit 03 byte (the handler is
// straight-line, no intra-item loop). PropTagTable carries the prop-id ->
// tag mapping the pure parser reads; see defaultPropTags for provenance
// (exact-close solving; the native descriptor-field layout is under
// active verification — see FINDINGS "Recv v3").
struct PropTriple {
    uint64_t lo = 0;
    uint32_t id = 0; // (uint32_t)-1 when lo == 0 (sentinel)
    uint16_t prop = 0;
    uint8_t tag = 0; // resolved via the table
    std::vector<uint8_t> value; // raw value bytes
};
struct PropTagTable {
    std::map<uint16_t, uint8_t> tag; // prop id -> value tag (0-70)
    // Content/asset (tags 33/61) prefix shape in 0x03 values (see
    // defaultPropTags provenance); absent_prop == plain snapshot shape.
    std::map<uint16_t, bool> prefix;
};
bool parsePropTriple(const uint8_t* d, size_t n, const PropTagTable& table,
                     PropTriple& out, size_t* consumed, std::string* err);
// One flags=01 batch entry (sub_342550C): [16B key][1B marker][varint
// len][len bytes] (marker 0 = plain, 1 = new-string-with-sender-id;
// anything else throws natively). Entries concatenate until the inner
// buffer is exhausted (no count, no terminator).
struct SharedString {
    uint8_t key[16] = {0};
    uint8_t marker = 0;
    std::vector<uint8_t> value;
};
bool parseSharedStrings(const uint8_t* d, size_t n,
                        std::vector<SharedString>& out, std::string* err);
// Post-spawn script-data bulk (content-addressed LocalScript bytecode):
// [8B key suffix][00][varint len][len payload][anchor ref lo+u32]. The
// full 16B key is the spawn's own Source-hash value: the native reads the
// key's first half as the spawn's parent ref (varint+u32,
// deterministically — exotic scopes miss the deferred-parent lookup and
// are tolerated), so the bulk starts mid-key with no introducing type
// byte. The anchor (here lo=01, id=11928 = the hosting Folder) re-anchors
// the script after its deferred parent; the chain's 00 end follows. The
// walker only attempts this shape as a fallback at an otherwise-unknown
// chain byte (never in place of a known item); the 00 marker + exact
// length make false positives ~impossible, and any downstream failure
// stays loud.
struct BulkItem {
    uint8_t suffix[8] = {0};
    std::vector<uint8_t> payload;
    uint64_t anchor_lo = 0;
    int64_t anchor = -1; // -1 when anchor_lo == 0
};
bool parseBulkItem(const uint8_t* d, size_t n, BulkItem& out, size_t* consumed,
                   std::string* err);
// One 0x04 item (sub_3485DD4): [04][u32 v] (4 bytes, stored verbatim).
struct Item04 {
    uint32_t v = 0;
};
bool parseItem04(const uint8_t* d, size_t n, Item04& out, size_t* consumed,
                 std::string* err);
// One 336800E guid (entity/blob id): [varint V]([u32 hi] iff V != 0 and
// V != 0xFFFFFFFF). Both 0 and 0xFFFFFFFF are null (no hi bytes).
struct GuidRef {
    uint64_t lo = 0;
    int64_t id = -1; // -1 when null
};
// One 0x13 item (sub_34908E4): [13][guid][guid] (two entity refs).
struct Item13 {
    GuidRef a;
    GuidRef b;
};
bool parseItem13(const uint8_t* d, size_t n, Item13& out, size_t* consumed,
                 std::string* err);
// One 0x15 item (sub_3475046): [15][guid][string][string] (entity ref +
// two varint-length-prefixed strings).
struct Item15 {
    GuidRef ref;
    std::string s1;
    std::string s2;
};
bool parseItem15(const uint8_t* d, size_t n, Item15& out, size_t* consumed,
                 std::string* err);
// One 0x29 item (sub_3579BFE): [29][u32 N][N x [guid][u8 flag]]
// (ref-list with per-entry bool; N bounded by remaining bytes).
struct Item29Entry {
    GuidRef ref;
    uint8_t flag = 0;
};
struct Item29 {
    std::vector<Item29Entry> entries;
};
bool parseItem29(const uint8_t* d, size_t n, Item29& out, size_t* consumed,
                 std::string* err);
// One flags=05 NetAsset batch entry (sub_348C62C): [NetAssetId = tag-60
// shape: varint schema (+ [8B id][varint v] iff schema != 0xFFFF)]
// [varint payLen][payLen payload bytes]. Entries concatenate until the
// batch buffer is exhausted (no count, no terminator, no decompression;
// the native additionally breaks on timeslice expiry, which has no wire
// effect offline). NEVER observed live (0 in 12k+ messages); grammar is
// IDA-only until a first sighting pins it — HYPOTHESIS on the batch
// body offset (bytes after [83][16][05] header).
struct NetAssetEntry {
    std::vector<uint8_t> id_raw; // raw NetAssetId bytes (tag-60 shape)
    std::vector<uint8_t> payload;
};
bool parseNetAssetBatch(const uint8_t* d, size_t n,
                        std::vector<NetAssetEntry>& out, std::string* err);
// The 0x03 prop-id -> tag map: the full native number map (10146
// entries, register-dumped) with solver-proven wire-shape overrides
// baked in (see tools/gen_proptable.py + proptable.inc). Unobserved
// props read their native snapshot shape (best prior; skew drops loudly).
// Solver proof per observed prop (two sessions, exact closes; every
// native-number alternative parses strictly fewer messages):
// 248:68 (unique) | 3796:61 Content 77B emote table | 4989:18 (tied 2B)
// 5018:61 Content 35B touch text | 5469:18 u16 const 257 (tied 2B)
// 5868:3 empty | 5918:46 16B | 10145:25 6B. Native numbers for these:
// {248:61, 3796:2, 4989:34, 5018:2, 5469:34, 5868:28, 5918:46, 10145:28}
// (server-0.742 vs traced-client-2.739 skew; see FINDINGS Recv v3). All
// dense-range, never schema-dense — the table is mapped, never computed.
PropTagTable defaultPropTags();
// One 0x10 item (sub_34899B8): [10][u32 v] (5 bytes; only v==13 arms
// extra timestamp/flag state, otherwise the value is stored).
struct Item10 {
    uint32_t v = 0;
};
bool parseItem10(const uint8_t* d, size_t n, Item10& out, size_t* consumed,
                 std::string* err);
// One 0x20 item (sub_35C3A7E): [20][u32 a][u32 len][len bytes] (9+len
// bytes; the blob is carried verbatim, the rest of the chain follows).
struct Item20 {
    uint32_t a = 0;
    std::vector<uint8_t> blob;
};
bool parseItem20(const uint8_t* d, size_t n, Item20& out, size_t* consumed,
                 std::string* err);
struct ChainItem {
    uint8_t type = 0; // 0x01/0x03/0x04/0x05/0x06/0x02/0x07/0x0d/0x10/0x13/
                      // 0x15/0x1b/0x20/0x23/0x29, or 0x78 pseudo-type for a
                      // typeless script-data bulk
    Item01 ref01;     // type 0x01
    PropTriple prop03; // type 0x03
    Item04 v04;       // type 0x04
    Item05 timing05;  // type 0x05/0x06 (same handler sub_3486B9E)
    SpawnRecord spawn; // type 0x02
    EventItem event;   // type 0x07
    Item0d delta0d;    // type 0x0d
    Item10 v10;        // type 0x10
    Item13 pair13;     // type 0x13
    Item15 info15;     // type 0x15
    Item1b refs1b;     // type 0x1b
    Item20 blob20;     // type 0x20
    Item29 list29;     // type 0x29
    BulkItem bulk;     // pseudo-type 0x78 (no wire type byte)
    uint32_t clock = 0; // type 0x23
};
// Walk result: items parsed before the stop point. complete=true iff the
// walk hit the 00 end marker (or clean EOF). complete=false means the walk
// stopped at a deferred-type item (0x03 prop updates need the runtime
// prop-id table; other bytes need their own native handler); stop_type/
// stop_off pin exactly where, so coverage is measured, never silent.
// False + *err only on structural violations (bad header, truncation,
// item parse failure).
struct Walk83 {
    std::vector<ChainItem> items;
    bool complete = false;
    uint8_t stop_type = 0;
    size_t stop_off = 0;
    size_t end_off = 0; // one past the 00 end marker, or EOF after the
                        // last item (== message length on exact close)
};
bool walk83Items(const uint8_t* body, size_t len, const JoinSchema& schema,
                 Walk83& out, std::string* err);
// Same, but 0x03 triples resolve through *props (unknown prop id fails
// the walk). nullptr restores the stop-pinned behavior.
bool walk83ItemsEx(const uint8_t* body, size_t len, const JoinSchema& schema,
                   const PropTagTable* props, Walk83& out, std::string* err);

} // namespace rbxclient
