// entity_batch.cpp — see entity_batch.h.
#include "send/entity_batch.h"

#include "instance/variant.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

std::vector<uint8_t> clock_ping9(uint8_t id, uint32_t clock_ms) {
    std::vector<uint8_t> out;
    out.reserve(9);
    out.push_back(id);
    out.insert(out.end(), 5, 0x00);
    out.push_back(uint8_t(clock_ms >> 16));
    out.push_back(uint8_t(clock_ms >> 8));
    out.push_back(uint8_t(clock_ms));
    return out;
}

std::vector<uint8_t> clock_ping25(uint8_t id, uint64_t system_ms,
                                  uint64_t session_ms, uint32_t nonce) {
    std::vector<uint8_t> out;
    out.reserve(25);
    out.push_back(id);
    for (int i = 7; i >= 0; --i) out.push_back(uint8_t(system_ms >> (8 * i)));
    for (int i = 7; i >= 0; --i) out.push_back(uint8_t(session_ms >> (8 * i)));
    out.push_back(0x00);
    out.push_back(0x06);
    out.push_back(0x60);
    out.push_back(0x06);
    out.push_back(uint8_t(nonce >> 24));
    out.push_back(uint8_t(nonce >> 16));
    out.push_back(uint8_t(nonce >> 8));
    out.push_back(uint8_t(nonce));
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

namespace {

void append_cframe_item(std::vector<uint8_t>& out, uint32_t ref, uint16_t prop,
                        const rbx::CFrame& cf) {
    put_u32(&out, ref);
    put_u16(&out, prop);
    out.push_back(0x01); // vfmt: every live 0x03 item carries this byte
    put_f32(&out, float(cf.x));
    put_f32(&out, float(cf.y));
    put_f32(&out, float(cf.z));

    // Tag 27: position, then one rotation byte.  Kind 1..36 is a pair of
    // signed axes from the native table; kind 0 is followed by a quantized
    // quaternion (u16 low, u32 high) as decoded by joindata.cpp.
    // Must match the decoder's table order (joindata.cpp cframe_table):
    // +X, +Y, +Z, -X, -Y, -Z.
    static const double kAxes[6][3] = {
        {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    int table_kind = 0;
    for (int kind = 1; kind <= 36 && !table_kind; ++kind) {
        const double* c0 = kAxes[(kind - 1) / 6];
        const double* c1 = kAxes[(kind - 1) % 6];
        const double c2[3] = {c0[1] * c1[2] - c0[2] * c1[1],
                              c0[2] * c1[0] - c0[0] * c1[2],
                              c0[0] * c1[1] - c0[1] * c1[0]};
        const double m[9] = {c0[0], c1[0], c2[0], c0[1], c1[1], c2[1],
                             c0[2], c1[2], c2[2]};
        const double have[9] = {cf.r00, cf.r01, cf.r02, cf.r10, cf.r11, cf.r12,
                                cf.r20, cf.r21, cf.r22};
        bool same = true;
        for (int i = 0; i < 9 && same; ++i)
            same = std::abs(have[i] - m[i]) < 1e-4;
        if (same)
            table_kind = kind;
    }
    if (table_kind) {
        out.push_back(uint8_t(table_kind));
    } else {
        const double tr = cf.r00 + cf.r11 + cf.r22;
        double w, x, y, z;
        if (tr > 0.0) {
            const double s = std::sqrt(tr + 1.0) * 2.0;
            w = 0.25 * s;
            x = (cf.r21 - cf.r12) / s;
            y = (cf.r02 - cf.r20) / s;
            z = (cf.r10 - cf.r01) / s;
        } else if (cf.r00 > cf.r11 && cf.r00 > cf.r22) {
            const double s = std::sqrt(1.0 + cf.r00 - cf.r11 - cf.r22) * 2.0;
            w = (cf.r21 - cf.r12) / s;
            x = 0.25 * s;
            y = (cf.r01 + cf.r10) / s;
            z = (cf.r02 + cf.r20) / s;
        } else if (cf.r11 > cf.r22) {
            const double s = std::sqrt(1.0 + cf.r11 - cf.r00 - cf.r22) * 2.0;
            w = (cf.r02 - cf.r20) / s;
            x = (cf.r01 + cf.r10) / s;
            y = 0.25 * s;
            z = (cf.r12 + cf.r21) / s;
        } else {
            const double s = std::sqrt(1.0 + cf.r22 - cf.r00 - cf.r11) * 2.0;
            w = (cf.r10 - cf.r01) / s;
            x = (cf.r02 + cf.r20) / s;
            y = (cf.r12 + cf.r21) / s;
            z = 0.25 * s;
        }
        const double inv =
            1.0 / std::sqrt(x * x + y * y + z * z + w * w);
        x *= inv;
        y *= inv;
        z *= inv;
        w *= inv;
        double q[4] = {x, y, z, w};
        int largest = 0;
        for (int i = 1; i < 4; ++i)
            if (std::abs(q[i]) > std::abs(q[largest]))
                largest = i;
        if (q[largest] < 0.0)
            for (int i = 0; i < 4; ++i)
                q[i] = -q[i];
        static const int kTab[12] = {1, 2, 3, 0, 2, 3, 0, 1, 3, 0, 1, 2};
        const int i0 = kTab[3 * largest];
        const int i1 = kTab[3 * largest + 1];
        const int i2 = kTab[3 * largest + 2];
        const double scale = 16383.0 / 0.70710677;
        auto quant = [scale](double v) -> int {
            return int(std::lround(std::clamp(v * scale, -16383.0, 16383.0)));
        };
        // Wire layout consumed by cframe_table's quaternion path: the separate
        // u16 is q[i0], bits 0..14 of `high` are q[i2], bits 15..29 are q[i1],
        // and bits 30..31 select the omitted component.
        const uint16_t low = uint16_t(quant(q[i0]) & 0xffff);
        const uint32_t high = (uint32_t(largest & 0x3) << 30) |
                              (uint32_t(quant(q[i1]) & 0x7fff) << 15) |
                              uint32_t(quant(q[i2]) & 0x7fff);
        out.push_back(0x00);
        out.push_back(uint8_t(low & 0xff));
        out.push_back(uint8_t(low >> 8));
        put_u32(&out, high);
    }
}

} // namespace

std::vector<uint8_t> cframe_batch(uint32_t ref, uint16_t prop,
                                  const rbx::CFrame& cf) {
    std::vector<uint8_t> out;
    open_batch(&out, 0x03);
    out.push_back(0x01);
    append_cframe_item(out, ref, prop, cf);
    out.push_back(0x00);
    return out;
}

std::vector<uint8_t> cframe_batch(const std::vector<CFrameWrite>& writes) {
    std::vector<uint8_t> out;
    open_batch(&out, 0x03);
    const size_t n = std::min<size_t>(writes.size(), 255);
    out.push_back(uint8_t(n));
    size_t used = 0;
    for (const CFrameWrite& w : writes) {
        if (used++ >= n || !w.cframe)
            continue;
        append_cframe_item(out, w.ref, w.prop, *w.cframe);
    }
    out.push_back(0x00);
    return out;
}

std::vector<uint8_t> cframe_batch(uint32_t ref, uint16_t prop, float x, float y,
                                  float z) {
    rbx::CFrame cf;
    cf.x = x;
    cf.y = y;
    cf.z = z;
    return cframe_batch(ref, prop, cf);
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

std::vector<uint8_t> client_replication(int32_t clock, uint16_t channel,
                                        const std::vector<uint8_t>& inner) {
    std::vector<uint8_t> out;
    out.reserve(inner.size() + 10);
    open_batch(&out, 0x1c);
    out.push_back(0x00); // flags: zero in every observed native frame
    put_u32(&out, uint32_t(clock));
    put_u16(&out, channel);
    out.insert(out.end(), inner.begin(), inner.end());
    out.push_back(0x00);
    return out;
}

std::vector<uint8_t> wrap_client_batch(const std::vector<uint8_t>& raw_batch,
                                       int32_t clock, uint16_t channel) {
    if (raw_batch.size() < 2 || raw_batch[0] != 0x83)
        return {};
    size_t end = raw_batch.size();
    if (raw_batch[end - 1] == 0x00)
        --end;
    if (end <= 1)
        return {};
    std::vector<uint8_t> inner(raw_batch.begin() + 1, raw_batch.begin() + end);
    return client_replication(clock, channel, inner);
}

std::vector<uint8_t> region_removal_batch(
    const std::vector<rbxclient::RegionRemovalRecord>& regions) {
    if (regions.empty())
        return {};
    std::vector<uint8_t> out;
    open_batch(&out, 0x0e);
    bool first = true;
    for (const auto& r : regions) {
        const bool large = r.region.large || r.region.x < -128 ||
                           r.region.x > 127 || r.region.y < -128 ||
                           r.region.y > 127 || r.region.z < -128 ||
                           r.region.z > 127;
        // The first record's tag is the 0x83 subtype byte; every following
        // record repeats it (native 83 0e 00 ... 0e 00 ...).
        if (!first)
            out.push_back(0x0e);
        first = false;
        out.push_back(large ? 1 : 0);
        if (large) {
            put_u32(&out, uint32_t(r.region.x));
            put_u32(&out, uint32_t(r.region.y));
            put_u32(&out, uint32_t(r.region.z));
        } else {
            out.push_back(uint8_t(int8_t(r.region.x)));
            out.push_back(uint8_t(int8_t(r.region.y)));
            out.push_back(uint8_t(int8_t(r.region.z)));
        }
        put_u32(&out, r.revision);
        put_u32(&out, uint32_t(r.instances.size()));
        out.push_back(0x00); // uncompressed GUID list (the only wire branch)
        for (const auto& ref : r.instances) {
            out.push_back(ref.scope);
            if (ref.scope != 0)
                put_u32(&out, ref.id);
        }
    }
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

std::vector<uint8_t> ack0a_batch(const std::vector<Ack0a>& records,
                                 uint32_t tick) {
    std::vector<uint8_t> out;
    // The first 0a is both the container subtype and the first record's item
    // tag; do not open_batch() here or the first record is written twice.
    out.push_back(0x83);
    for (const Ack0a& r : records) {
        out.push_back(0x0a);
        out.push_back(0x01);
        put_u32(&out, r.ref);
        put_u16(&out, r.prop);
        out.push_back(r.has_value ? 0x01 : 0x00);
        if (r.has_value)
            out.push_back(r.value);
        put_u32(&out, r.clock);
    }
    if (tick != 0) {
        out.push_back(0x23);
        put_u32(&out, tick);
    }
    out.push_back(0x00);
    return out;
}

std::vector<uint8_t> interest_batch(uint32_t ref, uint32_t clock) {
    Ack0a r;
    r.ref = ref;
    r.prop = 0x15b8; // historical short-form property id
    r.has_value = false;
    r.clock = clock;
    return ack0a_batch({r}, 0);
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
