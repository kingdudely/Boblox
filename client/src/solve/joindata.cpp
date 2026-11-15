// joindata.cpp — JoinData snapshot -> live DataModel instances.
//
// Engine-profile only (see joindata.h). Value decoding ports the native
// property readers (sub_340A3E4 family) field-for-field; see FINDINGS.md
// "JoinData record codec" for the per-tag provenance. Wire member names
// are matched against the registry exactly, then case-insensitively
// (2.739 wire names like "size"/"shape" postdate our dump's "Size"/"Shape");
// anything else skips with counts, never guesses.
#include "solve/joindata.h"

#include "instance/class_registry.h"
#include "instance/instance.h"
#include "instance/signal.h"
#include "instance/value_types.h"
#include "instance/variant.h"
#include "instance/vector3.h"

#include "lua.h"

#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace rbxclient {

namespace {

uint64_t read_varint(const uint8_t*& p, const uint8_t* end, bool* ok) {
    uint64_t v = 0;
    int shift = 0;
    while (true) {
        if (p >= end) {
            *ok = false;
            return 0;
        }
        const uint8_t b = *p++;
        v |= uint64_t(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80))
            return v;
        if (shift >= 64) {
            *ok = false;
            return 0;
        }
    }
}

float read_f32(const uint8_t*& p, const uint8_t* end, bool* ok) {
    if (size_t(end - p) < 4) {
        *ok = false;
        return 0;
    }
    float v = 0;
    std::memcpy(&v, p, 4);
    p += 4;
    return v;
}

double read_f64(const uint8_t*& p, const uint8_t* end, bool* ok) {
    if (size_t(end - p) < 8) {
        *ok = false;
        return 0;
    }
    double v = 0;
    std::memcpy(&v, p, 8);
    p += 8;
    return v;
}

int32_t read_s32(const uint8_t*& p, const uint8_t* end, bool* ok) {
    if (size_t(end - p) < 4) {
        *ok = false;
        return 0;
    }
    int32_t v = 0;
    std::memcpy(&v, p, 4);
    p += 4;
    return v;
}

uint16_t read_u16(const uint8_t*& p, const uint8_t* end, bool* ok) {
    if (size_t(end - p) < 2) {
        *ok = false;
        return 0;
    }
    uint16_t v = uint16_t(p[0]) | (uint16_t(p[1]) << 8);
    p += 2;
    return v;
}

