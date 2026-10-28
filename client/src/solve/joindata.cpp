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
#include "instance/variant.h"

#include <cmath>
#include <cctype>
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
    rep.by_id[r.id] = -1;
    return;
}
if (exists) {
    index = size_t(known->second);
    inst = rep.instances[index].get();
} else {
    rbx::Ref<rbx::Instance> fresh = rbx::instantiate(cls);
    index = rep.instances.size();
    rep.instances.push_back(fresh);
    rep.by_id[r.id] = int(index);
    inst = rep.instances[index].get();
    ++rep.stats.spawned;
}
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
    if (!decl) {
        ++rep.stats.props_skipped_undeclared;
        ++rep.stats.skip_names[key];
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
    std::string set_err;
    if (!inst->set_prop(used, var, &set_err))
        ++rep.stats.props_skipped_typemismatch;
    else
        ++rep.stats.props_set;
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
        if (records[i].parent >= 0)
            parent = lookupInstance(rep, uint32_t(records[i].parent));
        if (!parent) {
            parent = rep.root.get();
            ++rep.stats.grafted_to_root;
        }
        std::string e2;
        if (!inst->reparent(parent, &e2))
            ++rep.stats.grafted_to_root;
    }
    return rep;
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
    if (parent >= 0)
        par = lookupInstance(rep, uint32_t(parent));
    if (!par) {
        par = rep.root.get();
        ++st.stream_grafted;
    }
    std::string e2;
    if (!inst->reparent(par, &e2))
        ++st.stream_grafted;
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

bool applyStreamProp(ApplyReport& rep, const JoinSchema& schema, uint32_t id,
                     uint16_t cls, const JoinProp& prop, StreamStats& st) {
    if (!rep.root || prop.name.empty())
        return false;
    const auto known = rep.by_id.find(id);
    if (known == rep.by_id.end() || known->second < 0)
        return false;
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
            inst->event_signal(evs[size_t(ev.event) - idx].name)->fire(L);
            ++st.events_fired;
            return true;
        }
        idx += evs.size();
    }
    return false;
}

} // namespace rbxclient
