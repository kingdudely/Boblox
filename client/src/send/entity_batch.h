// entity_batch.h — client -> RCC replication batches (0x83), byte-exact.
//
// Shapes read from a labeled native capture (2026-11-05, build 2.743.0.1136):
// a script sent through the executor fired one RemoteEvent with a known string
// and called Humanoid:Move, and both appeared in plaintext.
//
//   event batch : 83 07  01 <ref u32 LE> <event u16 LE> <argc u8> <args...> 00
//   property    : 83 03  01 <ref u32 LE> <prop u16 LE> <value bytes>        00
//
// Arg tags (all observed):
//   02 <len varint> <bytes>  string
//   09 <u8 0|1>              boolean
//   0a <zigzag varint>       int
//   0c <f64 LE>              number
//   16 <f32 x y z>           Vector3
//   1c <leb lo> <u32 id>     Instance reference
//   1e <varint N> <values>   ValueArray (self-describing elements)
//   2c <zigzag varint>       int64
//   (argc 0 = no arguments)
//
// The 1013-byte native frame in run/parity/r7e1/native/dgram_234.bin decodes
// as five ValueArrays, each `[bool, text, path, locale, filtered text,
// int 0, int 0, int64 timestamp, int 142]`, so the text stream is a normal
// 0x83/0x07 RemoteEvent invocation with ValueArray arguments.
#pragma once

#include "solve/replicate.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rbx {
struct CFrame;
}