uint32_t read_u32(const uint8_t*& p, const uint8_t* end, bool* ok) {
    if (size_t(end - p) < 4) {
        *ok = false;
        return 0;
    }
    uint32_t v = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                 (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    p += 4;
    return v;
}

std::string lower(std::string s) {
    for (char& ch : s)
        ch = char(tolower((unsigned char)ch));
    return s;
}

// CFrame table path (sub_262D411): kind k (1..32); column 0 = axes[(k-1)/6],
// column 1 = axes[(k-1)%6], column 2 = cross. Axes baked from sub_262D4FD
// (_mm_set_ss rows: +X,+Y,+Z,-X,-Y,-Z).
void cframe_table(unsigned kind, double r[9]) {
    static const double kAxes[6][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1},
                                       {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    static const double kZero[3] = {0, 0, 0};
    // Mirrors sub_262D4FD: row (kind-1)/6, clamping past row 5 to the
    // all-zero default row (never sent by the server; parity edge).
    const double* c0 = (kind - 1) / 6 < 6 ? kAxes[(kind - 1) / 6] : kZero;
    const double* c1 = kAxes[(kind - 1) % 6];
    const double c2[3] = {c0[1] * c1[2] - c0[2] * c1[1],
                          c0[2] * c1[0] - c0[0] * c1[2],
                          c0[0] * c1[1] - c0[1] * c1[0]};
    r[0] = c0[0];
    r[1] = c1[0];
    r[2] = c2[0];
    r[3] = c0[1];
    r[4] = c1[1];
    r[5] = c2[1];
    r[6] = c0[2];
    r[7] = c1[2];
    r[8] = c2[2];
}

// CFrame quantized path (sub_33BEB0A + sub_29C7B4E), float arithmetic like
// the native (xmm f32 ops), widened to double on store.
void cframe_quat(uint16_t a, uint32_t n, double r[9]) {
    static const int kTab[12] = {1, 2, 3, 0, 2, 3, 0, 1, 3, 0, 1, 2};
    const uint32_t v5 = n >> 30;
    const int i0 = kTab[3 * v5], i1 = kTab[3 * v5 + 1], i2 = kTab[3 * v5 + 2];
    auto s15 = [](uint32_t w) -> int32_t {
        return int32_t(w << 17) >> 17;
    };
    const float f0 =
        float(s15(n)) / 16383.0f * 0.70710677f;
    const float f1 =
        float(s15(a)) / 16383.0f * 0.70710677f;
    const float v6 =
        float(int32_t(n * 4u) >> 17) / 16383.0f * 0.70710677f;
    float q[4] = {0, 0, 0, 0};
    q[i0] = f1;
    q[i1] = v6;
    q[i2] = f0;
    q[v5] = sqrtf(fmaxf(1.0f - (f0 * f0 + f1 * f1 + v6 * v6), 0.0f));
    const float inv =
        1.0f / sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    const float x = q[0] * inv, y = q[1] * inv, z = q[2] * inv, w = q[3] * inv;
    r[0] = 1.0 - (y * (y + y) + z * (z + z));
    r[1] = (x + x) * y - (z + z) * w; // 2xy-2wz
    r[2] = (x + x) * z + (y + y) * w; // 2xz+2yw
    r[3] = (z + z) * w + (x + x) * y; // 2xy+2wz
    r[4] = 1.0 - (z * (z + z) + x * (x + x));
    r[5] = (y + y) * z - (x + x) * w; // 2yz-2xw
    r[6] = (x + x) * z - (y + y) * w; // 2xz-2yw
    r[7] = (y + y) * z + (x + x) * w; // 2yz+2xw
    r[8] = 1.0 - (y * (y + y) + x * (x + x));
}

// Decode tag-27/51 rotation+position. Returns false for null pivots
// (leave the default) or undecodable input.
bool decode_cframe(const std::vector<uint8_t>& raw, uint8_t tag, double pos[3],
                   double rot[9], bool* is_null) {
    const uint8_t* p = raw.data();
    const uint8_t* end = p + raw.size();
    bool ok = true;
    *is_null = false;
    if (tag == 51) {
        // OptionalCoordinateFrame: kind first; high bit clear = null.
        if (p >= end)
            return false;
        const uint8_t k = *p++;
        if (!(k & 0x80)) {
            *is_null = true; // empty pivot: keep the default
            return true;
        }
        if ((k & 0x7F) == 0) {
            if (size_t(end - p) < 6)
                return false;
            const uint16_t a = read_u16(p, end, &ok);
            const uint32_t n = read_u32(p, end, &ok);
            if (!ok)
                return false;
            cframe_quat(a, n, rot);
        } else {
            cframe_table(k & 0x7F, rot);
        }
    } else if (tag == 27) {
        // CFrame: position first, then kind.
        if (size_t(end - p) < 13)
            return false;
        pos[0] = read_f32(p, end, &ok);
        pos[1] = read_f32(p, end, &ok);
        pos[2] = read_f32(p, end, &ok);
        if (!ok || p >= end)
            return false;
        const unsigned kind = *p++;
        if (kind == 0) {
            if (size_t(end - p) < 6)
                return false;
            const uint16_t a = read_u16(p, end, &ok);
            const uint32_t n = read_u32(p, end, &ok);
            if (!ok)
                return false;
            cframe_quat(a, n, rot);
        } else {
            cframe_table(kind, rot);
        }
        return true;
    } else {
        // tag 26 is unused in the 2.739 schema; shape per IDA only.
        return false;
    }
    pos[0] = read_f32(p, end, &ok);
    pos[1] = read_f32(p, end, &ok);
    pos[2] = read_f32(p, end, &ok);
    return ok && p == end;
}

// Push one decoded replicated-event argument as a Lua value. Always pushes
// exactly one value (nil for a shape this client cannot materialize) so the
// caller can keep positional argument counts intact. `depth` bounds nested
// Tuple/ValueTable containers, matching the parser's own nesting limit.
bool push_event_arg(lua_State* L, const EventArg& a, const JoinSchema& schema,
                    const ApplyReport& rep, int depth) {
    if (depth > 32) {
        lua_pushnil(L);
        return false;
    }
    const uint8_t* p = a.value.data();
    const uint8_t* end = p + a.value.size();
    bool ok = true;
    switch (a.tag) {
    case 0:
    case 3: // empty / zero-width value
        lua_pushnil(L);
        return true;
    case 2:
    case 8: { // string: [varint len][bytes]
        const uint64_t n = read_varint(p, end, &ok);
        if (!ok || size_t(end - p) < n) {
            lua_pushnil(L);
            return false;
        }
        lua_pushlstring(L, reinterpret_cast<const char*>(p), size_t(n));
        return true;
    }
    case 7: { // enum: [varint value], descriptor[1..2] = enum table index
        const uint64_t w = read_varint(p, end, &ok);
        if (!ok || a.extra >= schema.enums.size()) {
            lua_pushnil(L);
            return false;
        }
        rbx::push_enumitem(L, rbx::EnumItem{schema.enums[a.extra], int(w)});
        return true;
    }
    case 9: { // bool / u8
        if (end - p < 1) {
            lua_pushnil(L);
            return false;
        }
        lua_pushboolean(L, p[0] != 0);
        return true;
    }
    case 10:
    case 44: { // zigzag varint (int / int64)
        const uint64_t w = read_varint(p, end, &ok);
        if (!ok) {
            lua_pushnil(L);
            return false;
        }
        const int64_t s = int64_t(w >> 1) ^ -int64_t(w & 1);
        lua_pushnumber(L, double(s));
        return true;
    }
    case 34: { // unsigned varint
        const uint64_t w = read_varint(p, end, &ok);
        if (!ok) {
            lua_pushnil(L);
            return false;
        }
        lua_pushnumber(L, double(w));
        return true;
    }
    case 11: { // f32
        const float f = read_f32(p, end, &ok);
        if (!ok) {
            lua_pushnil(L);
            return false;
        }
        lua_pushnumber(L, double(f));
        return true;
    }
    case 12:
    case 55:
    case 62: { // f64 / u64
        const double d = read_f64(p, end, &ok);
        if (!ok) {
            lua_pushnil(L);
            return false;
        }
        lua_pushnumber(L, d);
        return true;
    }
    case 16:
    case 17: { // u32
        const uint32_t w = read_u32(p, end, &ok);
        if (!ok) {
            lua_pushnil(L);
            return false;
        }
        lua_pushnumber(L, double(w));
        return true;
    }
    case 48: { // DateTime (ms since epoch)
        const double d = read_f64(p, end, &ok);
        if (!ok) {
            lua_pushnil(L);
            return false;
        }
        int64_t ms = 0;
        std::memcpy(&ms, &d, 8);
        rbx::push_datetime(L, rbx::DateTime{ms});
        return true;
    }
    case 13: { // UDim: f32 scale + s32 offset
        const float s = read_f32(p, end, &ok);
        const int32_t o = read_s32(p, end, &ok);
        if (!ok) {
            lua_pushnil(L);
            return false;
        }
        rbx::push_udim(L, rbx::UDim{double(s), double(o)});
        return true;
    }
    case 14: { // UDim2: f32,s32,f32,s32
        const float sx = read_f32(p, end, &ok);
        const int32_t ox = read_s32(p, end, &ok);
        const float sy = read_f32(p, end, &ok);
        const int32_t oy = read_s32(p, end, &ok);
        if (!ok) {
            lua_pushnil(L);
            return false;
        }
        rbx::push_udim2(L, rbx::UDim2{rbx::UDim{double(sx), double(ox)},
                                      rbx::UDim{double(sy), double(oy)}});
        return true;
    }
    case 21: { // Vector2: 2x f32
        const float x = read_f32(p, end, &ok);
        const float y = read_f32(p, end, &ok);
        if (!ok) {
            lua_pushnil(L);
            return false;
        }
        rbx::push_vector2(L, rbx::Vector2{double(x), double(y)});
        return true;
    }
    case 37: { // NumberRange: 2x f32
        const float lo = read_f32(p, end, &ok);
        const float hi = read_f32(p, end, &ok);
        if (!ok) {
            lua_pushnil(L);
            return false;
        }
        rbx::push_numberrange(L, rbx::NumberRange{double(lo), double(hi)});
        return true;
    }
    case 40: { // Rect: min.xy + max.xy
        rbx::Rect r;
        r.min.x = double(read_f32(p, end, &ok));
        r.min.y = double(read_f32(p, end, &ok));
        r.max.x = double(read_f32(p, end, &ok));
        r.max.y = double(read_f32(p, end, &ok));
        if (!ok) {
            lua_pushnil(L);
            return false;
        }
        rbx::push_rect(L, r);
        return true;
    }
    case 15: { // Ray: origin + direction
        rbx::Ray r;
        r.origin.x = double(read_f32(p, end, &ok));
        r.origin.y = double(read_f32(p, end, &ok));
        r.origin.z = double(read_f32(p, end, &ok));
        r.direction.x = double(read_f32(p, end, &ok));
        r.direction.y = double(read_f32(p, end, &ok));
        r.direction.z = double(read_f32(p, end, &ok));
        if (!ok) {
            lua_pushnil(L);
            return false;
        }
        rbx::push_ray(L, r);
        return true;
    }
    case 18: { // BrickColor: u16 number
        const uint16_t w = read_u16(p, end, &ok);
        if (!ok) {
            lua_pushnil(L);
            return false;
        }
        rbx::push_brickcolor(L, rbx::BrickColor{int(w)});
        return true;
    }
    case 19: { // Color3: 3x f32
        const float r = read_f32(p, end, &ok);
        const float g = read_f32(p, end, &ok);
        const float b = read_f32(p, end, &ok);
        if (!ok) {
            lua_pushnil(L);
            return false;
        }
        rbx::push_color3(L, rbx::Color3{double(r), double(g), double(b)});
        return true;
    }
    case 20: { // Color3uint8: 3x u8
        if (end - p < 3) {
            lua_pushnil(L);
            return false;
        }
        rbx::push_color3(L, rbx::Color3{p[0] / 255.0, p[1] / 255.0,
                                        p[2] / 255.0});
        return true;
    }
    case 22:
    case 36: { // Vector3: 3x f32
        const float x = read_f32(p, end, &ok);
        const float y = read_f32(p, end, &ok);
        const float z = read_f32(p, end, &ok);
        if (!ok) {
            lua_pushnil(L);
            return false;
        }
        rbx::push_vector3(L, rbx::Vector3{double(x), double(y), double(z)});
        return true;
    }
    case 26:
    case 27:
    case 51: { // CFrame / OptionalCoordinateFrame
        double pos[3] = {0, 0, 0};
        double rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        bool is_null = false;
        if (!decode_cframe(a.value, a.tag, pos, rot, &is_null)) {
            lua_pushnil(L);
            return false;
        }
        if (is_null) {
            lua_pushnil(L);
            return true;
        }
        rbx::CFrame cf;
        cf.x = pos[0];
        cf.y = pos[1];
        cf.z = pos[2];
        cf.r00 = rot[0];
        cf.r01 = rot[1];
        cf.r02 = rot[2];
        cf.r10 = rot[3];
        cf.r11 = rot[4];
        cf.r12 = rot[5];
        cf.r20 = rot[6];
        cf.r21 = rot[7];
        cf.r22 = rot[8];
        rbx::push_cframe(L, cf);
        return true;
    }
    case 28:
    case 69: { // entity reference: [varint lo]([u32 id] iff lo != 0)
        const uint64_t lo = read_varint(p, end, &ok);
        if (!ok) {
            lua_pushnil(L);
            return false;
        }
        if (lo == 0) {
            lua_pushnil(L);
            return true;
        }
        const uint32_t id = read_u32(p, end, &ok);
        if (!ok) {
            lua_pushnil(L);
            return false;
        }
        const auto it = rep.by_id.find(id);
        if (it == rep.by_id.end() || it->second < 0 ||
            size_t(it->second) >= rep.instances.size()) {
            lua_pushnil(L); // unresolved refs are absent, never a bogus value
            return true;
        }
        rbx::Instance::push(L, rep.instances[size_t(it->second)].get());
        return true;
    }
    case 29:
    case 30:
    case 31:
    case 32: { // Tuple / ValueArray / ValueTable / ValueMap
        const bool keyed = a.tag == 31 || a.tag == 32;
        lua_createtable(L, keyed ? 0 : int(a.elements.size()), 0);
        for (size_t i = 0; i < a.elements.size(); ++i) {
            push_event_arg(L, a.elements[i], schema, rep, depth + 1);
            if (keyed && i < a.keys.size()) {
                lua_setfield(L, -2, a.keys[i].c_str());
            } else {
                lua_rawseti(L, -2, int(i + 1));
            }
        }
        return true;
    }
    case 49: { // Name: shared-table index or literal string
        if (p >= end) {
            lua_pushnil(L);
            return false;
        }
        const uint8_t u = *p++;
        if (u & 0x80) {
            const size_t idx = u & 0x7F;
            if (idx == 127 || idx >= schema.shared.size()) {
                lua_pushnil(L);
                return false;
            }
            lua_pushlstring(L, schema.shared[idx].data(),
                            schema.shared[idx].size());
            return true;
        }
        if (u == 127) {
            const uint64_t n = read_varint(p, end, &ok);
            if (!ok || size_t(end - p) < n) {
                lua_pushnil(L);
                return false;
            }
            lua_pushlstring(L, reinterpret_cast<const char*>(p), size_t(n));
            return true;
        }
        if (size_t(end - p) < u) {
            lua_pushnil(L);
            return false;
        }
        lua_pushlstring(L, reinterpret_cast<const char*>(p), u);
        return true;
    }
    case 33: { // asset string (event/prefix mode): [u8 p]([varint id]|[varint len][bytes])
        if (p >= end) {
            lua_pushnil(L);
            return false;
        }
        const uint8_t pfx = *p++;
        if (pfx & 1) {
            const uint64_t id = read_varint(p, end, &ok);
            if (!ok) {
                lua_pushnil(L);
                return false;
            }
            const std::string uri = "rbxassetid://" + std::to_string(id);
            lua_pushlstring(L, uri.data(), uri.size());
            return true;
        }
        const uint64_t n = read_varint(p, end, &ok);
        if (!ok || size_t(end - p) < n) {
            lua_pushnil(L);
            return false;
        }
        lua_pushlstring(L, reinterpret_cast<const char*>(p), size_t(n));
        return true;
    }
    case 54: { // FontFace: u16 weight, u8 style, family strings
        const uint16_t weight = read_u16(p, end, &ok);
        if (!ok || p >= end) {
            lua_pushnil(L);
            return false;
        }
        const uint8_t style = *p++;
        std::string family;
        for (int i = 0; i < 2; ++i) {
            const uint32_t n = read_u32(p, end, &ok);
            if (!ok || size_t(end - p) < n) {
                lua_pushnil(L);
                return false;
            }
            if (i == 0)
                family.assign(reinterpret_cast<const char*>(p), n);
            p += n;
        }
        rbx::push_font(L, rbx::Font{family, int(weight), int(style)});
        return true;
    }
    case 61: { // Content: [varint form]; prefix mode form 1 carries a prefix byte
        const uint64_t form = read_varint(p, end, &ok);
        if (!ok) {
            lua_pushnil(L);
            return false;
        }
        if (form == 0) {
            rbx::push_content(L, rbx::Content{});
            return true;
        }
        if (form == 2) {
            const uint64_t lo = read_varint(p, end, &ok);
            if (!ok) {
                lua_pushnil(L);
                return false;
            }
            if (lo == 0) {
                rbx::push_content(L, rbx::Content{});
                return true;
            }
            const uint32_t id = read_u32(p, end, &ok);
            if (!ok) {
                lua_pushnil(L);
                return false;
            }
            const auto it = rep.by_id.find(id);
            rbx::Content c;
            c.source = 2;
            if (it != rep.by_id.end() && it->second >= 0 &&
                size_t(it->second) < rep.instances.size())
                c.object = rep.instances[size_t(it->second)].get();
            rbx::push_content(L, c);
            return true;
        }
        if (form == 1) {
            if (p >= end) {
                lua_pushnil(L);
                return false;
            }
            const uint8_t pfx = *p++;
            rbx::Content c;
            c.source = 1;
            if (pfx & 1) {
                const uint64_t id = read_varint(p, end, &ok);
                if (!ok) {
                    lua_pushnil(L);
                    return false;
                }
                c.uri = "rbxassetid://" + std::to_string(id);
            } else {
                const uint64_t n = read_varint(p, end, &ok);
                if (!ok || size_t(end - p) < n) {
                    lua_pushnil(L);
                    return false;
                }
                c.uri.assign(reinterpret_cast<const char*>(p), size_t(n));
            }
            rbx::push_content(L, c);
            return true;
        }
        lua_pushnil(L); // form 3+ is the unmapped complex shape
        return false;
    }
    default:
        lua_pushnil(L);
        return false;
    }
}

} // namespace

struct PendingRef {
    size_t index;
    std::string prop;
    uint64_t lo;
    uint32_t ref_id;
};

// One record: spawn (or update when update_existing) + scalar props.
// Refs defer to `pending` (resolved by the caller). Never fails: every
// unmappable piece is counted, never guessed. update_existing=false
// preserves snapshot first-wins retransmit semantics.
static void applyRecordPass1(ApplyReport& rep, const JoinSchema& schema,
                             const JoinRecord& r, bool update_existing,
                             std::vector<PendingRef>& pending) {
rbx::Instance* inst = nullptr;
size_t index = 0;
const rbx::ClassInfo* cls = nullptr;
const auto known = rep.by_id.find(r.id);
const bool exists = known != rep.by_id.end() && known->second >= 0;
if (exists && !update_existing) {
    ++rep.stats.skipped_records; // retransmitted id: first wins
    return;
}
if (size_t(r.cls) >= schema.classes.size()) {
    ++rep.stats.skipped_records;
    rep.by_id[r.id] = -1;
    return;
}
const std::string& cls_name = schema.classes[r.cls].name;
cls = rbx::find_class(cls_name);
if (!cls) {
    ++rep.stats.skipped_records;
    ++rep.stats.skip_names[cls_name]; // name the class in RBX_DUMP_JOIN
    rep.by_id[r.id] = -1;
    return;
}
// The client owns Players.LocalPlayer: it is created at join (see
// LiveRuntime::ensureLocalPlayer) and exists before any replicated Player
// record. When the server does replicate our Player, fill that instance in
// rather than spawning a twin — the native client has exactly one.
rbx::Instance* placeholder = nullptr;
if (!exists && rbx::local_user_id() > 0 && rbx::class_isa(cls, "Player")) {
    // The 2.739 schema carries both UserId and the wire-only lowercase
    // userId (both tag 44 varints); accept either spelling. A streaming spawn
    // may carry Name alone and leave UserId to a later 0x03 update, so the
    // account name is an equally valid identity here: LocalPlayer is keyed by
    // exactly one Player, and a twin is never resolved by either lookup.
    int64_t rec_uid = 0;
    bool have_uid = false;
    std::string rec_name;
    for (const JoinProp& p : r.props) {
        const std::string want = lower(p.name);
        if (want == "userid" && !have_uid && !p.value.empty()) {
            const uint8_t* q = p.value.data();
            bool ok = true; // read_varint only clears it on failure
            const uint64_t w = read_varint(q, q + p.value.size(), &ok);
            // Tag 44 is a zigzag varint (see the property decoder below).
            if (ok)
                rec_uid = int64_t(w >> 1) ^ -int64_t(w & 1);
            have_uid = ok;
        } else if (want == "name" && rec_name.empty()) {
            // Name is a length-prefixed string unless the writer used a
            // shared-string reference (high bit set), which cannot be
            // resolved here and simply leaves the fallback unused.
            const uint8_t* q = p.value.data();
            const uint8_t* qe = q + p.value.size();
            if (q != qe && *q < 0x80) {
                bool ok = true;
                const uint64_t n = read_varint(q, qe, &ok);
                if (ok && size_t(qe - q) >= n)
                    rec_name.assign(reinterpret_cast<const char*>(q), size_t(n));
            }
        }
    }
    const bool is_us =
        (have_uid && rec_uid == rbx::local_user_id()) ||
        (!have_uid && !rec_name.empty() &&
         rec_name == rbx::local_user_name());
    if (std::getenv("RBX_PLAYER_DEBUG")) {
        std::string names;
        size_t vbytes = 0, vfilled = 0;
        for (const JoinProp& p : r.props) {
            names += (names.empty() ? "" : ",") + p.name;
            vbytes += p.value.size();
            if (!p.value.empty())
                ++vfilled;
        }
        std::fprintf(stderr,
                     "playerdebug spawn id=%u parent=%lld exists=%d "
                     "uid=%lld/%d name=%s/%s is_us=%d props=%zu filled=%zu "
                     "vbytes=%zu [%s]\n",
                     r.id, (long long)r.parent, int(exists),
                     (long long)rec_uid, int(have_uid), rec_name.c_str(),
                     rbx::local_user_name().c_str(), int(is_us), r.props.size(),
                     vfilled, vbytes, names.c_str());
    }
    if (is_us) {
        if (rbx::Instance* players =
                rep.root->find_first_child_of_class("Players")) {
            for (const auto& child : players->children()) {
                if (!child->is_a("Player"))
                    continue;
                if (have_uid) {
                    rbx::Variant uid;
                    if (!child->get_prop("UserId", uid))
                        continue;
                    const auto* id = std::get_if<int64_t>(&uid);
                    if (!id || *id != rec_uid)
                        continue;
                } else if (!rec_name.empty() && child->name() != rec_name) {
                    continue;
                }
                placeholder = child.get();
                break;
            }
        }
    }
}
if (exists) {
    index = size_t(known->second);
    inst = rep.instances[index].get();
} else if (placeholder) {
    index = rep.instances.size();
    rep.instances.push_back(rbx::Ref<rbx::Instance>(placeholder));
    rep.by_id[r.id] = int(index);
    inst = placeholder;
} else {
    rbx::Ref<rbx::Instance> fresh = rbx::instantiate(cls);
    index = rep.instances.size();
    rep.instances.push_back(fresh);
    rep.by_id[r.id] = int(index);
    inst = rep.instances[index].get();
    ++rep.stats.spawned;
}
// First materialization owns the wire identity. Later property-only
// passes (applyStreamProp) carry a synthetic lo=1 and must not clobber
// the id the snapshot/spawn installed.
if (!exists && r.lo)
    inst->set_network_identity(r.lo, r.id);
// Effective props (base-first) for exact + alias lookup.
const std::vector<rbx::PropInfo> eprops =
    rbx::effective_properties(cls);
for (const JoinProp& p : r.props) {
    const rbx::PropInfo* decl = rbx::find_property(cls, p.name);
    std::string used = p.name;
    if (!decl) {
        // Normal case-insensitive alias (size->Size, shape->Shape), plus
        // the one wire-only spelling the registry cannot resolve: the
        // BasePart serializer sends Color3uint8 (tag 20, 3 bytes) while the
        // engine property is Color. Both are the same Color3 value.
        std::string want = lower(p.name);
        if (want == "color3uint8")
            want = "color";
        for (const rbx::PropInfo& e : eprops) {
            if (lower(e.name) == want) {
                decl = rbx::find_property(cls, e.name);
                used = e.name;
                break;
            }
        }
        if (decl)
            rep.stats.aliases[p.name] = used;
    }
    const std::string key = cls_name + "." + p.name;
    // A script's Source is normally a 16-byte content hash (tag 47), not a
    // string: it is the key of the 0x78 script-data bulk that carries the
    // bytecode. Keep the mapping before property decoding so a declared
    // Source property cannot turn the hash into a type mismatch.
    if (p.name == "Source" && p.value.size() == 16 &&
        rbx::class_isa(cls, "LuaSourceContainer")) {
        const std::string h(reinterpret_cast<const char*>(p.value.data()), 16);
        rep.source_hash[h] = r.id;
        rep.source_half.emplace(h.substr(0, 8), r.id);
        rep.source_half.emplace(h.substr(8), r.id);
        ++rep.stats.source_hashes;
        continue;
    }
    if (!decl) {
        ++rep.stats.props_skipped_undeclared;
        ++rep.stats.skip_names[key];
        if (std::getenv("RBX_PROP_DEBUG") && p.name == "Source") {
            std::string hex;
            static const char* kHex = "0123456789abcdef";
            for (size_t k = 0; k < p.value.size() && k < 24; ++k) {
                hex.push_back(kHex[p.value[k] >> 4]);
                hex.push_back(kHex[p.value[k] & 15]);
            }
            std::fprintf(stderr, "propdebug %s tag=0x%02x len=%zu %s\n",
                         key.c_str(), unsigned(p.tag), p.value.size(),
                         hex.c_str());
        }
        continue;
    }
    if (decl->readonly) {
        ++rep.stats.props_skipped_readonly;
        continue;
    }
    const uint8_t* dp = p.value.data();
    const uint8_t* dend = dp + p.value.size();
    bool ok = true;
    rbx::Variant var;
    bool have = false;
    bool is_ref = false;
    uint64_t ref_lo = 0;
    uint32_t ref_id = 0;
    const uint8_t tag = p.tag;
    // BasePart.ReplicationPV (tag 57). It is Hidden, NotScriptable and
    // RobloxSecurity, so native has no Lua surface for it; keep the decoded
    // struct in engine-side state instead of installing a property. This is
    // the full wire value (position/linear/angular blocks and the orientation
    // table selector), not a raw-byte placeholder.
    if (tag == 57 && lower(p.name) == "replicationpv") {
        ReplicationPV pv;
        size_t used = 0;
        std::string perr;
        if (decodeReplicationPV(p.value.data(), p.value.size(), pv, &used,
                                &perr) &&
            used == p.value.size()) {
            rep.replication_pv[r.id] = pv;
            ++rep.stats.pv_values;
        } else {
            ++rep.stats.pv_invalid;
        }
        continue;
    }
    // RBX_DIAG=1: dump the raw wire form of UserId. The decoded value is
    // sign-extended from low 32 bits (measured: 11924576267 -> -960325621),
    // which no branch below explains, so the bytes decide it.
    if (p.name == "UserId" && std::getenv("RBX_DIAG")) {
        std::fprintf(stderr, "[diag] UserId tag=%u bytes=%zu [",
                     unsigned(p.tag), p.value.size());
        for (size_t di = 0; di < p.value.size() && di < 16; ++di)
            std::fprintf(stderr, "%02x", p.value[di]);
        std::fprintf(stderr, "]\n");
    }
    switch (decl->type) {
    case rbx::PropType::Bool:
        if (tag == 9 && dend - dp >= 1) {
            var = rbx::Variant(dp[0] != 0);
            have = true;
        }
        break;
    case rbx::PropType::Int:
        if (tag == 10) {
            const uint64_t w = read_varint(dp, dend, &ok);
            if (ok) {
                const int64_t s =
                    int64_t(w >> 1) ^ -int64_t(w & 1);
                var = rbx::Variant(s);
                have = ok && dp == dend;
            }
        } else if (tag == 34) {
            // NetworkOwnerV3 is an unsigned varint on the wire (0 = server,
            // 1..N = the client the server granted the assembly to).  The
            // generic Int branch ignored it, so ownership never reached the
            // runtime and every unanchored body stayed server-owned.
            const uint64_t w = read_varint(dp, dend, &ok);
            if (ok) {
                var = rbx::Variant(int64_t(w));
                have = dp == dend;
            }
        } else if (tag == 44) {
            // Zigzag, exactly like tag 10 (sub_340A3E4 case 0x2C:
            // (w>>1)^-(w&1); e.g. wire 9312858590 -> 4656429295).
            const uint64_t w = read_varint(dp, dend, &ok);
            if (ok) {
                const int64_t s =
                    int64_t(w >> 1) ^ -int64_t(w & 1);
                var = rbx::Variant(s);
                have = ok && dp == dend;
            }
        } else if ((tag == 16 || tag == 17) && dend - dp >= 4) {
            uint32_t w = 0;
            std::memcpy(&w, dp, 4);
            var = rbx::Variant(int64_t(w));
            have = true;
            dp += 4;
            have = dp == dend;
        }
        break;
    case rbx::PropType::Double:
        if (tag == 11 && dend - dp >= 4) {
            var = rbx::Variant(double(read_f32(dp, dend, &ok)));
            have = ok && dp == dend;
        } else if (tag == 12 && dend - dp >= 8) {
            var = rbx::Variant(read_f64(dp, dend, &ok));
            have = ok && dp == dend;
        }
        break;
    case rbx::PropType::String:
        if (tag == 2 || tag == 8) {
            const uint8_t* q = dp;
            const uint64_t n = read_varint(q, dend, &ok);
            if (ok && q + n == dend) {
                var = rbx::Variant(std::string(
                    reinterpret_cast<const char*>(q), size_t(n)));
                have = true;
            } else {
                ok = false;
            }
        } else if (tag == 49 && !p.value.empty()) {
            const uint8_t u = dp[0];
            if (u & 0x80) {
                const size_t idx = u & 0x7F;
                if ((u & 0x7F) == 127 || idx >= schema.shared.size()) {
                    ok = false;
                } else {
                    var = rbx::Variant(schema.shared[idx]);
                    have = true;
                }
            } else if (u == 127) {
                ++dp;
                const uint64_t n = read_varint(dp, dend, &ok);
                if (ok && dp + n == dend) {
                    var = rbx::Variant(std::string(
                        reinterpret_cast<const char*>(dp), size_t(n)));
                    have = true;
                } else {
                    ok = false;
                }
            } else if (dp + 1 + u == dend) {
                var = rbx::Variant(std::string(
                    reinterpret_cast<const char*>(dp + 1), u));
                have = true;
            } else {
                ok = false;
            }
        }
        break;
    case rbx::PropType::Vector3:
        if ((tag == 22 || tag == 36) && dend - dp >= 12) {
            rbx::Vector3 v{double(read_f32(dp, dend, &ok)),
                           double(read_f32(dp, dend, &ok)),
                           double(read_f32(dp, dend, &ok))};
            if (ok && dp == dend) {
                var = rbx::Variant(v);
                have = true;
            } else {
                ok = false;
            }
        }
        break;
    case rbx::PropType::Instance:
        if (tag == 28 || tag == 69) {
            const uint64_t w = read_varint(dp, dend, &ok);
            if (ok && (w == 0 || dp + 4 == dend)) {
                is_ref = true;
                ref_lo = w;
                ref_id = w == 0 ? 0 : read_u32(dp, dend, &ok);
                have = ok;
            } else {
                ok = false;
            }
        }
        break;
    case rbx::PropType::Color3:
        if (tag == 19 && dend - dp >= 12) {
            rbx::Color3 v{double(read_f32(dp, dend, &ok)),
                          double(read_f32(dp, dend, &ok)),
                          double(read_f32(dp, dend, &ok))};
            if (ok && dp == dend) {
                var = rbx::Variant(v);
                have = true;
            } else {
                ok = false;
            }
        } else if (tag == 20 && dend - dp >= 3) {
            rbx::Color3 v{dp[0] / 255.0, dp[1] / 255.0, dp[2] / 255.0};
            var = rbx::Variant(v);
            have = (dp += 3) == dend;
        }
        break;
    case rbx::PropType::CFrame:
        if (tag == 27 || tag == 51) {
            double pos[3] = {0, 0, 0}, rot[9] = {1, 0, 0, 0, 1,
                                                 0, 0, 0, 1};
            bool is_null = false;
            if (decode_cframe(p.value, tag, pos, rot, &is_null) &&
                !is_null) {
                rbx::CFrame cf;
                cf.x = pos[0];
                cf.y = pos[1];
                cf.z = pos[2];
                cf.r00 = rot[0];
                cf.r01 = rot[1];
                cf.r02 = rot[2];
                cf.r10 = rot[3];
                cf.r11 = rot[4];
                cf.r12 = rot[5];
                cf.r20 = rot[6];
                cf.r21 = rot[7];
                cf.r22 = rot[8];
                var = rbx::Variant(cf);
                have = true;
            } else if (is_null) {
                continue; // keep the default pivot
            } else {
                ok = false;
            }
        }
        break;
    case rbx::PropType::Vector2:
        if (tag == 21 && dend - dp >= 8) {
            rbx::Vector2 v{double(read_f32(dp, dend, &ok)),
                           double(read_f32(dp, dend, &ok))};
            if (ok && dp == dend) {
                var = rbx::Variant(v);
                have = true;
            } else {
                ok = false;
            }
        }
        break;
    case rbx::PropType::BrickColor:
        if (tag == 18 && dend - dp >= 2) {
            const uint16_t w = read_u16(dp, dend, &ok);
            if (ok && dp == dend) {
                var = rbx::Variant(rbx::BrickColor{int(w)});
                have = true;
            } else {
                ok = false;
            }
        }
        break;
    case rbx::PropType::UDim:
        if (tag == 13 && dend - dp >= 8) {
            const float s = read_f32(dp, dend, &ok);
            const int32_t o = read_s32(dp, dend, &ok);
            if (ok && dp == dend) {
                var = rbx::Variant(
                    rbx::UDim{double(s), double(o)});
                have = true;
            } else {
                ok = false;
            }
        }
        break;
    case rbx::PropType::UDim2:
        if (tag == 14 && dend - dp >= 16) {
            const float sx = read_f32(dp, dend, &ok);
            const int32_t ox = read_s32(dp, dend, &ok);
            const float sy = read_f32(dp, dend, &ok);
            const int32_t oy = read_s32(dp, dend, &ok);
            if (ok && dp == dend) {
                var = rbx::Variant(rbx::UDim2{
                    rbx::UDim{double(sx), double(ox)},
                    rbx::UDim{double(sy), double(oy)}});
                have = true;
            } else {
                ok = false;
            }
        }
        break;
    case rbx::PropType::Rect:
        if (tag == 40 && dend - dp >= 16) {
            rbx::Rect rc;
            rc.min.x = double(read_f32(dp, dend, &ok));
            rc.min.y = double(read_f32(dp, dend, &ok));
            rc.max.x = double(read_f32(dp, dend, &ok));
            rc.max.y = double(read_f32(dp, dend, &ok));
            if (ok && dp == dend) {
                var = rbx::Variant(rc);
                have = true;
            } else {
                ok = false;
            }
        }
        break;
    case rbx::PropType::NumberRange:
        if (tag == 37 && dend - dp >= 8) {
            rbx::NumberRange nr{double(read_f32(dp, dend, &ok)),
                                double(read_f32(dp, dend, &ok))};
            if (ok && dp == dend) {
                var = rbx::Variant(nr);
                have = true;
            } else {
                ok = false;
            }
        }
        break;
    case rbx::PropType::ColorSequence:
        if (tag == 38 && dend - dp >= 4) {
            const uint32_t n = read_u32(dp, dend, &ok);
            rbx::ColorSequence cs;
            for (uint32_t i = 0; ok && i < n; ++i) {
                if (size_t(dend - dp) < 20) {
                    ok = false;
                    break;
                }
                rbx::ColorSequenceKeypoint k;
                k.time = double(read_f32(dp, dend, &ok));
                k.color.r = double(read_f32(dp, dend, &ok));
                k.color.g = double(read_f32(dp, dend, &ok));
                k.color.b = double(read_f32(dp, dend, &ok));
                k.envelope = double(read_f32(dp, dend, &ok));
                cs.keys.push_back(k);
            }
            if (ok && dp == dend) {
                var = rbx::Variant(cs);
                have = true;
            } else {
                ok = false;
            }
        }
        break;
    case rbx::PropType::Content:
        if (tag == 61 && !p.value.empty()) {
            const uint8_t* q = dp;
            const uint64_t form = read_varint(q, dend, &ok);
            if (!ok) {
                break;
            }
            if (form == 0 && q == dend) {
                var = rbx::Variant(rbx::Content{});
                have = true;
            } else if (form == 1) {
                const uint64_t n = read_varint(q, dend, &ok);
                if (ok && q + n == dend) {
                    rbx::Content cval;
                    cval.source = 1;
                    cval.uri.assign(
                        reinterpret_cast<const char*>(q), size_t(n));
                    var = rbx::Variant(cval);
                    have = true;
                } else {
                    ok = false;
                }
            } else if (form == 2) {
                const uint64_t w = read_varint(q, dend, &ok);
                if (ok && (w == 0 || q + 4 == dend)) {
                    is_ref = true;
                    ref_lo = w;
                    ref_id = w == 0 ? 0 : read_u32(q, dend, &ok);
                    have = ok;
                } else {
                    ok = false;
                }
            } else {
                ok = false;
            }
        }
        break;
    case rbx::PropType::PhysicalProperties:
        if (tag == 41 && !p.value.empty()) {
            const uint8_t f = *dp++;
            if ((f & 1) && dend - dp >= 20) {
                rbx::PhysicalProperties pp;
                pp.density = double(read_f32(dp, dend, &ok));
                pp.friction = double(read_f32(dp, dend, &ok));
                pp.elasticity = double(read_f32(dp, dend, &ok));
                pp.friction_weight = double(read_f32(dp, dend, &ok));
                pp.elasticity_weight =
                    double(read_f32(dp, dend, &ok));
                // Order past density..elasticityWeight is
                // HYPOTHESIS (wire order assumed == constructor
                // order); physics is server-side regardless.
                pp.acoustic_absorption = 1.0;
                if (f >= 2 && dend - dp >= 4)
                    pp.acoustic_absorption =
                        double(read_f32(dp, dend, &ok));
                if (ok && dp == dend) {
                    var = rbx::Variant(pp);
                    have = true;
                } else {
                    ok = false;
                }
            } else if (!(f & 1) && dp == dend) {
                var = rbx::Variant(rbx::PhysicalProperties{});
                have = true;
            } else {
                ok = false;
            }
        }
        break;
    case rbx::PropType::Ray:
        if (tag == 15 && dend - dp >= 24) {
            rbx::Ray ray;
            ray.origin.x = double(read_f32(dp, dend, &ok));
            ray.origin.y = double(read_f32(dp, dend, &ok));
            ray.origin.z = double(read_f32(dp, dend, &ok));
            ray.direction.x = double(read_f32(dp, dend, &ok));
            ray.direction.y = double(read_f32(dp, dend, &ok));
            ray.direction.z = double(read_f32(dp, dend, &ok));
            if (ok && dp == dend) {
                var = rbx::Variant(ray);
                have = true;
            } else {
                ok = false;
            }
        }
        break;
    case rbx::PropType::DateTime:
        if (tag == 48 && dend - dp >= 8) {
            uint64_t w = 0;
            std::memcpy(&w, dp, 8);
            var = rbx::Variant(
                rbx::DateTime{int64_t(w)});
            have = (dp += 8) == dend;
        }
        break;
    case rbx::PropType::Font:
        // tag 54: [u16 weight][u8 style][u32 family len][family]
        // [u32 face-source len][source] (source may be empty; the native
        // resolves the family JSON, a headless client carries it verbatim).
        if (tag == 54 && dend - dp >= 7) {
            rbx::Font f;
            f.weight = int(dp[0] | (dp[1] << 8));
            f.style = int(dp[2]);
            dp += 3;
            uint32_t flen = 0;
            std::memcpy(&flen, dp, 4);
            dp += 4;
            if (flen <= uint32_t(dend - dp)) {
                f.family.assign(reinterpret_cast<const char*>(dp), flen);
                dp += flen;
                if (dend - dp >= 4) {
                    uint32_t slen = 0;
                    std::memcpy(&slen, dp, 4);
                    dp += 4;
                    if (slen <= uint32_t(dend - dp)) {
                        dp += slen; // face URI: not part of the Font value
                        if (dp == dend) {
                            var = rbx::Variant(f);
                            have = true;
                        }
                    }
                }
            }
        }
        break;
    case rbx::PropType::Enum:
        if (tag == 7) {
            const SchemaClass& sc = schema.classes[r.cls];
            const SchemaMember* sm = nullptr;
            for (const SchemaMember& m : sc.members) {
                if (m.name == p.name) {
                    sm = &m;
                    break;
                }
            }
            const uint8_t* q = dp;
            const uint64_t w = read_varint(q, dend, &ok);
            if (ok && q == dend && sm && sm->has_extra &&
                sm->extra < schema.enums.size()) {
                var = rbx::Variant(rbx::EnumItem{
                    schema.enums[size_t(sm->extra)], int(w)});
                have = true;
            } else {
                ok = false;
            }
        }
        break;
    default:
        ok = false;
        break;
    }
    if (is_ref) {
        if (ref_lo == 0 && ref_id == 0) {
            std::string e2;
            if (!inst->set_prop(used, rbx::Variant(
                                           static_cast<rbx::Instance*>(
                                               nullptr)),
                                &e2))
                ++rep.stats.props_skipped_typemismatch;
            else
                ++rep.stats.props_set;
        } else {
            pending.push_back(
                PendingRef{size_t(index), used, ref_lo, ref_id});
        }
        continue;
    }
    if (!ok || !have) {
        ++rep.stats.props_skipped_typemismatch;
        continue;
    }
    // Once a headless script has moved its character locally, server CFrame
    // snapshots must not yank the local model back every tick.
    if (used == "CFrame" && inst->has_local_cframe())
        continue;
    std::string set_err;
    if (!inst->set_prop(used, var, &set_err))
        ++rep.stats.props_skipped_typemismatch;
    else
        ++rep.stats.props_set;
}
// RBX_DIAG=1: every record that ends up named after the local user, whatever its
// class and parent. Decides whether the server sends our own character at all, or
// whether it arrives and is dropped before the Player.Character link is rebuilt.
if (std::getenv("RBX_DIAG") && inst &&
    inst->name() == rbx::local_user_name())
{
    // Also print a Part child's CFrame: two Model records carry our name each
    // session, and only one of them is the body the RCC simulates.
    std::string pos = "none";
    for (const auto& ch : inst->children()) {
        if (!ch->is_a("Part")) continue;
        rbx::Variant cv;
        if (ch->get_prop("CFrame", cv))
            if (const auto* cf = std::get_if<rbx::CFrame>(&cv)) {
                char pb[96];
                std::snprintf(pb, sizeof pb, "%.2f,%.2f,%.2f", cf->x, cf->y,
                              cf->z);
                pos = pb;
            }
        break;
    }
    std::fprintf(stderr,
                 "[diag] record self name='%s' class=%s ref=%u parent=%lld "
                 "pos=%s\n",
                 inst->name().c_str(), cls_name.c_str(), unsigned(r.id),
                 static_cast<long long>(r.parent), pos.c_str());
}
}

// Pass-1 helper above; ref/parent resolution shared by both passes.
static rbx::Instance* lookupInstance(ApplyReport& rep, uint32_t id) {
    const auto it = rep.by_id.find(id);
    if (it == rep.by_id.end() || it->second < 0)
        return nullptr;
    return rep.instances[size_t(it->second)].get();
}

static void resolvePendingRefs(ApplyReport& rep,
                               const std::vector<PendingRef>& pending) {
    for (const PendingRef& pr : pending) {
        rbx::Instance* target =
            pr.ref_id == 0 ? nullptr : lookupInstance(rep, pr.ref_id);
        if (!target && pr.ref_id != 0)
            ++rep.stats.refs_missing;
        std::string e2;
        rbx::Instance* inst = rep.instances[pr.index].get();
        if (!inst->set_prop(pr.prop,
                            rbx::Variant(static_cast<rbx::Instance*>(target)),
                            &e2))
            ++rep.stats.props_skipped_typemismatch;
        else
            ++rep.stats.props_set;
    }
}

// Attach every deferred child waiting on `parent_id` that is now known. The
// snapshot can reference a parent from a later 0x83 batch (services and
// containers are streamed after the 0x97 records), so a missing parent is a
// deferral, never a graft.
static void attachPending(ApplyReport& rep, uint32_t parent_id) {
    if (rep.pending_parent.empty())
        return;
    const auto pit = rep.by_id.find(parent_id);
    if (pit == rep.by_id.end() || pit->second < 0)
        return;
    rbx::Instance* parent = rep.instances[size_t(pit->second)].get();
    for (auto it = rep.pending_parent.begin(); it != rep.pending_parent.end();) {
        if (it->second != parent_id) {
            ++it;
            continue;
        }
        const auto cit = rep.by_id.find(it->first);
        if (cit == rep.by_id.end() || cit->second < 0) {
            it = rep.pending_parent.erase(it);
            continue;
        }
        std::string e2;
        if (!rep.instances[size_t(cit->second)]->reparent(parent, &e2))
            ++rep.stats.grafted_to_root;
        it = rep.pending_parent.erase(it);
    }
}

void flushDeferredToRoot(ApplyReport& rep) {
    if (!rep.root)
        return;
    for (const auto& kv : rep.pending_parent) {
        const auto it = rep.by_id.find(kv.first);
        if (it == rep.by_id.end() || it->second < 0)
            continue;
        rbx::Instance* inst = rep.instances[size_t(it->second)].get();
        if (!inst || inst->parent())
            continue;
        std::string e;
        if (inst->reparent(rep.root.get(), &e))
            ++rep.stats.grafted_to_root;
    }
}

ApplyReport applyJoinSnapshot(const JoinSchema& schema,
                              const std::vector<JoinRecord>& records,
                              std::string* err) {
    ApplyReport rep;
    const rbx::ClassInfo* dm = rbx::find_class("DataModel");
    if (!dm) {
        if (err)
            *err = "DataModel class not registered";
        return rep;
    }
    rep.root = rbx::instantiate(dm);
    if (!rep.root) {
        if (err)
            *err = "cannot instantiate DataModel";
        return rep;
    }
    std::vector<PendingRef> pending;
    // RBX_DUMP_JOIN=1: one line per snapshot record, with whether its parent
    // is part of the same snapshot. Grafted counts alone cannot tell a broken
    // parent ref from a parent that only arrives in a later 0x83 batch.
    if (getenv("RBX_DUMP_JOIN")) {
        std::set<uint32_t> ids;
        for (const auto& r : records)
            if (r.lo)
                ids.insert(r.id);
        for (const auto& r : records) {
            std::string name;
            for (const auto& p : r.props)
                if (p.name == "Name") {
                    for (uint8_t c : p.value)
                        if (c >= 32 && c < 127 && name.size() < 40)
                            name.push_back(char(c));
                    break;
                }
            const char* cn = r.cls < schema.classes.size()
                                 ? schema.classes[r.cls].name.c_str()
                                 : "?";
            const char* where =
                r.parent < 0 || uint32_t(r.parent) == 13
                    ? "ROOT"
                    : (ids.count(uint32_t(r.parent)) ? "PARENT-IN-SNAPSHOT"
                                                     : "PARENT-MISSING");
            printf("joinrec id=%u cls=%s parent=%lld %s name=%s\n", r.id, cn,
                   (long long)r.parent, where, name.c_str());
        }
    }
    // Pass 1: spawn + scalar props.
    for (size_t ri = 0; ri < records.size(); ++ri)
        applyRecordPass1(rep, schema, records[ri], false, pending);
    // Pass 2: refs + parents.
    resolvePendingRefs(rep, pending);
    std::set<uint32_t> parented;
    for (size_t i = 0; i < records.size(); ++i) {
        // First occurrence only: retransmitted ids share by_id with the
        // original instance and must not reparent it.
        if (!parented.insert(records[i].id).second)
            continue;
        const auto it = rep.by_id.find(records[i].id);
        if (it == rep.by_id.end() || it->second < 0)
            continue;
        rbx::Instance* inst = rep.instances[size_t(it->second)].get();
        rbx::Instance* parent = nullptr;
        // Wire id 13 is the implicit DataModel root (FINDINGS: "53 children of
        // id 13 = the implicit DataModel root (Workspace, Players, Lighting,
        // …)"). It is never a record, so deferring on it detaches the whole
        // tree: root->children() ends up empty, there is no Players service,
        // PlayerAdded never fires and the avatar is never linked (measured
        // 2026-10-30: Player:1/PlayerGui:1 in the instance list, root empty).
        constexpr uint32_t kDataModelId = 13;
        const bool parent_is_root =
            records[i].parent >= 0 &&
            uint32_t(records[i].parent) == kDataModelId;
        if (records[i].parent >= 0 && !parent_is_root)
            parent = lookupInstance(rep, uint32_t(records[i].parent));
        if (!parent && records[i].parent >= 0 && !parent_is_root) {
            // Parent not in this snapshot: keep it for the batch that carries
            // it instead of losing the hierarchy to the root.
            rep.pending_parent[records[i].id] = uint32_t(records[i].parent);
            ++rep.stats.deferred_to_parent;
            continue;
        }
        if (!parent) {
            parent = rep.root.get();
            ++rep.stats.grafted_to_root;
        }
        std::string e2;
        if (!inst->reparent(parent, &e2))
            ++rep.stats.grafted_to_root;
    }
    // Services (Players/Workspace/…) are client-created and never appear as
    // records, so every record waiting on one of them attaches to the root
    // here. Streaming preserves the queue: when the real parent does arrive
    // in a later 0x83 batch, attachPending re-parents the child.
    flushDeferredToRoot(rep);
    if (getenv("RBX_DUMP_JOIN")) {
        // Which parent ids the snapshot references but never defines: the
        // client-created services (Players/Workspace/…) show up here when the
        // server does not replicate them. A histogram says whether the tree
        // hangs on a handful of service ids or on real missing instances.
        std::map<uint32_t, int> miss;
        for (const auto& kv : rep.pending_parent) ++miss[kv.second];
        printf("joinmiss skipped=%d grafted=%d deferred=%d parents:",
               rep.stats.skipped_records, rep.stats.grafted_to_root,
               rep.stats.deferred_to_parent);
        for (const auto& kv : miss) printf(" %u:%d", kv.first, kv.second);
        printf("\n");
        for (const auto& kv : rep.stats.skip_names)
            printf("joinskip %s x%d\n", kv.first.c_str(), kv.second);
    }
    return rep;
}

namespace {

// Attribute values use the same wire tag space as properties, but there is no
// class member declaration to disambiguate them. Decode only the types a
// dynamic attribute can hold and require the whole value to be consumed; an
// unknown tag is counted, never coerced into a wrong Lua type.
bool decodeAttributeValue(const A1AttributeUpdate& a, rbx::Variant* out,
                          std::string* err) {
    const uint8_t* p = a.value.data();
    const uint8_t* end = p + a.value.size();
    bool ok = true;
    auto done = [&]() {
        if (!ok || p != end) {
            if (err)
                *err = "attribute " + a.name + " tag " +
                       std::to_string(unsigned(a.tag)) +
                       (ok ? " has trailing bytes" : " is truncated");
            return false;
        }
        return true;
    };
    switch (a.tag) {
    case 2:
    case 8: { // string: varint length + bytes
        const uint64_t n = read_varint(p, end, &ok);
        if (!ok || n > uint64_t(end - p)) {
            ok = false;
            break;
        }
        *out = rbx::Variant(std::string(reinterpret_cast<const char*>(p),
                                        size_t(n)));
        p += n;
        break;
    }
    case 7: { // enum: [varint value], owning Enum name from the schema
        const uint64_t w = read_varint(p, end, &ok);
        if (ok)
            *out = rbx::Variant(rbx::EnumItem{a.enum_name, int(w)});
        break;
    }
    case 9: // boolean
        if (end - p < 1) {
            ok = false;
            break;
        }
        *out = rbx::Variant(p[0] != 0);
        ++p;
        break;
    case 11:
    case 16: // f32
        *out = rbx::Variant(double(read_f32(p, end, &ok)));
        break;
    case 12: // f64
        *out = rbx::Variant(read_f64(p, end, &ok));
        break;
    case 10:
    case 44: { // zigzag integer
        const uint64_t w = read_varint(p, end, &ok);
        if (ok)
            *out = rbx::Variant(int64_t(w >> 1) ^ -int64_t(w & 1));
        break;
    }
    case 17: { // u32
        const uint32_t w = read_u32(p, end, &ok);
        if (ok)
            *out = rbx::Variant(int64_t(w));
        break;
    }
    case 18: { // BrickColor number
        const uint16_t w = read_u16(p, end, &ok);
        if (ok)
            *out = rbx::Variant(rbx::BrickColor{int(w)});
        break;
    }
    case 21: { // Vector2: 2x f32
        const double x = read_f32(p, end, &ok);
        const double y = read_f32(p, end, &ok);
        *out = rbx::Variant(rbx::Vector2{x, y});
        break;
    }
    case 22:
    case 36: { // Vector3: 3x f32
        const double x = read_f32(p, end, &ok);
        const double y = read_f32(p, end, &ok);
        const double z = read_f32(p, end, &ok);
        *out = rbx::Variant(rbx::Vector3{x, y, z});
        break;
    }
    case 19: { // Color3: 3x f32
        const double r = read_f32(p, end, &ok);
        const double g = read_f32(p, end, &ok);
        const double b = read_f32(p, end, &ok);
        *out = rbx::Variant(rbx::Color3{r, g, b});
        break;
    }
    case 27:
    case 26: {
        double pos[3] = {0, 0, 0}, rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        bool is_null = false;
        if (!decode_cframe(a.value, a.tag, pos, rot, &is_null) || is_null) {
            ok = false;
            break;
        }
        rbx::CFrame cf;
        cf.x = pos[0]; cf.y = pos[1]; cf.z = pos[2];
        cf.r00 = rot[0]; cf.r01 = rot[1]; cf.r02 = rot[2];
        cf.r10 = rot[3]; cf.r11 = rot[4]; cf.r12 = rot[5];
        cf.r20 = rot[6]; cf.r21 = rot[7]; cf.r22 = rot[8];
        *out = rbx::Variant(cf);
        p = end; // decode_cframe consumed the whole raw value
        break;
    }
    default:
        ok = false;
        break;
    }
    return done();
}

} // namespace

bool applyStreamAttribute(ApplyReport& rep, uint32_t id,
                          const A1AttributeUpdate& attr, StreamStats& st) {
    if (attr.name.empty())
        return false;
    rbx::Instance* inst = lookupInstance(rep, id);
    if (!inst) {
        constexpr size_t kMaxPerId = 64;
        constexpr size_t kMaxIds = 4096;
        auto it = rep.pending_attrs.find(id);
        if (it != rep.pending_attrs.end()) {
            if (it->second.size() >= kMaxPerId) {
                ++rep.stats.attrs_unsupported;
                return false;
            }
            it->second.push_back({attr});
        } else if (rep.pending_attrs.size() < kMaxIds) {
            rep.pending_attrs[id].push_back({attr});
        } else {
            ++rep.stats.attrs_unsupported;
            return false;
        }
        ++rep.stats.attrs_deferred;
        return true;
    }
    auto note_signal = [&] {
        constexpr size_t kMaxSignals = 4096;
        if (rep.attr_signals.size() < kMaxSignals)
            rep.attr_signals.push_back({inst, attr.name});
    };
    if (attr.tag == 0) { // removal form
        inst->remove_attr(attr.name);
        ++rep.stats.attrs_removed;
        note_signal();
        return true;
    }
    rbx::Variant value;
    std::string err;
    if (!decodeAttributeValue(attr, &value, &err)) {
        ++rep.stats.attrs_unsupported;
        return false;
    }
    inst->set_attr(attr.name, value);
    ++rep.stats.attrs_set;
    note_signal();
    return true;
}

void applyStreamRecord(ApplyReport& rep, const JoinSchema& schema,
                       uint32_t id, uint16_t cls,
                       const std::vector<JoinProp>& props, int64_t parent,
                       StreamStats& st) {
    if (!rep.root)
        return;
    const auto known = rep.by_id.find(id);
    const bool exists = known != rep.by_id.end() && known->second >= 0;
    if (exists)
        ++st.updated;
    else
        ++st.spawned;
    JoinRecord r;
    r.lo = 1;
    r.id = id;
    r.cls = cls;
    r.props = props;
    r.plo = parent >= 0 ? 1 : 0;
    r.parent = parent;
    std::vector<PendingRef> pending;
    const int spawned_before = rep.stats.spawned;
    applyRecordPass1(rep, schema, r, true, pending);
    if (rep.stats.spawned == spawned_before) {
        // Unknown class (counted inside as skipped_records); an
        // update for a never-spawned id lands here too — recount it.
        if (!exists)
            --st.spawned;
    }
    resolvePendingRefs(rep, pending);
    const auto it = rep.by_id.find(id);
    if (it == rep.by_id.end() || it->second < 0)
        return; // unknown class: nothing to parent
    rbx::Instance* inst = rep.instances[size_t(it->second)].get();
    rbx::Instance* par = nullptr;
    constexpr uint32_t kDataModelId = 13; // implicit DataModel root, never a record
    const bool parent_is_root = parent >= 0 && uint32_t(parent) == kDataModelId;
    if (parent >= 0 && !parent_is_root)
        par = lookupInstance(rep, uint32_t(parent));
    if (!par && parent >= 0 && !parent_is_root) {
        // Same deferral as the snapshot path; attach when the parent streams in.
        rep.pending_parent[id] = uint32_t(parent);
        ++rep.stats.deferred_to_parent;
    } else {
        if (!par) {
            par = rep.root.get();
            ++st.stream_grafted;
        }
        std::string e2;
        if (!inst->reparent(par, &e2))
            ++st.stream_grafted;
    }
    // This id may itself be the parent other records are waiting on.
    attachPending(rep, id);
    // ... and it may be the entity an out-of-order 0x03 triple named. The
    // native replicator keeps such updates and applies them on spawn; the
    // property is otherwise lost for the rest of the session.
    auto pending_props = rep.pending_props.find(id);
    if (pending_props != rep.pending_props.end()) {
        std::vector<DeferredProp> queued = std::move(pending_props->second);
        rep.pending_props.erase(pending_props);
        for (const DeferredProp& dp : queued) {
            if (!applyStreamProp(rep, schema, id, dp.cls, dp.prop, st))
                ++st.prop_missing;
        }
    }
    auto pending_attrs = rep.pending_attrs.find(id);
    if (pending_attrs != rep.pending_attrs.end()) {
        std::vector<DeferredAttr> queued = std::move(pending_attrs->second);
        rep.pending_attrs.erase(pending_attrs);
        for (const DeferredAttr& da : queued)
            applyStreamAttribute(rep, id, da.attr, st);
    }
}

void logStreamEvent(const JoinSchema& schema, const EventItem& ev,
                    StreamStats& st) {
    ++st.events_logged;
    // Resolve the event name when the index is in range (out-of-range
    // indexes can't happen from parseEventItem, but never trust input).
    size_t idx = 0;
    for (size_t ci = 0; ci < schema.classes.size(); ++ci) {
        const auto& evs = schema.classes[ci].events;
        if (size_t(ev.event) >= idx && size_t(ev.event) < idx + evs.size()) {
            const std::string name =
                schema.classes[ci].name + "." + evs[size_t(ev.event) - idx].name;
            ++st.events_by_name[name];
            return;
        }
        idx += evs.size();
    }
    ++st.events_by_name["?"];
}

bool applyParentUpdate(ApplyReport& rep, uint32_t id, int64_t parent,
                       StreamStats& st) {
    if (!rep.root)
        return false;
    const auto known = rep.by_id.find(id);
    if (known == rep.by_id.end() || known->second < 0 ||
        size_t(known->second) >= rep.instances.size())
        return false;
    rbx::Instance* child = rep.instances[size_t(known->second)].get();
    if (!child)
        return false;
    rbx::Instance* target = nullptr;
    if (parent >= 0) {
        const auto pit = rep.by_id.find(uint32_t(parent));
        if (pit == rep.by_id.end() || pit->second < 0 ||
            size_t(pit->second) >= rep.instances.size()) {
            // The native replicator keeps the relation until the parent
            // exists; attaching later is attachPending's job.
            rep.pending_parent[id] = uint32_t(parent);
            ++st.prop_deferred;
            return true;
        }
        target = rep.instances[size_t(pit->second)].get();
    }
    std::string err;
    if (!child->reparent(target, &err))
        return false;
    ++st.prop_applied;
    return true;
}

bool applyStreamProp(ApplyReport& rep, const JoinSchema& schema, uint32_t id,
                     uint16_t cls, const JoinProp& prop, StreamStats& st) {
    if (!rep.root || prop.name.empty())
        return false;
    const auto known = rep.by_id.find(id);
    if (known == rep.by_id.end() || known->second < 0) {
        // Entity not spawned yet: hold the update for applyStreamRecord. Cap
        // both the per-id and the global queue so a reaped id cannot grow
        // memory forever; anything dropped is counted, never silent.
        constexpr size_t kMaxPerId = 64;
        constexpr size_t kMaxIds = 4096;
        auto it = rep.pending_props.find(id);
        if (it != rep.pending_props.end()) {
            if (it->second.size() >= kMaxPerId) {
                ++st.prop_dropped;
                return false;
            }
            it->second.push_back({cls, prop});
        } else if (rep.pending_props.size() < kMaxIds) {
            rep.pending_props[id].push_back({cls, prop});
        } else {
            ++st.prop_dropped;
            return false;
        }
        ++st.prop_deferred;
        return true;
    }
    JoinRecord r;
    r.lo = 1;
    r.id = id;
    r.cls = cls;
    r.props.push_back(prop);
    r.plo = 0;
    r.parent = -1;
    std::vector<PendingRef> pending;
    applyRecordPass1(rep, schema, r, true, pending);
    resolvePendingRefs(rep, pending);
    ++st.prop_applied;
    // RBX_DIAG=1: a name can arrive as a property set on an instance created by an
    // earlier spawn message, so the record probe above never sees it. This is the
    // path the character Model's Name actually takes (measured 2026-11-05: the
    // native's inbound carries `0f 0c poopnest5002` inside a 0x83/0x03 batch).
    if (std::getenv("RBX_DIAG")) {
        const auto it = rep.by_id.find(id);
        if (it != rep.by_id.end() && it->second >= 0) {
            rbx::Instance* inst = rep.instances[size_t(it->second)].get();
            if (inst && !inst->name().empty() &&
                inst->name() == rbx::local_user_name())
            {
                // Raw bytes, because JoinProp stores the wire value untyped:
                // for Character this is the instance ref the server expects us
                // to already hold.
                std::string extra;
                if (prop.name == "Character") {
                    static const char* kH = "0123456789abcdef";
                    extra = " char_tag=" + std::to_string(unsigned(prop.tag)) +
                            " char_bytes=";
                    for (uint8_t b : prop.value) {
                        extra.push_back(kH[b >> 4]);
                        extra.push_back(kH[b & 15]);
                    }
                }
                std::fprintf(stderr,
                             "[diag] prop self name='%s' class=%s ref=%u "
                             "prop=%s%s\n",
                             inst->name().c_str(), inst->class_name().c_str(),
                             unsigned(id), prop.name.c_str(), extra.c_str());
            }
        }
    }
    return true;
}

bool fireStreamEvent(ApplyReport& rep, const JoinSchema& schema,
                     const EventItem& ev, lua_State* L, StreamStats& st) {
    if (!rep.root || !L)
        return false;
    const auto it = rep.by_id.find(ev.entity);
    if (it == rep.by_id.end() || it->second < 0)
        return false;
    size_t idx = 0;
    for (size_t ci = 0; ci < schema.classes.size(); ++ci) {
        const auto& evs = schema.classes[ci].events;
        if (size_t(ev.event) >= idx && size_t(ev.event) < idx + evs.size()) {
            rbx::Instance* inst = rep.instances[size_t(it->second)].get();
            // Decode every argument straight onto L's stack (tables have no
            // Variant alternative). Argument positions are preserved: an
            // unsupported shape becomes nil rather than shifting later args.
            const int base = lua_gettop(L);
            int nargs = 0;
            for (const EventArg& a : ev.args) {
                push_event_arg(L, a, schema, rep, 0);
                ++nargs;
            }
            inst->event_signal(evs[size_t(ev.event) - idx].name)
                ->fire_lua(L, nargs);
            lua_settop(L, base);
            ++st.events_fired;
            return true;
        }
        idx += evs.size();
    }
    return false;
}

} // namespace rbxclient
