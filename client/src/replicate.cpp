// replicate.cpp — pure-function replication frame parsers (see replicate.h
// for provenance). Conventions mirror blob.cpp: strict bounds checks,
// false + *err on any violation, byte-exact against frozen captures.
#include "replicate.h"

#include "util.h"

#include <cstring>

#include <zstd.h>

namespace rbxclient {

namespace {

bool fail(std::string* err, const std::string& e) {
    if (err)
        *err = e;
    return false;
}

// Unsigned LEB128 at [p, end). Returns bytes consumed, 0 on truncation.
size_t leb128(const uint8_t* p, const uint8_t* end, uint64_t* v) {
    uint64_t val = 0;
    int shift = 0;
    size_t i = 0;
    while (p + i < end) {
        const uint8_t b = p[i++];
        val |= uint64_t(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80)) {
            *v = val;
            return i;
        }
        if (shift >= 64)
            return 0;
    }
    return 0;
}

uint32_t u32le(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

uint16_t u16le(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

uint64_t u64le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | p[i];
    return v;
}

} // namespace

// ---- JoinData schema + records ---------------------------------------------
namespace {

// Bounds-checked cursor over [base, base + len).
struct Cursor {
    const uint8_t* p;
    size_t left;
    size_t total;
    Cursor(const uint8_t* d, size_t n) : p(d), left(n), total(n) {}
    size_t off() const {
        return total - left;
    }
    bool take(size_t n, const uint8_t** out, std::string* err,
              const char* what) {
        if (left < n) {
            if (err)
                *err = std::string("truncated ") + what + " at offset " +
                       std::to_string(off());
            return false;
        }
        *out = p;
        p += n;
        left -= n;
        return true;
    }
    bool u8(uint8_t* v, std::string* err, const char* what) {
        const uint8_t* b = nullptr;
        if (!take(1, &b, err, what))
            return false;
        *v = b[0];
        return true;
    }
    bool varint(uint64_t* v, std::string* err, const char* what) {
        uint64_t val = 0;
        int shift = 0;
        while (true) {
            if (left == 0) {
                if (err)
                    *err = std::string("truncated varint (") + what +
                           ") at offset " + std::to_string(off());
                return false;
            }
            const uint8_t b = *p++;
            --left;
            val |= uint64_t(b & 0x7F) << shift;
            shift += 7;
            if (!(b & 0x80)) {
                *v = val;
                return true;
            }
            if (shift >= 64) {
                if (err)
                    *err = "varint too long";
                return false;
            }
        }
    }
    bool bytes(std::string& s, std::string* err, const char* what) {
        uint64_t n = 0;
        if (!varint(&n, err, what))
            return false;
        if (n > left) {
            if (err)
                *err = std::string("string overruns (") + what + ") at offset " +
                       std::to_string(off());
            return false;
        }
        s.assign(reinterpret_cast<const char*>(p), size_t(n));
        p += n;
        left -= n;
        return true;
    }
};

bool read_name_value(Cursor& c, std::vector<uint8_t>& raw, std::string* err) {
    // tag 49 (Name): u8 L; high-bit = shared-table index (low 7 bits;
    // 127 = varint+127 extended — never observed, rejected); L == 127 =
    // varint-len string; else L literal bytes. (sub_340A3E4 case 0x31.)
    const size_t start = c.off();
    uint8_t u = 0;
    if (!c.u8(&u, err, "name len"))
        return false;
    if (u & 0x80) {
        if ((u & 0x7F) == 127) {
            if (err)
                *err = "extended shared name at offset " + std::to_string(start);
            return false;
        }
        raw.assign(c.p - 1, c.p); // the index byte only
        return true;
    }
    if (u == 127) {
        uint64_t n = 0;
        if (!c.varint(&n, err, "name ext len"))
            return false;
        const uint8_t* b = nullptr;
        if (!c.take(size_t(n), &b, err, "name ext bytes"))
            return false;
        raw.assign(c.p - (c.off() - start), c.p);
        return true;
    }
    const uint8_t* b = nullptr;
    if (!c.take(u, &b, err, "name bytes"))
        return false;
    raw.assign(c.p - (c.off() - start), c.p);
    return true;
}

bool read_content_value(Cursor& c, std::vector<uint8_t>& raw, std::string* err) {
    // tag 61 (Content): [varint form]; 0 = empty, 1 = [varint len][bytes],
    // 2 = ref. Only form 1 occurs in the snapshot; anything else fails
    // loudly (sub_340A3E4 case 0x3D).
    const size_t start = c.off();
    uint64_t form = 0;
    if (!c.varint(&form, err, "content form"))
        return false;
    if (form == 0) {
        raw.assign(c.p - (c.off() - start), c.p);
        return true;
    }
    if (form == 1) {
        uint64_t n = 0;
        if (!c.varint(&n, err, "content len"))
            return false;
        const uint8_t* b = nullptr;
        if (!c.take(size_t(n), &b, err, "content bytes"))
            return false;
        raw.assign(c.p - (c.off() - start), c.p);
        return true;
    }
    if (form == 2) {
        uint64_t v = 0;
        if (!c.varint(&v, err, "content ref"))
            return false;
        if (v != 0) {
            const uint8_t* b = nullptr;
            if (!c.take(4, &b, err, "content ref id"))
                return false;
        }
        raw.assign(c.p - (c.off() - start), c.p);
        return true;
    }
    if (err)
        *err = "content form " + std::to_string(form) + " at offset " +
               std::to_string(start);
    return false;
}

// Consume one property value per schema tag; appends raw bytes to `raw`.
// Mirrors sub_340A3E4's wire reads case by case (see replicate.h).
bool read_tagged_value(Cursor& c, uint8_t tag, const std::string& ctx,
                       std::vector<uint8_t>& raw, std::string* err) {
    const size_t start = c.off();
    const uint8_t* b = nullptr;
    uint64_t v = 0;
    auto fixed = [&](size_t n, const char* what) -> bool {
        if (!c.take(n, &b, err, what))
            return false;
        raw.insert(raw.end(), b, b + n);
        return true;
    };
    switch (tag) {
    case 2:
    case 8: { // string / BinaryString: [varint len][bytes]
        uint64_t n = 0;
        if (!c.varint(&n, err, "string len"))
            return false;
        if (!c.take(size_t(n), &b, err, "string bytes"))
            return false;
        raw.insert(raw.end(), c.p - (c.off() - start), c.p);
        return true;
    }
    case 3: // zero-width value (no wire bytes)
        return true;
    case 6:
    case 46:
    case 47: // GUID / Tags / bytecode-hash: 16B
        return fixed(16, "16B value");
    case 7:
    case 34:
    case 44: // enum / int / int64: varint
        if (!c.varint(&v, err, "varint value"))
            return false;
        raw.insert(raw.end(), c.p - (c.off() - start), c.p);
        return true;
    case 9: // bool/u8
        return fixed(1, "u8 value");
    case 10: // zigzag varint
        if (!c.varint(&v, err, "zigzag value"))
            return false;
        raw.insert(raw.end(), c.p - (c.off() - start), c.p);
        return true;
    case 11:
    case 16:
    case 17: // f32/u32
        return fixed(4, "u32 value");
    case 12:
    case 48:
    case 55:
    case 62: // f64/u64
        return fixed(8, "u64 value");
    case 13:
    case 21:
    case 37: // 2x f32 (UDim scale+offset, Vector2, NumberRange)
        return fixed(8, "8B value");
    case 14:
    case 40:
    case 67: // 16B (UDim2 f32,s32,f32,s32 / Rect / 2x f32 + f64)
        return fixed(16, "16B value");
    case 15: // Ray: 2x Vector3
        return fixed(24, "ray value");
    case 18: // BrickColor: u16
        return fixed(2, "u16 value");
    case 19:
    case 22:
    case 36: // Vector3 / Color3: 12B
        return fixed(12, "12B value");
    case 20: // 3x u8
        return fixed(3, "3B value");
    case 66: // u8 + u16
        return fixed(3, "3B value");
    case 26: { // CFrame-A (unused in 2.739 schema; IDA-only shape)
        if (!c.take(12, &b, err, "cframe pos"))
            return false;
        uint8_t k = 0;
        if (!c.u8(&k, err, "cframe kind"))
            return false;
        if (k == 0 && !c.take(36, &b, err, "cframe matrix"))
            return false;
        raw.insert(raw.end(), c.p - (c.off() - start), c.p);
        return true;
    }
    case 27: { // CFrame-B: pos + kind + optional quantized rotation
        if (!c.take(12, &b, err, "cframe pos"))
            return false;
        uint8_t k = 0;
        if (!c.u8(&k, err, "cframe kind"))
            return false;
        if (k == 0 && !c.take(6, &b, err, "cframe quat"))
            return false;
        raw.insert(raw.end(), c.p - (c.off() - start), c.p);
        return true;
    }
    case 24: // 2x u16
        return fixed(4, "u16x2 value");
    case 25: // 3x u16 (Vector3int16)
        return fixed(6, "u16x3 value");
    case 28:
    case 69: { // ref: 336800E [varint][u32]; 0 = null (no u32)
        if (!c.varint(&v, err, "ref"))
            return false;
        if (v != 0 && !c.take(4, &b, err, "ref id"))
            return false;
        raw.insert(raw.end(), c.p - (c.off() - start), c.p);
        return true;
    }
    case 38: { // sequence: [u32 N][N x 20B keypoints]
        if (!c.take(4, &b, err, "sequence count"))
            return false;
        const uint32_t n = u32le(b);
        if (n > 0x14) {
            if (err)
                *err = "sequence too big at offset " + std::to_string(start);
            return false;
        }
        if (!c.take(size_t(n) * 20, &b, err, "sequence keys"))
            return false;
        raw.insert(raw.end(), c.p - (c.off() - start), c.p);
        return true;
    }
    case 39: // single 20B keypoint (time + Vector3 + envelope)
        return fixed(20, "keypoint value");
    case 41: { // PhysicalProperties: u8 + optional floats
        uint8_t f = 0;
        if (!c.u8(&f, err, "phys flags"))
            return false;
        if (f & 1) {
            if (!c.take(20, &b, err, "phys floats"))
                return false;
            if (f >= 2 && !c.take(4, &b, err, "phys extra"))
                return false;
        }
        raw.insert(raw.end(), c.p - (c.off() - start), c.p);
        return true;
    }
    case 49:
        return read_name_value(c, raw, err);
    case 51: { // optional CFrame: u8; high bit set adds rotation + pos
        uint8_t k = 0;
        if (!c.u8(&k, err, "pivot kind"))
            return false;
        if (k & 0x80) {
            if ((k & 0x7F) == 0 && !c.take(6, &b, err, "pivot quat"))
                return false;
            if (!c.take(12, &b, err, "pivot pos"))
                return false;
        }
        raw.insert(raw.end(), c.p - (c.off() - start), c.p);
        return true;
    }
    case 52: // 8B + 2x u32
        return fixed(16, "16B value");
    case 54: { // FontFace: u16 + u8 + 2x [u32 len][bytes]
        if (!c.take(2, &b, err, "font weight"))
            return false;
        uint8_t s = 0;
        if (!c.u8(&s, err, "font style"))
            return false;
        for (int i = 0; i < 2; ++i) {
            if (!c.take(4, &b, err, "font str len"))
                return false;
            if (!c.take(size_t(u32le(b)), &b, err, "font str"))
                return false;
        }
        raw.insert(raw.end(), c.p - (c.off() - start), c.p);
        return true;
    }
    case 61:
        return read_content_value(c, raw, err);
    case 65: // u8 + f32 + f64
        return fixed(13, "13B value");
    case 68: { // User: 4x varint (domain + ids, sub_3617B0E)
        uint64_t w = 0;
        for (int i = 0; i < 4; ++i) {
            if (!c.varint(&w, err, "user varint"))
                return false;
        }
        raw.insert(raw.end(), c.p - (c.off() - start), c.p);
        return true;
    }
    default:
        break;
    }
    if (err)
        *err = "unhandled value tag " + std::to_string(tag) + " (" + ctx +
               ") at offset " + std::to_string(start);
    return false;
}

} // namespace

bool walkReplFrames(const uint8_t* d, size_t n, std::vector<ReplMessage>& out,
                    std::string* err) {
    size_t off = 0;
    while (off < n) {
        uint64_t ln = 0;
        const size_t k = rbx::compactVarintDecode(d + off, n - off, &ln);
        if (k == 0)
            return fail(err, "bad varint");
        if (ln == 0 || off + k + ln > n)
            return fail(err, "bad frame length");
        ReplMessage m;
        m.id = d[off + k];
        m.body.assign(d + off + k, d + off + k + ln);
        out.push_back(std::move(m));
        off += k + ln;
    }
    return true;
}

bool parseJoinOuter(const uint8_t* body, size_t len,
                    std::vector<uint8_t>& inner, std::string* err) {
    if (len < 13 || body[0] != 0x97)
        return fail(err, "not a 0x97 frame");
    if (body[1] != 0xA3 || body[2] != 0xCF || body[3] != 0x00 ||
        body[4] != 0x00)
        return fail(err, "bad 0x97 magic");
    const uint32_t size = u32le(body + 5);
    static const uint8_t kZstdMagic[4] = {0x28, 0xB5, 0x2F, 0xFD};
    if (std::memcmp(body + 9, kZstdMagic, 4) != 0)
        return fail(err, "0x97 zstd magic missing");
    // Unlike 0x16 batches (whose comp length counts frame-only), the 0x97
    // frame runs to the end: no trailing byte (verified by decode).
    const size_t flen = len - 9;
    std::vector<uint8_t> out(size);
    const size_t r = ZSTD_decompress(out.data(), out.size(), body + 9, flen);
    if (ZSTD_isError(r))
        return fail(err, std::string("zstd: ") + ZSTD_getErrorName(r));
    if (r != size)
        return fail(err, "0x97 size mismatch");
    out.resize(r);
    inner = std::move(out);
    return true;
}

bool parsePeerInit(const uint8_t* body, size_t len, PeerInit& out,
                   std::string* err) {
    // Proven prefix is 7 flag bytes + LEB128 peer (both captures).
    if (len < 9 || body[0] != 0x81)
        return fail(err, "not a 0x81 frame");
    out.ena = body[1];
    out.x = body[2];
    out.isr_read = body[3];
    out.isr_write = body[4];
    out.isr_legacy = body[5];
    out.v53 = body[6];
    out.v58 = body[7];
    uint64_t peer = 0;
    const size_t k = leb128(body + 8, body + len, &peer);
    if (k == 0 || peer > 0xFFFFFFFFu)
        return fail(err, "bad peer varint");
    out.peer = uint32_t(peer);
    out.tail.assign(body + 8 + k, body + len);
    return true;
}

bool parseDeltaBatch(const uint8_t* body, size_t len,
                     std::vector<uint8_t>& inner, std::string* err) {
    if (len < 15 || body[0] != 0x83 || body[1] != 0x16)
        return fail(err, "not a 0x83/0x16 batch");
    static const uint8_t kZstdMagic[4] = {0x28, 0xB5, 0x2F, 0xFD};
    if (std::memcmp(body + 11, kZstdMagic, 4) != 0)
        return fail(err, "0x16 zstd magic missing");
    const uint32_t comp = u32le(body + 3);
    const uint32_t decomp = u32le(body + 7);
    // comp counts the frame only (trailing byte excluded).
    if (comp == 0 || 11 + comp > len)
        return fail(err, "0x16 comp out of range");
    std::vector<uint8_t> out(decomp);
    const size_t r =
        ZSTD_decompress(out.data(), out.size(), body + 11, comp);
    if (ZSTD_isError(r))
        return fail(err, std::string("zstd: ") + ZSTD_getErrorName(r));
    if (r != decomp)
        return fail(err, "0x16 decomp mismatch");
    out.resize(r);
    inner = std::move(out);
    return true;
}

bool parsePropUpdates(const uint8_t* body, size_t len,
                      std::vector<PropUpdate>& out, std::string* err) {
    // 19B records (autocorrelation period 19, validated across captures):
    // [01 90 23 00 00 2f 09][owner u32 LE][sep: 0x22 numeric / 0x13 ref]
    // [val u32 LE][x][01 07]. Owner/val endianness + x/sep semantics are
    // HYPOTHESIS. Records are extracted greedily; anything else (mixed
    // string fields, message tails) is left for the caller — never fatal.
    static const uint8_t kHead[7] = {0x01, 0x90, 0x23, 0x00, 0x00, 0x2F, 0x09};
    if (len < 2 || body[0] != 0x83 || body[1] != 0x07)
        return fail(err, "not a 0x83/0x07 message");
    size_t i = 2;
    while (i + 19 <= len) {
        if (std::memcmp(body + i, kHead, sizeof kHead) != 0) {
            ++i;
            continue;
        }
        if (body[i + 17] != 0x01 || body[i + 18] != 0x07) {
            ++i;
            continue;
        }
        PropUpdate u;
        u.owner = u32le(body + i + 7);
        u.val = u32le(body + i + 12);
        u.x = body[i + 16];
        out.push_back(u);
        i += 19;
    }
    return true;
}

bool parsePlayerRecs(const uint8_t* body, size_t len,
                     std::vector<PlayerRec>& out, size_t* skipped,
                     std::string* err) {
    if (len < 2 || body[0] != 0x83 || body[1] != 0x02)
        return fail(err, "not a 0x83/0x02 message");
    if (skipped)
        *skipped = 0;
    // Anchor scan: avatar URL carried as [u8 len][bytes]; the entity id
    // follows as 0x0c + u32 LE; the display name as 0x18 + u8 len.
    // (Validated on the live spawn experiment: url userId + entity +
    // "Chat GPT0" in one record.)
    size_t i = 2;
    while (i < len) {
        const uint8_t* h =
            (const uint8_t*)std::memchr(body + i, 'h', len - i);
        if (!h || h + 7 >= body + len ||
            std::memcmp(h, "https://", 8) != 0)
            break;
        // Length prefix is the byte before 'h' (0x48 == 72 verified).
        const uint8_t* lp = h - 1;
        if (lp < body + i || *lp > 200 || h + *lp > body + len) {
            i = size_t(h - body) + 1;
            if (skipped)
                ++*skipped;
            continue;
        }
        PlayerRec r;
        r.url.assign((const char*)h, *lp);
        const uint8_t* p = h + *lp;
        // entity: expect 0x0c + u32
        if (p + 5 < body + len && p[0] == 0x0C) {
            r.entity = u32le(p + 1);
            p += 5;
        } else {
            i = size_t(h - body) + 1;
            if (skipped)
                ++*skipped;
            continue;
        }
        // name: scan forward for 0x18 + len + printable run (bounded).
        r.name.clear();
        for (const uint8_t* q = p; q + 2 < body + len && q < p + 64; ++q) {
            if (q[0] == 0x18 && q[1] > 0 && q[1] <= 32 &&
                q + 2 + q[1] <= body + len) {
                bool ok = true;
                for (uint8_t k = 0; k < q[1]; ++k) {
                    const uint8_t c = q[2 + k];
                    if (c < 32 || c >= 127) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    r.name.assign((const char*)q + 2, q[1]);
                    p = q + 2 + q[1];
                    break;
                }
            }
        }
        out.push_back(std::move(r));
        i = size_t(p - body);
        if (i <= size_t(h - body))
            i = size_t(h - body) + 1; // always advance
    }
    return true;
}

bool parseJoinSchema(const uint8_t* d, size_t n, JoinSchema& out,
                     std::string* err) {
    Cursor c(d, n);
    uint64_t v = 0;
    if (!c.varint(&v, err, "schema version"))
        return false;
    out.version = uint8_t(v);
    const uint8_t* b = nullptr;
    if (!c.take(8, &b, err, "schema flags"))
        return false;
    out.flags = u64le(b);
    uint64_t nenum = 0;
    if (!c.varint(&nenum, err, "enum count"))
        return false;
    out.enums.clear();
    for (uint64_t i = 0; i < nenum; ++i) {
        std::string name;
        if (!c.bytes(name, err, "enum name"))
            return false;
        uint8_t trailer = 0;
        if (!c.u8(&trailer, err, "enum trailer"))
            return false;
        if (trailer != 0x07)
            return fail(err, "enum trailer is not 0x07 at offset " +
                                 std::to_string(c.off() - 1));
        out.enums.push_back(std::move(name));
    }
    uint64_t nclass = 0;
    if (!c.varint(&nclass, err, "class count"))
        return false;
    // Preallocation bounds (frozen values pinned by tests, never assumed).
    if (!c.varint(&out.prop_bound, err, "prop bound"))
        return false;
    if (!c.varint(&out.ev_bound, err, "event bound"))
        return false;
    out.classes.clear();
    for (uint64_t i = 0; i < nclass; ++i) {
        SchemaClass cls;
        if (!c.bytes(cls.name, err, "class name"))
            return false;
        uint64_t nmem = 0;
        if (!c.varint(&nmem, err, "member count"))
            return false;
        for (uint64_t k = 0; k < nmem; ++k) {
            SchemaMember m;
            if (!c.bytes(m.name, err, "member name"))
                return false;
            if (!c.u8(&m.tag, err, "member tag"))
                return false;
            if (m.tag == 60) {
                if (!c.varint(&m.extra, err, "ref extra"))
                    return false;
                m.has_extra = true;
            } else if (m.tag == 7) {
                if (!c.take(2, &b, err, "enum extra"))
                    return false;
                m.extra = u16le(b);
                m.has_extra = true;
            }
            cls.members.push_back(std::move(m));
        }
        if (!c.take(2, &b, err, "class flags"))
            return false;
        uint64_t nev = 0;
        if (!c.varint(&nev, err, "event count"))
            return false;
        for (uint64_t k = 0; k < nev; ++k) {
            std::string ev;
            if (!c.bytes(ev, err, "event name"))
                return false;
            uint8_t np = 0;
            if (!c.u8(&np, err, "event params"))
                return false;
            if (!c.take(size_t(np) * 3, &b, err, "event param"))
                return false;
        }
        out.classes.push_back(std::move(cls));
    }
    // URL-prefix table + shared-Name table: bare [varint len][bytes].
    uint64_t nurl = 0;
    if (!c.varint(&nurl, err, "url table count"))
        return false;
    out.urls.clear();
    for (uint64_t i = 0; i < nurl; ++i) {
        std::string s;
        if (!c.bytes(s, err, "url prefix"))
            return false;
        out.urls.push_back(std::move(s));
    }
    uint64_t nshared = 0;
    if (!c.varint(&nshared, err, "shared table count"))
        return false;
    out.shared.clear();
    for (uint64_t i = 0; i < nshared; ++i) {
        std::string s;
        if (!c.bytes(s, err, "shared string"))
            return false;
        out.shared.push_back(std::move(s));
    }
    out.tail_off = c.off();
    return true;
}

bool parseJoinRecord(const uint8_t* d, size_t n, const JoinSchema& schema,
                     JoinRecord& out, size_t* consumed, std::string* err) {
    Cursor c(d, n);
    const size_t start = 0;
    uint64_t lo = 0;
    if (!c.varint(&lo, err, "record lo"))
        return false;
    out.lo = lo;
    if (lo == 0) {
        out.id = uint32_t(-1); // sentinel: no u32 follows (sub_336800E)
    } else {
        const uint8_t* b = nullptr;
        if (!c.take(4, &b, err, "record id"))
            return false;
        out.id = u32le(b);
    }
    const uint8_t* b = nullptr;
    if (!c.take(2, &b, err, "record class"))
        return false;
    out.cls = u16le(b);
    if (size_t(out.cls) >= schema.classes.size())
        return fail(err, "record class " + std::to_string(out.cls) +
                             " out of range at offset " + std::to_string(start));
    const std::vector<SchemaMember>& members =
        schema.classes[out.cls].members;
    out.props.clear();
    while (true) {
        uint8_t delta = 0;
        if (!c.u8(&delta, err, "prop delta"))
            return false;
        if (delta == 0xFF)
            break;
        if (size_t(delta) >= members.size())
            return fail(err, "prop delta " + std::to_string(delta) +
                                 " out of range for class " +
                                 schema.classes[out.cls].name + " at offset " +
                                 std::to_string(c.off() - 1));
        const SchemaMember& m = members[delta];
        JoinProp p;
        p.name = m.name;
        p.tag = m.tag;
        std::string ctx = schema.classes[out.cls].name + "." + m.name;
        if (!read_tagged_value(c, m.tag, ctx, p.value, err))
            return false;
        out.props.push_back(std::move(p));
    }
    uint64_t plo = 0;
    if (!c.varint(&plo, err, "record plo"))
        return false;
    out.plo = plo;
    if (plo == 0) {
        out.parent = -1; // sentinel: graft to root (sub_336800E)
    } else {
        if (!c.take(4, &b, err, "record parent"))
            return false;
        out.parent = int64_t(u32le(b));
    }
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseJoinRecords(const uint8_t* d, size_t n, const JoinSchema& schema,
                      std::vector<JoinRecord>& out, std::string* err) {
    size_t off = 0;
    while (off < n) {
        JoinRecord r;
        size_t used = 0;
        if (!parseJoinRecord(d + off, n - off, schema, r, &used, err)) {
            if (err)
                *err = "record " + std::to_string(out.size()) + " at offset " +
                       std::to_string(off) + ": " + *err;
            return false;
        }
        if (used == 0)
            return fail(err, "record consumed zero bytes");
        out.push_back(std::move(r));
        off += used;
    }
    return true;
}

} // namespace rbxclient
