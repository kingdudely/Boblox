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
//   02 <len u8> <bytes>      string
//   09 <u8 0|1>              boolean
//   0c <f64 LE>              number
//   16 <f32 x y z>           Vector3
//   1c <leb lo> <u32 id>     Instance reference
//   (argc 0 = no arguments)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rbxclient {
namespace batch {

// One event call's argument, in the wire's own tag space.
struct Arg {
    enum class Kind { Nil, String, Bool, Number, Vector3, Instance } kind = Kind::Nil;
    uint32_t ref_lo = 1; // instance refs: 1 = replicated, higher = client-local
    uint32_t ref_id = 0;
    std::string str;
    double number = 0.0;
    bool boolean = false;
    float v[3] = {0, 0, 0};
};

Arg arg_string(const std::string& s);
Arg arg_bool(bool b);
Arg arg_number(double v);
Arg arg_vector3(float x, float y, float z);
Arg arg_instance(uint32_t id, uint32_t lo = 1);

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

// 83 08 — RequestCharacter: ask the RCC to replicate our own character.
std::vector<uint8_t> request_character(uint32_t player_ref, uint32_t flags = 0);

// 83 03 — one float property write (used for MaxSimulationRadius).
std::vector<uint8_t> value_batch(uint32_t ref, uint16_t prop, float v);

// 83 03 — one instance-reference property write (tag 28): value is
// [varint scope=1][u32 target ref]. Used for Player.ReplicationFocus.
std::vector<uint8_t> ref_batch(uint32_t ref, uint16_t prop, uint32_t target);

// A1 — one ISR instance-state record carrying one ref property. This is the
// client->server state path (the native receives it through
// InstanceStateReceiver::readReceivedPacket). Header is the 13-byte form with
// the optional timestamp, then one object with one dirty member.
//   a1 | tick u32 | objects=1 u32 | timestamp u32
//      | u16 item_count=1 | u8 lod=0 | u8 delta=0
//      | [ref lo=1][u32 object_ref] | u16 class | u16 member_count
//      | mask words (ceil(member_count/64) x u64) | u16 body_len
//      | [ref lo=1][u32 target_ref]
std::vector<uint8_t> isr_ref_batch(uint32_t tick, uint32_t object_ref,
                                   uint16_t cls, uint16_t member_count,
                                   uint16_t member_index, uint32_t target_ref,
                                   bool include_timestamp = true);

// TOUCH report (client-authored collision): id, both refs, phase, terminator.
std::vector<uint8_t> touch_batch(uint8_t id, uint32_t a, uint32_t b,
                                 uint8_t phase);

// 83 0a — ownership claim for one instance. Captured shape (15 B):
//   83 0a 01 <ref u32> b8 15 01 <clock u32> 00
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

// 83 03 property write with a CFrame value: three floats of position, a kind
// byte (0 = explicit 3x3 matrix), then the nine matrix floats.
std::vector<uint8_t> cframe_batch(uint32_t ref, uint16_t prop, float x, float y,
                                  float z);

// 83 06 — physics claim/state batch (template from a reference capture).
//  83 06 01 <ref u32> 00000000 <ref u32> <clock u32> <clock u32> <clock u32>
//         <u32> <u32> <s32> 00000000 ffffffff 01 00
// The three clocks are the same millisecond domain as 0x83/05's clock_a.
std::vector<uint8_t> physics06_batch(uint32_t ref);

// Lower-level pieces, for probes and tests.
void put_ref(std::vector<uint8_t>* out, uint32_t ref);
void put_arg(std::vector<uint8_t>* out, const Arg& a);

} // namespace batch
} // namespace rbxclient
