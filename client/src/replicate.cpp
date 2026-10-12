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

bool read_content_value(Cursor& c, bool prefix_mode,
                        std::vector<uint8_t>& raw, std::string* err) {
    // tag 61 (Content): [varint form]; 0 = empty, 1 = asset id (same two
    // shapes as tag 33, selected by the same flag), 2 = ref, 3 = complex
    // (unmapped, fails loudly). Only forms 0-2 occur in the snapshot.
    // (sub_340A3E4 case 0x3D; form 1 delegates to sub_3409332.)
    const size_t start = c.off();
    uint64_t form = 0;
    if (!c.varint(&form, err, "content form"))
        return false;
    if (form == 0) {
        raw.assign(c.p - (c.off() - start), c.p);
        return true;
    }
    if (form == 1) {
        if (!prefix_mode) {
            uint64_t n = 0;
            if (!c.varint(&n, err, "content len"))
                return false;
            const uint8_t* b = nullptr;
            if (!c.take(size_t(n), &b, err, "content bytes"))
                return false;
        } else {
            uint8_t pfx = 0;
            if (!c.u8(&pfx, err, "content prefix"))
                return false;
            if (pfx & 1) {
                uint64_t v = 0;
                if (!c.varint(&v, err, "content id"))
                    return false;
            } else {
                uint64_t n = 0;
                if (!c.varint(&n, err, "content suffix len"))
                    return false;
                const uint8_t* b = nullptr;
                if (!c.take(size_t(n), &b, err, "content suffix bytes"))
                    return false;
            }
        }
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
// prefix_mode selects tag 33's shape (sub_3409332's a4 flag): false =
// plain string [varint len][bytes] (snapshot records, 0x0d entries, spawn
// loop B); true = prefix-compressed [u8 p]([varint id] if p&1 else
// [varint len][bytes]) (event params, spawn loop A). The preshared prefix
// table itself is never needed for extents.
// depth is the container-nesting depth of the value itself (0 =
// top-level arg/prop); tuple-family elements recurse with depth+1.
bool read_tagged_value(Cursor& c, uint8_t tag, bool prefix_mode,
                       const std::string& ctx, std::vector<uint8_t>& raw,
                       std::string* err, int depth = 0);

// Tuple-family values (tags 29-32, sub_340A3E4 cases 0x1D-0x20 via the
// element decoder sub_340887C). Elements are self-describing on the wire:
//   29 Tuple:      [varint N][N x element] (top-level only: nested Tuple
//                  throws natively, "cannot be received as a member of
//                  another container")
//   30 ValueArray: [varint N][N x element] (nestable)
//   31 ValueTable: [varint N][N x [string key][element]]
//   32 ValueMap:   [varint N][N x [string key][element]]
// element = [u8 tag]([u16 index] iff tag==7)[payload(tag)] (tag==0 =
// empty, tag byte only). Native guards mirrored without runtime cvars:
// N <= remaining bytes (every element costs >= 1 byte), depth <= 32,
// Tuple only at depth 0. The value's raw extent covers everything.
bool read_tuple_value(Cursor& c, uint8_t tag, bool prefix_mode, int depth,
                      const std::string& ctx, EventArg& out,
                      std::string* err) {
    const size_t start = c.off();
    if (tag == 29 && depth > 0) {
        if (err)
            *err = "tuple tag 29 (" + ctx +
                   "): Tuple cannot be received as a member of another "
                   "container at offset " +
                   std::to_string(start);
        return false;
    }
    if (depth > 32) {
        if (err)
            *err = "tuple tag " + std::to_string(tag) + " (" + ctx +
                   "): container nesting too deep at offset " +
                   std::to_string(start);
        return false;
    }
    uint64_t n = 0;
    if (!c.varint(&n, err, "tuple count"))
        return false;
    // Every element costs at least its tag byte.
    if (n > c.left) {
        if (err)
            *err = "tuple count " + std::to_string(n) + " (" + ctx +
                   ") exceeds remaining bytes at offset " +
                   std::to_string(start);
        return false;
    }
    out.tag = tag;
    out.elements.clear();
    out.keys.clear();
    for (uint64_t i = 0; i < n; ++i) {
        if (tag == 31 || tag == 32) {
            uint64_t klen = 0;
            if (!c.varint(&klen, err, "tuple key len"))
                return false;
            const uint8_t* kb = nullptr;
            if (!c.take(size_t(klen), &kb, err, "tuple key bytes"))
                return false;
            out.keys.emplace_back(reinterpret_cast<const char*>(kb),
                                  size_t(klen));
        }
        EventArg el;
        uint8_t t = 0;
        if (!c.u8(&t, err, "tuple elem tag"))
            return false;
        el.tag = t;
        if (t == 0) {
            // Empty: tag byte only (sub_340887C early return).
        } else {
            if (t == 7) {
                const uint8_t* xb = nullptr;
                if (!c.take(2, &xb, err, "tuple elem enum index"))
                    return false;
                el.extra = uint16_t(xb[0] | (uint16_t(xb[1]) << 8));
            }
            if (!read_tagged_value(c, t, prefix_mode, ctx, el.value, err,
                                   depth + 1))
                return false;
        }
        out.elements.push_back(std::move(el));
    }
    out.value.assign(c.p - (c.off() - start), c.p);
    return true;
}

bool read_tagged_value(Cursor& c, uint8_t tag, bool prefix_mode,
                       const std::string& ctx, std::vector<uint8_t>& raw,
                       std::string* err, int depth) {
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
        return read_content_value(c, prefix_mode, raw, err);
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
    case 56: { // compressed bulk blob (sub_340989C): [varint comp]
               // [varint decomp][comp bytes]. The payload is carried raw
               // (HeapProfilerService.OnNewData is the only schema user).
        uint64_t comp = 0, decomp = 0;
        if (!c.varint(&comp, err, "bulk comp len"))
            return false;
        if (!c.varint(&decomp, err, "bulk decomp len"))
            return false;
        (void)decomp;
        if (!c.take(size_t(comp), &b, err, "bulk bytes"))
            return false;
        raw.insert(raw.end(), c.p - (c.off() - start), c.p);
        return true;
    }
    case 29:
    case 30:
    case 31:
    case 32: {
        // Tuple-family: self-describing elements (sub_340887C); no table.
        // Structured parse is discarded here (prop loops keep raw bytes);
        // event params use parseEventItem's structured path below.
        EventArg tmp;
        if (!read_tuple_value(c, tag, prefix_mode, depth, ctx, tmp, err))
            return false;
        raw.insert(raw.end(), tmp.value.begin(), tmp.value.end());
        return true;
    }
    case 33: { // prefix-compressed asset id (sub_3409332) or plain string
        if (!prefix_mode) { // snapshot/entries/loop-B: [varint][bytes]
            uint64_t n = 0;
            if (!c.varint(&n, err, "asset string len"))
                return false;
            if (!c.take(size_t(n), &b, err, "asset string bytes"))
                return false;
            raw.insert(raw.end(), c.p - (c.off() - start), c.p);
            return true;
        }
        // events/loop-A: [u8 p]([varint id] if p&1 else [varint][bytes]).
        // Only extents matter (prefix table is preshared, never on wire).
        uint8_t pfx = 0;
        if (!c.u8(&pfx, err, "asset prefix"))
            return false;
        if (pfx & 1) {
            if (!c.varint(&v, err, "asset id"))
                return false;
        } else {
            uint64_t n = 0;
            if (!c.varint(&n, err, "asset suffix len"))
                return false;
            if (!c.take(size_t(n), &b, err, "asset suffix bytes"))
                return false;
        }
        raw.insert(raw.end(), c.p - (c.off() - start), c.p);
        return true;
    }
    case 57:
        if (err)
            *err = "tag 57 (" + ctx +
                   "): flag-variable frustum shape (sub_3615946), "
                   "client->server-only — never sent server->client";
        return false;
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
            SchemaEvent ev;
            if (!c.bytes(ev.name, err, "event name"))
                return false;
            uint8_t np = 0;
            if (!c.u8(&np, err, "event params"))
                return false;
            for (uint8_t j = 0; j < np; ++j) {
                if (!c.take(3, &b, err, "event param"))
                    return false;
                ev.params.push_back({b[0], b[1], b[2]});
            }
            cls.events.push_back(std::move(ev));
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

// Entity/parent reference: [varint lo][u32 id], lo == 0 = sentinel with
// no u32 following (sub_336800E; id reported as -1, i.e. null/graft).
bool read_ref(Cursor& c, uint64_t& lo, int64_t& id, std::string* err,
              const char* what) {
    std::string lo_what = std::string(what) + " lo";
    if (!c.varint(&lo, err, lo_what.c_str()))
        return false;
    if (lo == 0) {
        id = -1;
        return true;
    }
    const uint8_t* b = nullptr;
    std::string id_what = std::string(what) + " id";
    if (!c.take(4, &b, err, id_what.c_str()))
        return false;
    id = int64_t(u32le(b));
    return true;
}

// One ([u8 delta][value])* [0xFF] property loop (sub_3493C2E). Deltas are
// absolute member indexes (validated EOF-exact on all 2911 snapshot
// records; the native's relative id-walk absorbs its gappy global table
// onto the same members — see replicate.h).
bool read_prop_loop(Cursor& c, const std::vector<SchemaMember>& members,
                    const std::string& cls_name, bool prefix_mode,
                    std::vector<JoinProp>& out, std::string* err) {
    while (true) {
        uint8_t delta = 0;
        if (!c.u8(&delta, err, "prop delta"))
            return false;
        if (delta == 0xFF)
            break;
        if (size_t(delta) >= members.size())
            return fail(err, "prop delta " + std::to_string(delta) +
                                 " out of range for class " + cls_name +
                                 " at offset " + std::to_string(c.off() - 1));
        const SchemaMember& m = members[delta];
        JoinProp p;
        p.name = m.name;
        p.tag = m.tag;
        std::string ctx = cls_name + "." + m.name;
        if (!read_tagged_value(c, m.tag, prefix_mode, ctx, p.value, err))
            return false;
        out.push_back(std::move(p));
    }
    return true;
}

bool parseJoinRecord(const uint8_t* d, size_t n, const JoinSchema& schema,
                     JoinRecord& out, size_t* consumed, std::string* err) {
    Cursor c(d, n);
    const size_t start = 0;
    int64_t rid = -1;
    if (!read_ref(c, out.lo, rid, err, "record"))
        return false;
    out.id = uint32_t(rid); // (uint32_t)-1 on the lo == 0 sentinel
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
    if (!read_prop_loop(c, members, schema.classes[out.cls].name, false,
                        out.props, err))
        return false;
    if (!read_ref(c, out.plo, out.parent, err, "record parent"))
        return false;
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

bool parseSpawnItem(const uint8_t* d, size_t n, const JoinSchema& schema,
                    bool has_pad, SpawnRecord& out, size_t* consumed,
                    std::string* err) {
    Cursor c(d, n);
    int64_t rid = -1;
    if (!read_ref(c, out.lo, rid, err, "spawn"))
        return false;
    out.id = uint32_t(rid); // (uint32_t)-1 on the lo == 0 sentinel
    const uint8_t* b = nullptr;
    if (!c.take(2, &b, err, "spawn class"))
        return false;
    out.cls = u16le(b);
    if (size_t(out.cls) >= schema.classes.size())
        return fail(err, "spawn class " + std::to_string(out.cls) +
                             " out of range at offset 0");
    if (has_pad) {
        uint8_t pad = 0;
        if (!c.u8(&pad, err, "spawn pad"))
            return false;
    }
    const std::string& cls_name = schema.classes[out.cls].name;
    const std::vector<SchemaMember>& members =
        schema.classes[out.cls].members;
    out.props_a.clear();
    out.props_b.clear();
    if (!read_prop_loop(c, members, cls_name, true, out.props_a, err)) {
        if (err)
            *err = "spawn loop A: " + *err;
        return false;
    }
    if (!read_prop_loop(c, members, cls_name, false, out.props_b, err)) {
        if (err)
            *err = "spawn loop B: " + *err;
        return false;
    }
    if (!read_ref(c, out.plo, out.parent, err, "spawn parent"))
        return false;
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseEventItem(const uint8_t* d, size_t n, const JoinSchema& schema,
                    EventItem& out, size_t* consumed, std::string* err) {
    Cursor c(d, n);
    int64_t ent = -1;
    if (!read_ref(c, out.lo, ent, err, "event entity"))
        return false;
    out.entity = uint32_t(ent); // (uint32_t)-1 on the lo == 0 sentinel
    const uint8_t* b = nullptr;
    if (!c.take(2, &b, err, "event id"))
        return false;
    out.event = u16le(b);
    // Global event index over the concatenated per-class lists.
    size_t idx = 0;
    bool found = false;
    for (size_t ci = 0; ci < schema.classes.size() && !found; ++ci) {
        const std::vector<SchemaEvent>& evs = schema.classes[ci].events;
        if (size_t(out.event) >= idx && size_t(out.event) < idx + evs.size()) {
            out.cls_idx = ci;
            out.ev_idx = size_t(out.event) - idx;
            found = true;
        }
        idx += evs.size();
    }
    if (!found)
        return fail(err, "event id " + std::to_string(out.event) +
                             " out of range (" + std::to_string(idx) +
                             " total) at offset 0");
    const SchemaClass& cls = schema.classes[out.cls_idx];
    const SchemaEvent& ev = cls.events[out.ev_idx];
    out.args.clear();
    for (const auto& p : ev.params) {
        EventArg a;
        a.tag = p[0];
        a.extra = uint16_t(p[1] | (uint16_t(p[2]) << 8));
        std::string ctx = cls.name + "." + ev.name + " arg";
        if (a.tag >= 29 && a.tag <= 32) {
            if (!read_tuple_value(c, a.tag, true, 0, ctx, a, err))
                return false;
        } else {
            if (!read_tagged_value(c, a.tag, true, ctx, a.value, err))
                return false;
        }
        out.args.push_back(std::move(a));
    }
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseItem01(const uint8_t* d, size_t n, Item01& out, size_t* consumed,
                 std::string* err) {
    Cursor c(d, n);
    int64_t ent = -1;
    if (!read_ref(c, out.lo, ent, err, "item01 entity"))
        return false;
    out.entity = uint32_t(ent); // (uint32_t)-1 on the lo == 0 sentinel
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseItem0d(const uint8_t* d, size_t n, const JoinSchema& schema,
                 Item0d& out, size_t* consumed, std::string* err) {
    Cursor c(d, n);
    const uint8_t* b = nullptr;
    if (!c.take(4, &b, err, "0d count"))
        return false;
    out.entries.n = u32le(b);
    out.entries.records.clear();
    if (out.entries.n != 0) {
        // Entry frame: [u32 comp][u32 decomp][zstd -> N snapshot records]
        // (sub_34374B2 head; entries read snapshot-shape from the
        // decompressed buffer, short path only — a long-path
        // (RbxStorage blob) entry would fail the strict checks below).
        if (!c.take(4, &b, err, "0d comp"))
            return false;
        const uint32_t comp = u32le(b);
        if (!c.take(4, &b, err, "0d decomp"))
            return false;
        const uint32_t decomp = u32le(b);
        static const uint8_t kZstdMagic[4] = {0x28, 0xB5, 0x2F, 0xFD};
        const uint8_t* fr = nullptr;
        if (!c.take(comp, &fr, err, "0d frame"))
            return false;
        if (comp < 4 || std::memcmp(fr, kZstdMagic, 4) != 0)
            return fail(err, "0d zstd magic missing");
        std::vector<uint8_t> inner(decomp);
        const size_t r = ZSTD_decompress(inner.data(), inner.size(), fr, comp);
        if (ZSTD_isError(r))
            return fail(err, std::string("0d zstd: ") + ZSTD_getErrorName(r));
        if (r != decomp)
            return fail(err, "0d decomp mismatch");
        inner.resize(r);
        // Exactly N records (native entry loop runs N times; any trailing
        // buffer bytes are never read — carried opaque, counted).
        size_t off = 0;
        for (uint32_t k = 0; k < out.entries.n; ++k) {
            JoinRecord rec;
            size_t used = 0;
            if (!parseJoinRecord(inner.data() + off, inner.size() - off,
                                 schema, rec, &used, err)) {
                if (err)
                    *err = "0d entries: record " + std::to_string(k) +
                           " at offset " + std::to_string(off) + ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0d entries: record consumed zero bytes");
            out.entries.records.push_back(std::move(rec));
            off += used;
        }
        out.entries.trail.assign(inner.begin() + off, inner.end());
    }
    // Element run: [u8 flags][payload?]* while flags&8 (sub_353ECD2).
    out.elems.clear();
    while (true) {
        uint8_t flags = 0;
        if (!c.u8(&flags, err, "0d elem flags"))
            return false;
        Elem0d e;
        e.flags = flags;
        if ((flags & 0xF7) == 0) {
            const size_t start = c.off();
            uint8_t vf = 0;
            if (!c.u8(&vf, err, "0d elem valflag"))
                return false;
            if (!c.take(vf != 0 ? 12 : 3, &b, err, "0d elem value"))
                return false;
            e.payload.assign(c.p - (c.off() - start), c.p);
        }
        out.elems.push_back(std::move(e));
        if (!(flags & 8))
            break;
    }
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseItem1b(const uint8_t* d, size_t n, Item1b& out, size_t* consumed,
                 std::string* err) {
    Cursor c(d, n);
    const uint8_t* b = nullptr;
    if (!c.take(4, &b, err, "1b count"))
        return false;
    const uint32_t cnt = u32le(b);
    if (cnt > 1000000)
        return fail(err, "1b count out of range");
    out.refs.clear();
    for (uint32_t k = 0; k < cnt; ++k) {
        Item01 r;
        size_t used = 0;
        if (!parseItem01(c.p, c.left, r, &used, err)) {
            if (err)
                *err = "1b ref " + std::to_string(k) + ": " + *err;
            return false;
        }
        if (used == 0)
            return fail(err, "1b ref consumed zero bytes");
        c.p += used;
        c.left -= used;
        out.refs.push_back(r);
    }
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseItem05(const uint8_t* d, size_t n, bool st_bit11, bool st_bit15,
                 Item05& out, size_t* consumed, std::string* err) {
    Cursor c(d, n);
    const uint8_t* b = nullptr;
    if (!c.u8(&out.flags, err, "05 flags"))
        return false;
    if (!c.take(8, &b, err, "05 t"))
        return false;
    out.t = u64le(b);
    if (!c.take(4, &b, err, "05 a"))
        return false;
    out.a = u32le(b);
    if (!c.take(4, &b, err, "05 b"))
        return false;
    out.b = u32le(b);
    if (!c.take(4, &b, err, "05 c"))
        return false;
    out.c = u32le(b);
    if (!c.u8(&out.tail, err, "05 tail"))
        return false;
    if (out.tail > 1)
        return fail(err, "05 tail > 1 (item dropped natively)");
    out.has_x = st_bit11 && (out.flags & 4) != 0;
    if (out.has_x) {
        if (!c.take(4, &b, err, "05 x"))
            return false;
        out.x = u32le(b);
    }
    out.has_y = st_bit15 && (out.flags & 0x10) != 0;
    if (out.has_y) {
        if (!c.take(4, &b, err, "05 y"))
            return false;
        out.y = u32le(b);
    }
    if ((out.flags & 8) != 0)
        return fail(err, "05 flags&8 reads via unmapped sub_57D3718");
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseSharedStrings(const uint8_t* d, size_t n,
                        std::vector<SharedString>& out, std::string* err) {
    Cursor c(d, n);
    out.clear();
    const uint8_t* b = nullptr;
    while (c.left > 0) {
        SharedString s;
        if (!c.take(16, &b, err, "shared key"))
            return false;
        std::memcpy(s.key, b, 16);
        if (!c.u8(&s.marker, err, "shared marker"))
            return false;
        if (s.marker != 0 && s.marker != 1)
            return fail(err, "Unknown new shared string format marker at offset " +
                                 std::to_string(c.off() - 1));
        uint64_t len = 0;
        if (!c.varint(&len, err, "shared len"))
            return false;
        if (!c.take(size_t(len), &b, err, "shared bytes"))
            return false;
        s.value.assign(b, b + size_t(len));
        out.push_back(std::move(s));
    }
    return true;
}

bool parseBulkItem(const uint8_t* d, size_t n, BulkItem& out, size_t* consumed,
                   std::string* err) {
    Cursor c(d, n);
    const uint8_t* b = nullptr;
    if (!c.take(8, &b, err, "bulk key suffix"))
        return false;
    std::memcpy(out.suffix, b, 8);
    uint8_t marker = 0;
    if (!c.u8(&marker, err, "bulk marker"))
        return false;
    if (marker != 0)
        return fail(err, "bulk marker is not 0x00 at offset " +
                             std::to_string(c.off() - 1));
    uint64_t len = 0;
    if (!c.varint(&len, err, "bulk len"))
        return false;
    if (!c.take(size_t(len), &b, err, "bulk payload"))
        return false;
    out.payload.assign(b, b + size_t(len));
    // Anchor ref (same shape as every other ref): the script's true
    // parent after its content-keyed deferred parent.
    int64_t anchor = -1;
    if (!read_ref(c, out.anchor_lo, anchor, err, "bulk anchor"))
        return false;
    out.anchor = anchor;
    if (consumed)
        *consumed = c.off();
    return true;
}

PropTagTable defaultPropTags() {
    PropTagTable t;
    t.tag[248] = 68;   // 4x varint (only all-closing shape; native# is 61)
    t.tag[3796] = 61;  // Content (77B emote-table value closes exact)
    t.tag[4989] = 18;  // u16 const 0x0101 (tied 2B shapes; see 5469)
    t.tag[5018] = 61;  // Content (35B touch-text closes exact)
    t.tag[5469] = 18;  // u16 (constant 0x0101 = 257 in all 15 occurrences;
                       // extent-ambiguous with string/asset/Name 2B shapes
                       // {2,8,33,49} — BrickColor-index story is hypothesis)
    t.tag[5868] = 3;   // empty (zero-width value in all 8 occurrences)
    t.tag[5918] = 46;  // 16B guid (== native#; fixed-size class unique)
    t.tag[10145] = 25; // 6B s16x3 (== prop_bound; dense-range, not dense)
    return t;
}

bool parseItem10(const uint8_t* d, size_t n, Item10& out, size_t* consumed,
                 std::string* err) {
    Cursor c(d, n);
    const uint8_t* b = nullptr;
    if (!c.take(4, &b, err, "10 value"))
        return false;
    out.v = u32le(b);
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseItem20(const uint8_t* d, size_t n, Item20& out, size_t* consumed,
                 std::string* err) {
    Cursor c(d, n);
    const uint8_t* b = nullptr;
    if (!c.take(4, &b, err, "20 a"))
        return false;
    out.a = u32le(b);
    uint64_t len = 0;
    if (!c.take(4, &b, err, "20 len"))
        return false;
    len = u32le(b);
    if (!c.take(size_t(len), &b, err, "20 blob"))
        return false;
    out.blob.assign(b, b + size_t(len));
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parsePropTriple(const uint8_t* d, size_t n, const PropTagTable& table,
                     PropTriple& out, size_t* consumed, std::string* err) {
    Cursor c(d, n);
    int64_t rid = -1;
    if (!read_ref(c, out.lo, rid, err, "prop triple"))
        return false;
    out.id = uint32_t(rid); // (uint32_t)-1 on the lo == 0 sentinel
    const uint8_t* b = nullptr;
    if (!c.take(2, &b, err, "prop id"))
        return false;
    out.prop = uint16_t(b[0] | (uint16_t(b[1]) << 8));
    auto it = table.tag.find(out.prop);
    if (it == table.tag.end())
        return fail(err, "prop id " + std::to_string(out.prop) +
                             " not in tag table at offset 0");
    out.tag = it->second;
    // 0x03 values read snapshot-shape (prefix_mode=false): the 3404858 ->
    // 340A3E4 call's flag is settled by exact-close solving over live
    // chains (a wrong mode desyncs; see unit-replicate prop03 goldens).
    if (!read_tagged_value(c, out.tag, false, "prop", out.value, err))
        return false;
    if (consumed)
        *consumed = c.off();
    return true;
}

bool walk83ItemsEx(const uint8_t* body, size_t len, const JoinSchema& schema,
                   const PropTagTable* props, Walk83& out, std::string* err);

bool walk83Items(const uint8_t* body, size_t len, const JoinSchema& schema,
                 Walk83& out, std::string* err) {
    return walk83ItemsEx(body, len, schema, nullptr, out, err);
}

bool walk83ItemsEx(const uint8_t* body, size_t len, const JoinSchema& schema,
                   const PropTagTable* props, Walk83& out, std::string* err) {
    if (len < 2 || body[0] != 0x83)
        return fail(err, "not a 0x83 message");
    out.items.clear();
    out.complete = false;
    out.stop_type = 0;
    out.stop_off = 0;
    size_t i = 1;
    out.end_off = 0;
    while (i < len) {
        const uint8_t type = body[i++];
        if (type == 0x00) {
            out.complete = true;
            out.end_off = i; // one past the end marker
            break; // end marker (length framing also ends the walk)
        }
        ChainItem it;
        it.type = type;
        size_t used = 0;
        if (type == 0x23) { // clock tick: [u32] (sub_3511D26)
            if (len - i < 4)
                return fail(err, "truncated clock item at offset " +
                                     std::to_string(i));
            it.clock = u32le(body + i);
            i += 4;
        } else if (type == 0x02) { // streaming spawn (sub_34956D2)
            if (!parseSpawnItem(body + i, len - i, schema, false, it.spawn,
                                &used, err)) {
                if (err)
                    *err = "spawn item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "spawn item consumed zero bytes");
            i += used;
        } else if (type == 0x07) { // event invocation (sub_34822C6)
            if (!parseEventItem(body + i, len - i, schema, it.event, &used,
                                err)) {
                if (err)
                    *err = "event item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "event item consumed zero bytes");
            i += used;
        } else if (type == 0x01) { // entity ref (sub_3481460)
            if (!parseItem01(body + i, len - i, it.ref01, &used, err)) {
                if (err)
                    *err = "0x01 item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x01 item consumed zero bytes");
            i += used;
        } else if (type == 0x0d) { // delta entries + elements
            if (!parseItem0d(body + i, len - i, schema, it.delta0d, &used,
                             err)) {
                if (err)
                    *err = "0x0d item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x0d item consumed zero bytes");
            i += used;
        } else if (type == 0x1b) { // referent list (sub_34F07AE)
            if (!parseItem1b(body + i, len - i, it.refs1b, &used, err)) {
                if (err)
                    *err = "0x1b item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x1b item consumed zero bytes");
            i += used;
        } else if (type == 0x05) { // timing (sub_3486B9E; session bits)
            if (!parseItem05(body + i, len - i, false, true, it.timing05,
                             &used, err)) {
                if (err)
                    *err = "0x05 item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x05 item consumed zero bytes");
            i += used;
        } else if (type == 0x03) { // prop update (sub_3404858)
            if (!props)
                goto deferred;
            if (!parsePropTriple(body + i, len - i, *props, it.prop03, &used,
                                 err)) {
                if (err)
                    *err = "0x03 item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x03 item consumed zero bytes");
            i += used;
        } else if (type == 0x10) { // small u32 item (sub_34899B8)
            if (!parseItem10(body + i, len - i, it.v10, &used, err)) {
                if (err)
                    *err = "0x10 item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x10 item consumed zero bytes");
            i += used;
        } else if (type == 0x20) { // sized blob (sub_35C3A7E)
            if (!parseItem20(body + i, len - i, it.blob20, &used, err)) {
                if (err)
                    *err = "0x20 item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x20 item consumed zero bytes");
            i += used;
        } else {
        deferred:
            // Typeless script-data bulk fallback (content-addressed
            // LocalScript bytecode): only attempted at an otherwise-unknown
            // chain byte, never in place of a known item. A validated bulk
            // continues the chain; anything else stops pinned here.
            if (type != 0x00) {
                BulkItem bulk;
                size_t bulk_used = 0;
                std::string bulk_err;
                // Typeless: the unknown byte is key data, not a type, so
                // rewind one (i already advanced past it).
                if (parseBulkItem(body + i - 1, len - (i - 1), bulk,
                                  &bulk_used, &bulk_err) &&
                    bulk_used > 1) {
                    it.type = 0x78; // pseudo-type (no wire type byte)
                    it.bulk = std::move(bulk);
                    i += bulk_used - 1;
                    out.items.push_back(std::move(it));
                    continue;
                }
            }
            // Deferred-type item (0x03 prop updates need the runtime
            // prop-id table; anything else needs its own handler's
            // grammar), so the walk stops here with the stop
            // point pinned (coverage is measured, never silent).
            out.stop_type = type;
            out.stop_off = i - 1;
            return true;
        }
        out.items.push_back(std::move(it));
    }
    if (i >= len)
        out.complete = true; // clean EOF counts as complete (length-framed)
    if (out.complete && out.end_off == 0)
        out.end_off = i;
    return true;
}

} // namespace rbxclient
