// entity_batch.cpp — see entity_batch.h.
#include "send/entity_batch.h"

#include <atomic>
#include <chrono>
#include <cstring>

namespace rbxclient {
namespace batch {

namespace {

int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::atomic<int64_t> g_clock_origin_ns{0};
std::atomic<uint32_t> g_clock_base{0};

void put_u16(std::vector<uint8_t>* out, uint16_t v) {
    out->push_back(uint8_t(v & 0xff));
    out->push_back(uint8_t(v >> 8));
}

void put_u32(std::vector<uint8_t>* out, uint32_t v) {
    for (int i = 0; i < 4; i++) out->push_back(uint8_t(v >> (8 * i)));
}

void put_f32(std::vector<uint8_t>* out, float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    put_u32(out, bits);
}

void put_f64(std::vector<uint8_t>* out, double d) {
    uint64_t bits = 0;
    std::memcpy(&bits, &d, 8);
    for (int i = 0; i < 8; i++) out->push_back(uint8_t(bits >> (8 * i)));
}

// Every batch opens with ID_DATA + a sub-id and closes with a zero byte.
void open_batch(std::vector<uint8_t>* out, uint8_t sub) {
    out->push_back(0x83);
    out->push_back(sub);
}

} // namespace

Arg arg_string(const std::string& s) {
    Arg a;
    a.kind = Arg::Kind::String;
    a.str = s;
    return a;
}
Arg arg_bool(bool b) {
    Arg a;
    a.kind = Arg::Kind::Bool;
    a.boolean = b;
    return a;
}
Arg arg_number(double v) {
    Arg a;
    a.kind = Arg::Kind::Number;
    a.number = v;
    return a;
}
Arg arg_instance(uint32_t id, uint32_t lo) {
    Arg a;
    a.kind = Arg::Kind::Instance;
    a.ref_id = id;
    a.ref_lo = lo;
    return a;
}

Arg arg_vector3(float x, float y, float z) {
    Arg a;
    a.kind = Arg::Kind::Vector3;
    a.v[0] = x;
    a.v[1] = y;
    a.v[2] = z;
    return a;
}

Arg arg_table(std::vector<Arg> items) {
    Arg a;
    a.kind = Arg::Kind::Table;
    a.table = std::move(items);
    return a;
}

Arg arg_int(int64_t v) {
    Arg a;
    a.kind = Arg::Kind::Int;
    a.integer = v;
    return a;
}

Arg arg_int64(int64_t v) {
    Arg a;
    a.kind = Arg::Kind::Int64;
    a.integer = v;
    return a;
}

void put_ref(std::vector<uint8_t>* out, uint32_t ref) { put_u32(out, ref); }

static void put_leb(std::vector<uint8_t>* out, uint64_t v) {
    do {
        uint8_t b = uint8_t(v & 0x7F);
        v >>= 7;
        out->push_back(uint8_t(b | (v ? 0x80 : 0)));
    } while (v);
}

static void put_zigzag(std::vector<uint8_t>* out, int64_t v) {
    put_leb(out, (uint64_t(v) << 1) ^ uint64_t(v >> 63));
}

void put_arg(std::vector<uint8_t>* out, const Arg& a) {
    switch (a.kind) {
    case Arg::Kind::String:
        out->push_back(0x02);
        // The native text records use a one-byte length for these short
        // strings, but RemoteEvent strings are varint-framed; use the same
        // varint as the engine's RemoteEvent encoder.
        put_leb(out, a.str.size());
        out->insert(out->end(), a.str.begin(), a.str.end());
        break;
    case Arg::Kind::Bool:
        out->push_back(0x09);
        out->push_back(a.boolean ? 1 : 0);
        break;
    case Arg::Kind::Number:
        out->push_back(0x0c);
        put_f64(out, a.number);
        break;
    case Arg::Kind::Vector3:
        out->push_back(0x16);
        put_f32(out, a.v[0]);
        put_f32(out, a.v[1]);
        put_f32(out, a.v[2]);
        break;
    case Arg::Kind::Instance:
        out->push_back(0x1c);
        put_leb(out, a.ref_lo);
        put_u32(out, a.ref_id);
        break;
    case Arg::Kind::Table:
        out->push_back(0x1e); // ValueArray
        put_leb(out, a.table.size());
        for (const Arg& e : a.table) {
            if (e.kind == Arg::Kind::Nil)
                out->push_back(0x00);
            else
                put_arg(out, e);
        }
        break;
    case Arg::Kind::Int:
        out->push_back(0x0a);
        // The text record's trailing field is 0x0a 8e 01 = 142, an unsigned
        // LEB128. Unlike snapshot tag 10, this event-argument int is not
        // zigzagged (the 0x2c int64 beside it is).
        put_leb(out, uint64_t(a.integer));
        break;
    case Arg::Kind::Int64:
        out->push_back(0x2c);
        put_zigzag(out, a.integer);
        break;
    case Arg::Kind::Nil:
        break; // argc already excludes it
    }
}

std::vector<uint8_t> filtered_text_batch(uint32_t ref,
                                         const std::vector<TextRecord>& records,
                                         uint16_t event_id) {
    std::vector<Arg> args;
    args.reserve(records.size());
    for (const TextRecord& r : records) {
        args.push_back(arg_table({
            arg_bool(false),
            arg_string(r.text),
            arg_string(r.path),
            arg_string(r.locale),
            arg_string(r.filtered_text),
            arg_int(0),
            arg_int(0),
            arg_int64(r.timestamp),
            arg_int(r.result),
        }));
    }
    return event_batch(ref, event_id, args);
}

std::vector<uint8_t> event_batch(uint32_t ref, uint16_t event_id,
                                 const std::vector<Arg>& args) {
    std::vector<uint8_t> out;
    open_batch(&out, 0x07);
    out.push_back(0x01);
    put_ref(&out, ref);
    put_u16(&out, event_id);
    uint8_t argc = 0;
    for (const Arg& a : args)
        if (a.kind != Arg::Kind::Nil) argc++;
    out.push_back(argc);
    for (const Arg& a : args) put_arg(&out, a);
    out.push_back(0x00);
    return out;
}

std::vector<uint8_t> physics06_batch(uint32_t ref) {
    std::vector<uint8_t> out;
    open_batch(&out, 0x06);
    out.push_back(0x01);
    put_u32(&out, ref);
    put_u32(&out, 0);
    put_u32(&out, ref);
    const uint32_t clk = net_clock_ms();
    put_u32(&out, clk);
    put_u32(&out, clk);
    put_u32(&out, clk + 2);
    put_u32(&out, 0);
    put_u32(&out, 0);
    put_u32(&out, 0);
    put_u32(&out, 0);
    put_u32(&out, 0xffffffffu);
    out.push_back(0x01);
    out.push_back(0x00);
    return out;
}

std::vector<uint8_t> cframe_batch(uint32_t ref, uint16_t prop, float x, float y,
                                  float z) {
    std::vector<uint8_t> out;
    open_batch(&out, 0x03);
    out.push_back(0x01);
    put_u32(&out, ref);
    put_u16(&out, prop);
    out.push_back(0x01); // vfmt: every live 0x03 item carries this byte
    put_f32(&out, x);
    put_f32(&out, y);
    put_f32(&out, z);
    // Tag 27 reads position, then a rotation kind. Kind 2 is the table
    // entry (+X, +Y, cross) == identity; kind 0 would demand 6 more bytes
    // of quantized quaternion.
    out.push_back(0x02);
    out.push_back(0x00);
    return out;
}

// The same 45-byte message, laid out the way the reference client actually sends
// it. Re-derived byte for byte on the 2026-11-08 capture (place 1818, 1814
// frames over 811 s), which corrects the field order used before:
//
//   83 05 16 | a u32 | 0 u32 | b u32 | 0 u32 | x y z (3f) | u u32 | fl u8
//             | c u32 | f f32 | 00
//
// The capture's first frame is
//   83 05 16 1b160000 00000000 1b160000 00000000 | 1b91fa41 349f6a41 00000000
//   | 00000000 | 01 | 1c160000 | 80a12b44 | 00
// i.e. x=31.3209 y=14.6639 z=0 at offset 19, not 15. The earlier layout put
// the position where the reference puts its second zero, so every transform we
// sent named the wrong bytes as coordinates.
//
// a == b and both climb ~997/s, so they are one millisecond clock sampled
// twice; c is a second sample within +-7 ms of it (measured e-a over 1814
// frames). `u` is 0 or 0xffffffff and `fl` is 0 or 1, each roughly half the
// time and uncorrelated with the other, so they are flags, not clocks. `f` is
// a client-local scalar in 618..878 across ten captures (never 0) whose
// semantics are still unresolved — see FINDINGS.md.
std::vector<uint8_t> physics_batch_clocked(uint32_t clock_a, uint32_t clock_b,
                                           uint32_t clock_c, float x, float y,
                                           float z, uint32_t flags, float f) {
    std::vector<uint8_t> out;
    open_batch(&out, 0x05);
    out.push_back(0x16);
    put_u32(&out, clock_a);
    put_u32(&out, 0);
    put_u32(&out, clock_b);
    put_u32(&out, 0);
    put_f32(&out, x);
    put_f32(&out, y);
    put_f32(&out, z);
    put_u32(&out, flags);
    out.push_back(0x01);
    put_u32(&out, clock_c);
    put_f32(&out, f);
    out.push_back(0x00);
    return out;
}

uint32_t net_clock_ms() {
    int64_t origin = g_clock_origin_ns.load();
    if (!origin) {
        origin = steady_ns();
        g_clock_origin_ns.store(origin);
    }
    return g_clock_base.load() +
           uint32_t((steady_ns() - origin) / 1000000);
}

void rebase_net_clock(uint32_t base) {
    g_clock_origin_ns.store(steady_ns());
    g_clock_base.store(base);
}

// 0x83/04 — 12-byte client-ready marker from the reference's post-join stream:
//   83 04 d6 09 00 00 | 23 62 00 00 00 | 00
// i.e. item 04 <millisecond clock>, item 23 <240 Hz tick>, end marker. The two
// numbers are different domains in the reference (2518 ms, tick 98). Sent once,
// before the first 0x83/05, in the same session the avatar spawns in.
std::vector<uint8_t> ready_batch(uint32_t clock, uint32_t tick) {
    std::vector<uint8_t> out;
    open_batch(&out, 0x04);
    put_u32(&out, clock);
    out.push_back(0x23);
    put_u32(&out, tick);
    out.push_back(0x00);
    return out;
}

// The standalone clock frame: the whole body of a 0x83/23 record and nothing
// else. `net_tick_240()` is the domain of the value it carries.
std::vector<uint8_t> clock_batch(uint32_t tick) {
    std::vector<uint8_t> out;
    open_batch(&out, 0x23);
    put_u32(&out, tick);
    out.push_back(0x00);
    return out;
}

uint32_t net_tick_240() {
    return uint32_t((uint64_t(net_clock_ms()) * 240u) / 1000u);
}

// 0x83/0a — one ownership claim for an instance. Reference body, measured
// (chain item, 12 B, 109 of them in a 4.8 s capture):
//   0a 01 <ref u32> b8 15 01 <server clock u32>
// The old builder appended a second 0x00, which is what earned 0x15/0105;
// the chain end marker is the caller's single 0x00.
// 0x83/04 with the first transform chained inside it, the way the reference
// sends it (measured 2026-11-06, /tmp/rc6 dgram_115: 04 <event> 05 <transform> 00).
// Ownership is decided at spawn time, so the ready message has to carry the
// initial position claim or the body is spawned server-owned.
std::vector<uint8_t> ready_with_transform(uint32_t clock, uint32_t tick,
                                          float x, float y,
                                          float z, uint32_t flags, float f) {
    // Native 55-byte 0x83/04 shape (same-job capture):
    //   04 <clock> | 05 16 <a><0><b><pos><0><flags><0><c><f>
    //              | 23 <clock> | 00
    // The extra 0 before c is part of the transform item, not a ref scope.
    std::vector<uint8_t> out;
    open_batch(&out, 0x04);
    put_u32(&out, clock);
    out.push_back(0x05);
    out.push_back(0x16);
    put_u32(&out, clock);
    put_u32(&out, 0);
    put_u32(&out, clock);
    put_f32(&out, x);
    put_f32(&out, y);
    put_f32(&out, z);
    put_u32(&out, 0);
    put_u32(&out, flags);
    out.push_back(0x00);
    put_u32(&out, clock + 3);
    put_f32(&out, f);
    out.push_back(0x23);
    put_u32(&out, tick);
    out.push_back(0x00);
    return out;
}

// One client-authored spawn item: 83 02 | ref | u16 class | FF | FF | parent | 00
// Grammar inverted from parseSpawnItem (repl_stream.cpp): an empty property
// loop is a single 0xFF delta, and a ref is [lo varint][u32 id].
std::vector<uint8_t> client_spawn(uint32_t ref, uint16_t cls,
                                  uint32_t parent) {
    std::vector<uint8_t> out;
    open_batch(&out, 0x02);
    out.push_back(0x01); // ref lo
    put_u32(&out, ref);
    put_u16(&out, cls);
    out.push_back(0xFF); // property loop A: empty
    out.push_back(0xFF); // property loop B: empty
    out.push_back(0x01); // parent ref lo
    put_u32(&out, parent);
    out.push_back(0x00);
    return out;
}

// TOUCH report: [id][ref a][ref b][phase][0], refs as [var lo][u32 id].
std::vector<uint8_t> touch_batch(uint8_t id, uint32_t a, uint32_t b,
                                 uint8_t phase) {
    // Wire shape confirmed by a public implementation: app id 0x86, then two
    // instance refs encoded as [leb peer-id length][u32 instance id] and
    // nothing else — no phase byte, no chain terminator. `phase` is kept in the
    // signature for callers and deliberately not written.
    (void)phase;
    std::vector<uint8_t> out;
    out.push_back(id);
    out.push_back(0x01);
    put_u32(&out, a);
    out.push_back(0x01);
    put_u32(&out, b);
    return out;
}

// RequestCharacter (ClientReplicator::RequestCharacterItem::write):
//   item type 8 | u32 stats/hack flags | string (spawn name, empty) | player id
// This is the message that makes the RCC replicate a player's own character;
// nothing else in the stream asks for it.
std::vector<uint8_t> request_character(uint32_t player_ref, uint32_t flags) {
    std::vector<uint8_t> out;
    open_batch(&out, 0x08);
    put_u32(&out, flags);
    out.push_back(0x00); // empty spawn-name string
    out.push_back(0x01); // ref lo
    put_u32(&out, player_ref);
    out.push_back(0x00); // chain end
    return out;
}

// One float property write: 83 03 01 <ref> <prop u16> <f32> 00
std::vector<uint8_t> value_batch(uint32_t ref, uint16_t prop, float v) {
    std::vector<uint8_t> out;
    open_batch(&out, 0x03);
    out.push_back(0x01);
    put_u32(&out, ref);
    put_u16(&out, prop);
    put_f32(&out, v);
    out.push_back(0x00);
    return out;
}

std::vector<uint8_t> ref_batch(uint32_t ref, uint16_t prop, uint32_t target) {
    std::vector<uint8_t> out;
    open_batch(&out, 0x03);
    out.push_back(0x01);
    put_u32(&out, ref);
    put_u16(&out, prop);
    out.push_back(0x01); // vfmt, same byte every live 0x03 item carries
    out.push_back(0x01); // tag 28 ref: non-null scope (lo=1)
    put_u32(&out, target);
    out.push_back(0x00);
    return out;
}

std::vector<uint8_t> isr_empty(uint32_t tick, uint32_t ack) {
    std::vector<uint8_t> out;
    out.push_back(0xA1);
    put_u32(&out, tick);
    put_u32(&out, 0); // records
    put_u32(&out, ack);
    return out;
}

std::vector<uint8_t> isr_batch(uint32_t tick, uint32_t ack,
                               const std::vector<IsrRecord>& records,
                               uint32_t flags) {
    if (records.empty()) return isr_empty(tick, ack);
    std::vector<uint8_t> out;
    out.push_back(0xA1);
    put_u32(&out, tick);
    put_u32(&out, uint32_t(records.size()));
    put_u32(&out, ack);
    put_u32(&out, flags);
    for (const IsrRecord& r : records) {
        // Ref: varint lo (1 = replicated) then the u32 id, exactly what
        // read_ref() in repl_core.cpp expects on the receive side.
        out.push_back(0x01);
        put_u32(&out, r.ref);
        put_u16(&out, r.cls);
        put_u16(&out, r.member_count);
        const size_t words = (size_t(r.member_count) + 63) / 64;
        if (words == 0) return {};
        for (size_t w = 0; w < words; ++w) {
            uint64_t mask = 0;
            if (size_t(r.member_index) / 64 == w)
                mask |= uint64_t(1) << (r.member_index % 64);
            for (int i = 0; i < 8; ++i) out.push_back(uint8_t(mask >> (8 * i)));
        }
        if (r.value.size() > 0xFFFF) return {};
        put_u16(&out, uint16_t(r.value.size()));
        out.insert(out.end(), r.value.begin(), r.value.end());
    }
    return out;
}

std::vector<uint8_t> interest_batch(uint32_t ref, uint32_t clock) {
    std::vector<uint8_t> out;
    open_batch(&out, 0x0a);
    out.push_back(0x01);
    put_u32(&out, ref);
    out.push_back(0xb8);
    out.push_back(0x15);
    out.push_back(0x01);
    put_u32(&out, clock);
    out.push_back(0x00);
    return out;
}

std::vector<uint8_t> vector3_batch(uint32_t ref, uint16_t prop, float x,
                                   float y, float z) {
    std::vector<uint8_t> out;
    open_batch(&out, 0x03);
    out.push_back(0x01);
    put_ref(&out, ref);
    put_u16(&out, prop);
    put_f32(&out, x);
    put_f32(&out, y);
    put_f32(&out, z);
    out.push_back(0x00);
    return out;
}

} // namespace batch
} // namespace rbxclient
