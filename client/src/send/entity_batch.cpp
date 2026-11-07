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

void put_ref(std::vector<uint8_t>* out, uint32_t ref) { put_u32(out, ref); }

static void put_leb(std::vector<uint8_t>* out, uint32_t v) {
    do {
        uint8_t b = uint8_t(v & 0x7F);
        v >>= 7;
        out->push_back(uint8_t(b | (v ? 0x80 : 0)));
    } while (v);
}

void put_arg(std::vector<uint8_t>* out, const Arg& a) {
    switch (a.kind) {
    case Arg::Kind::String:
        out->push_back(0x02);
        out->push_back(uint8_t(a.str.size() > 255 ? 255 : a.str.size()));
        out->insert(out->end(), a.str.begin(),
                    a.str.begin() + (a.str.size() > 255 ? 255 : a.str.size()));
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
    case Arg::Kind::Nil:
        break; // argc already excludes it
    }
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

std::vector<uint8_t> physics_batch(uint32_t ref, uint32_t tick, float x,
                                   float y, float z, uint32_t tail_ref) {
    std::vector<uint8_t> out;
    open_batch(&out, 0x05);
    out.push_back(0x16);
    put_u32(&out, ref);
    put_u32(&out, tick);
    put_u32(&out, ref);
    put_f32(&out, x);
    put_f32(&out, y);
    put_f32(&out, z);
    put_u32(&out, 0);
    put_u32(&out, 0);
    out.push_back(0x01);
    put_u32(&out, tail_ref);
    put_f32(&out, 0.0f);
    out.push_back(0x00);
    return out;
}

// The same 45-byte message, laid out the way the reference client actually sends
// it (measured 2026-11-05, decoded from its plaintext capture at place 189707):
//
//   83 05 16 | a u32 | 0 u32 | b u32 | x y z (3f) | 0 u32 | flags u32 | 01
//             | c u32 | f f32 | 00
//
// a, b and c are not references — they climb monotonically (~+500 per 20
// messages) while the position moves smoothly, so they are clocks. Sending an
// instance reference there puts a multi-million value in a clock field, which
// is why every earlier attempt was acknowledged and never applied.
// `flags` alternates between 0 and 0xffffffff; `f` tracks the camera.
std::vector<uint8_t> physics_batch_clocked(uint32_t clock_a, uint32_t clock_b,
                                           uint32_t clock_c, float x, float y,
                                           float z, uint32_t flags, float f) {
    std::vector<uint8_t> out;
    open_batch(&out, 0x05);
    out.push_back(0x16);
    put_u32(&out, clock_a);
    put_u32(&out, 0);
    put_u32(&out, clock_b);
    put_f32(&out, x);
    put_f32(&out, y);
    put_f32(&out, z);
    put_u32(&out, 0);
    put_u32(&out, flags);
    out.push_back(0x01);
    put_u32(&out, clock_c);
    put_f32(&out, f);
    out.push_back(0x00);
    return out;
}

// One 0x83 chain carrying the claim + transform + ownership items together,
// the way the reference does it (measured: 05, 06 and 0a share a datagram).
std::vector<uint8_t> physics_chain(uint32_t ref, uint32_t tail,
                                   uint32_t srv_clock, uint32_t clock, float x,
                                   float y, float z, uint32_t flags, float f,
                                   bool claims) {
    const std::vector<uint8_t> six = physics06_batch(ref);
    const std::vector<uint8_t> five =
        physics_batch_clocked(clock, clock, clock + 3, x, y, z, flags, f);
    const std::vector<uint8_t> claim = interest_batch(ref, srv_clock);
    const std::vector<uint8_t> claim2 = tail ? interest_batch(tail, srv_clock)
                                             : std::vector<uint8_t>();
    std::vector<uint8_t> out;
    out.reserve(six.size() + five.size() + claim.size() + claim2.size() + 2);
    out.push_back(0x83);
    auto add = [&](const std::vector<uint8_t>& m) {
        if (m.size() < 3) return;
        out.insert(out.end(), m.begin() + 1, m.end() - 1); // item type + body
    };
    // 0x06 is an ack record list, not a claim: it must name the instances the
    // client actually applied. Sending a fabricated ref made the RCC resend
    // every property chain forever. Acks go out separately (LiveRuntime).
    (void)six;
    add(five);
    if (claims) {
        add(claim);
        add(claim2);
    }
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
// i.e. item 04 <clock>, clock item 23 <clock>, end marker. Sent once, before
// the first 0x83/05, in the same session the avatar spawns in.
std::vector<uint8_t> ready_batch(uint32_t clock) {
    std::vector<uint8_t> out;
    open_batch(&out, 0x04);
    put_u32(&out, clock);
    out.push_back(0x23);
    put_u32(&out, clock);
    out.push_back(0x00);
    return out;
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
std::vector<uint8_t> ready_with_transform(uint32_t clock, float x, float y,
                                          float z, uint32_t flags, float f) {
    std::vector<uint8_t> out;
    open_batch(&out, 0x04);
    put_u32(&out, clock);
    out.push_back(0x23);
    put_u32(&out, clock);
    // transform item, same field order as the standalone 0x83/05
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
    out.push_back(0x01);
    put_u32(&out, clock + 3);
    put_f32(&out, f);
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