namespace rbxclient {
namespace batch {

// One event call's argument, in the wire's own tag space.
struct Arg {
    enum class Kind {
        Nil,
        String,
        Bool,
        Number,
        Vector3,
        Instance,
        Table,
        Int,
        Int64
    } kind = Kind::Nil;
    uint32_t ref_lo = 1; // instance refs: 1 = replicated, higher = client-local
    uint32_t ref_id = 0;
    std::string str;
    double number = 0.0;
    bool boolean = false;
    float v[3] = {0, 0, 0};
    std::vector<Arg> table; // ValueArray elements, in order
    int64_t integer = 0;    // Int (tag 0x0a) / Int64 (tag 0x2c)
};

Arg arg_string(const std::string& s);
Arg arg_bool(bool b);
Arg arg_number(double v);
Arg arg_vector3(float x, float y, float z);
Arg arg_instance(uint32_t id, uint32_t lo = 1);
Arg arg_table(std::vector<Arg> items);
Arg arg_int(int64_t v);
Arg arg_int64(int64_t v);

// One filtered-text record from the native GUI stream. The five loading-screen
// records in dgram_234 each carry this exact 9-element ValueArray. `timestamp`
// is the captured int64 (milliseconds); `result` is the captured trailing int
// (142 in every loading-screen record).
struct TextRecord {
    std::string text;
    std::string path;
    std::string locale;
    std::string filtered_text;
    int64_t timestamp = 0;
    int32_t result = 142;
};

// 83 07 01 <ref> <event> <argc> <N x ValueArray> 00.
// `event` is 0x11bb (the native RemoteEvent invocation id). The caller must
// supply the live RemoteEvent's ref; a ref from another session is rejected.
std::vector<uint8_t> filtered_text_batch(
    uint32_t ref, const std::vector<TextRecord>& records,
    uint16_t event_id = 0x11bb);

// 83 07 ... 00 — a RemoteEvent's OnServerEvent (or any class event) call.
std::vector<uint8_t> event_batch(uint32_t ref, uint16_t event_id,
                                 const std::vector<Arg>& args);

// 83 03 ... 00 — a property write. `prop` is the schema property id; a
// Vector3 value is the local Humanoid's move direction (prop 3848 observed).
std::vector<uint8_t> vector3_batch(uint32_t ref, uint16_t prop, float x,
                                   float y, float z);

// 83 04 — client-ready marker (12 B): item 04 <clock ms>, item 23 <tick>, end.
// The two are different domains: the 04 field is the millisecond clock, the 23
// item is the 240 Hz tick (captured reference frame: 04 d6 09 00 00 | 23 62 00
// 00 00, i.e. 2518 ms and tick 98 — not the same number twice).
std::vector<uint8_t> ready_batch(uint32_t clock, uint32_t tick);

// 83 04 — ready marker with the first transform chained inside, matching the
// reference: 04 <clock> 05 16 <transform> 23 <tick> 00.
std::vector<uint8_t> ready_with_transform(uint32_t clock, uint32_t tick,
                                          float x, float y, float z,
                                          uint32_t flags, float f);

// 83 23 <tick> 00 — the standalone clock frame. The native streams this at
// ~14/s for the whole session (measured 2026-11-08, place 1818, 813 s: 11445
// of 12044 0x83/23 frames are exactly this 7-byte shape, gap p50 16 ms, and
// the tick rises 304 -> 193804 = 239.1/s). It carries no instance state.
std::vector<uint8_t> clock_batch(uint32_t tick);

// The 23 item's clock domain: 240 Hz ticks, not milliseconds. Measured on the
// same capture (239.1/s fit) and cross-checked in the 0xA1 packet header, which
// carries the same value. Derived from net_clock_ms() so it shares one origin
// (and the same rebase at world-live) with every other clock this client sends.
uint32_t net_tick_240();

// 83 02 — client-authored spawn (upstream replication of an instance we own).
std::vector<uint8_t> client_spawn(uint32_t ref, uint16_t cls, uint32_t parent);

// 83 1c — native client-replication envelope. Every observed native frame is
//     [83][1c][flags=0][i32 clock][u16 channel][inner chain][00]
// channel 6 carries the ordinary client chain, 0x7fff the client-owned physics
// chain. `inner` must be complete chain items WITHOUT the leading 83 or the
// trailing 00; wrap_client_batch() accepts a normal batch and strips them.
std::vector<uint8_t> client_replication(int32_t clock, uint16_t channel,
                                        const std::vector<uint8_t>& inner);
std::vector<uint8_t> wrap_client_batch(const std::vector<uint8_t>& raw_batch,
                                       int32_t clock, uint16_t channel = 6);

// 83 0e — region removal. One record per region:
//   [0e][u8 large][3x i8 | 3x i32][u32 revision][u32 count][u8 0]
//     [count x ([u8 scope]([u32 id] iff scope != 0))]
// The native only ever sends the uncompressed list, so the builder emits the
// exact observed branch; a zero-scope reference is a null GUID.
std::vector<uint8_t> region_removal_batch(
    const std::vector<rbxclient::RegionRemovalRecord>& regions);

// 83 08 — RequestCharacter: ask the RCC to replicate our own character.
std::vector<uint8_t> request_character(uint32_t player_ref, uint32_t flags = 0);

// A1 — client -> server instance state (the InstanceStateSender).
//
// Re-derived 2026-11-09 from one 700 s native session (149 frames, four
// shapes, *every* frame closes exactly under this grammar):
//
//   envelope: a1 <tick u32> <records u32> <ack u32> [<flags u32>]
//             the flags word exists only when records > 0, so an idle frame
//             is 13 bytes and a carrying frame is 17 + body.
//   record:   [varint lo][u32 id iff lo != 0] <class u16> <member_count u16>
//             <mask words, ceil(member_count/64) x u64 LE>
//             <body_len u16> <values>
//
// `values` is the selected members' own schema tags in ascending order — the
// same value grammar 0xA1 *reception* already decodes, which is why a writer
// needs no new encoding: the caller passes the encoded value bytes.
// The capture's shapes (all reproduced byte-for-byte in unit_batch):
//   13 B  no records                       (119 of 149 frames)
//   44 B  Player.CameraViewportSize   tag 21 Vector2  (2 floats)
//   48 B  Player member,              tag 22 Vector3  (3 floats)
//   37 B  Humanoid member,            tag 7  enum     (1 byte, 0x0f)
struct IsrRecord {
    uint32_t ref = 0;          // instance replication id
    uint16_t cls = 0;          // class network id (schema index)
    uint16_t member_count = 0; // the writer's member bound for the class
    uint16_t member_index = 0; // selected member (mask bit)
    std::vector<uint8_t> value;
};

// a1 <tick> <records=0> <ack> — the native's ~0.17/s keepalive shape.
std::vector<uint8_t> isr_empty(uint32_t tick, uint32_t ack);

// Envelope with records; `flags` is the captured 1 (it only exists when
// records > 0, which is why it is a parameter of this overload and not of
// isr_empty).
std::vector<uint8_t> isr_batch(uint32_t tick, uint32_t ack,
                               const std::vector<IsrRecord>& records,
                               uint32_t flags = 1);

// TOUCH report (client-authored collision): id, both refs, phase, terminator.
std::vector<uint8_t> touch_batch(uint8_t id, uint32_t a, uint32_t b,
                                 uint8_t phase);

// 83 0a — per-instance acknowledgement/ownership record.
//
// Native grammar (same-job place-1818 capture, 776 frames):
//   83 0a (0a 01 <ref u32> <prop u16> <has-value u8>
//          [<value u8> iff has-value] <clock u32>)*
//         [23 <tick u32>] 00
// has-value 1 records are 14 B (the value byte is 00/0a/10 in the capture);
// has-value 0 records are 13 B.  The 23 clock item is optional.  The final 00
// is the chain terminator the previous builder omitted (ours was 14 B where the
// native's one-record frames are 16 B).
struct Ack0a {
    uint32_t ref = 0;
    uint16_t prop = 0x1607; // observed property id
    bool has_value = true;
    uint8_t value = 0;
    uint32_t clock = 0;
};

std::vector<uint8_t> ack0a_batch(const std::vector<Ack0a>& records,
                                 uint32_t tick = 0);

// One has-value=0 record with the historical property id, kept for callers
// that only need an ownership claim.  The native's short form is 15 B.
std::vector<uint8_t> interest_batch(uint32_t ref, uint32_t clock);

// 83 05 — client physics upload (45 B, captured):
//   83 05 16 <clock u32> 0 <clock u32> 0 <x y z f32> <flags u32> <fl u8>
//            <clock u32> <f32> 00
// The position is at offset 19, not 15: the reference carries two zero words
// around its clock pair. Re-derived from the 2026-11-08 capture; see the
// comment in the .cpp for the full frame and what each field was measured to be.
std::vector<uint8_t> physics_batch_clocked(uint32_t clock_a, uint32_t clock_b,
                                           uint32_t clock_c, float x, float y,
                                           float z, uint32_t flags, float f);

// The clock domain those fields live in: ~1000 ticks/s. Measured: the
// reference's clock_a/c rise 2730 -> 7298 over 4.6 s (~993/s), and its first
// upload already reads 2730, i.e. milliseconds of session uptime. Every builder
// that emits a clock reads this one function.
uint32_t net_clock_ms();

// Restart that clock at a chosen base. The runtime rebases to 2730 when the
// world goes live; before that net_clock_ms() counts from its first use.
void rebase_net_clock(uint32_t base);

// 83 03 property write with a CFrame value: three floats of position, then a
// rotation kind byte.  Kind 1..36 is a pair of signed axes from the native's
// table; kind 0 carries a quantized quaternion (u16 + u32).
std::vector<uint8_t> cframe_batch(uint32_t ref, uint16_t prop, float x, float y,
                                  float z);

// Full tag-27 CFrame write.  Axis-aligned rotations use the native's compact
// rotation table; everything else uses the quantized quaternion form.  The
// position-only overload above is kept for the identity rotation.
std::vector<uint8_t> cframe_batch(uint32_t ref, uint16_t prop,
                                  const rbx::CFrame& cframe);

// One 0x83/03 frame carrying several owned-body poses.  Native clients bundle
// the bodies they own instead of emitting one reliable message per body; the
// pointer keeps CFrame out of this header's dependencies and is only read
// during the call.
struct CFrameWrite {
    uint32_t ref = 0;
    uint16_t prop = 0;
    const rbx::CFrame* cframe = nullptr;
};
std::vector<uint8_t> cframe_batch(const std::vector<CFrameWrite>& writes);

// 83 06 — physics claim/state batch (template from a reference capture).
//  83 06 01 <ref u32> 00000000 <ref u32> <clock u32> <clock u32> <clock u32>
//         <u32> <u32> <s32> 00000000 ffffffff 01 00
// The three clocks are the same millisecond domain as 0x83/05's clock_a.
std::vector<uint8_t> physics06_batch(uint32_t ref);

// 0x51..0x53 / 0x54..0x56 — the native's 5-second clock-sync burst.
//   9B  [id][u32 0][u24 clock]                     (0x51..0x53)
//   25B [id][u64 system_ms][u64 session_ms][u16 6][u16 0x6006][u32 nonce]
// The 25-byte form is the same clock in two domains; the nonce is opaque.
std::vector<uint8_t> clock_ping9(uint8_t id, uint32_t clock_ms);
std::vector<uint8_t> clock_ping25(uint8_t id, uint64_t system_ms,
                                  uint64_t session_ms, uint32_t nonce);

// Lower-level pieces, for probes and tests.
void put_ref(std::vector<uint8_t>* out, uint32_t ref);
void put_arg(std::vector<uint8_t>* out, const Arg& a);

} // namespace batch
} // namespace rbxclient
