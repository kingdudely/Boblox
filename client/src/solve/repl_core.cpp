// repl_core.cpp — value codec: name/content/tuple/tagged readers + ref primitives (see replicate.h for provenance).
//
// Pure-function parsers, no I/O, no state (fuzz-friendly). Split verbatim
// from replicate.cpp (recv v4); no behavior change.
#include "solve/replicate.h"

#include "solve/repl_internal.h"

#include <cstdio>
#include <cstdlib>

namespace rbxclient {
namespace detail {

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
    // Errors carry the member context: "truncated 12B value" alone cannot be
    // traced to a class/member from a live log, and a wire-shape mismatch is
    // only actionable when the member is named.
    auto fixed = [&](size_t n, const char* what) -> bool {
        const std::string w = std::string(what) + " (" + ctx + ")";
        if (!c.take(n, &b, err, w.c_str()))
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
        if (v != 0) {
            const std::string w = "ref id (" + ctx + ")";
            if (!c.take(4, &b, err, w.c_str()))
                return false;
        }
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
    case 35: { // NumberSequence (sub_3614E86): [u32 N][N x 12B keypoints]
        // Each keypoint is 3x f32 (time/value/envelope, HYPOTHESIS on
        // field semantics — extents verified). N > 20 throws natively.
        if (!c.take(4, &b, err, "sequence count"))
            return false;
        const uint32_t n = u32le(b);
        if (n > 0x14) {
            if (err)
                *err = "Number sequence is too big at offset " +
                       std::to_string(start);
            return false;
        }
        if (!c.take(size_t(n) * 12, &b, err, "sequence keys"))
            return false;
        raw.insert(raw.end(), c.p - (c.off() - start), c.p);
        return true;
    }
    case 58: { // MeshScale physical/controller blob (sub_36172DA): 7B header
        // + [u8 C][C bytes]. C > 50 throws natively ("too many"). When
        // header bit0 is set the native Huffman-expands the C bytes
        // in-memory (dict 3616A14) — the wire extent is still C bytes.
        if (!c.take(7, &b, err, "meshscale header"))
            return false;
        uint8_t cc = 0;
        if (!c.u8(&cc, err, "meshscale count"))
            return false;
        if (cc >= 51) {
            if (err)
                *err = "too many control values at offset " +
                       std::to_string(start);
            return false;
        }
        if (!c.take(cc, &b, err, "meshscale values"))
            return false;
        raw.insert(raw.end(), c.p - (c.off() - start), c.p);
        return true;
    }
    case 60: { // NetAssetId / LR handle (sub_3409C02 + 254E814): modern wire
        // [varint schemaIdx]([8B id][varint v] iff idx != 0xFFFF). Legacy
        // wire is [8B id][varint v][8B schema-source] (17B, same extent as
        // the shape below) — the modern reader is what live traffic shows.
        uint64_t idx = 0;
        if (!c.varint(&idx, err, "netasset schema"))
            return false;
        if (idx > 0xFFFF) {
            if (err)
                *err = "netasset schema out of range at offset " +
                       std::to_string(start);
            return false;
        }
        if (idx != 0xFFFF) {
            if (!c.take(8, &b, err, "netasset id"))
                return false;
            uint64_t vv = 0;
            if (!c.varint(&vv, err, "netasset v"))
                return false;
            // Fixed 7-byte tail (empirical): a live UnionOperation spawn
            // carries 17 bytes here, and 17 is the only length that keeps the
            // record's member deltas strictly increasing (every one of the
            // 641 preceding records in the same batch is strictly
            // increasing). Both unions in that batch (identical 3x9x0.6
            // slabs sharing one mesh) carry the same 17 bytes.
            if (!c.take(7, &b, err, "netasset tail"))
                return false;
        }
        raw.insert(raw.end(), c.p - (c.off() - start), c.p);
        return true;
    }
    case 57:
        // BasePart.ReplicationPV: a RobloxSecurity / hidden / non-scriptable
        // physics struct. Its wire shape is still not modelled, and the old
        // note here ("client->server-only") was wrong — it arrives on every
        // 0xA1 tick. The 0xA1 reader handles it by extent: when it is the
        // record's last selected member the body length says where it ends, so
        // the record closes exactly; otherwise the members before it are kept
        // and the record is partial. Other paths (0x03/0x16) have no per-record
        // extent to lean on, so they still refuse it here.
        if (err)
            *err = "tag 57 (" + ctx +
                   "): ReplicationPV shape not modelled (handled by extent in "
                   "the 0xA1 reader)";
        return false;
    default:
        break;
    }
    if (err)
        *err = "unhandled value tag " + std::to_string(tag) + " (" + ctx +
               ") at offset " + std::to_string(start);
    return false;
}

// One 3617A08 triplet: [u8 flag]([3x u32] iff flag != 0, else [3x u8]).
bool read_triplet(Cursor& c, ItemTriplet& out, std::string* err,
                  const char* what) {
    std::string flag_what = std::string(what) + " flag";
    if (!c.u8(&out.flag, err, flag_what.c_str()))
        return false;
    out.wide = out.flag != 0;
    const uint8_t* b = nullptr;
    if (out.wide) {
        std::string v_what = std::string(what) + " u32s";
        if (!c.take(12, &b, err, v_what.c_str()))
            return false;
        for (int i = 0; i < 3; ++i)
            out.v[i] = u32le(b + 4 * i);
    } else {
        std::string v_what = std::string(what) + " u8s";
        if (!c.take(3, &b, err, v_what.c_str()))
            return false;
        for (int i = 0; i < 3; ++i)
            out.v[i] = b[i];
    }
    return true;
}

// Entity/parent reference: [varint lo][u32 id], lo == 0 = sentinel with
// no u32 following (sub_336800E; id reported as -1, i.e. null/graft).
//
// The lo field is a 4-byte varint, not an unbounded LEB128: live 0x02
// spawns carry lo e2 92 ed e8 where the fourth byte's high bit is data, not
// continuation. Reading it as an unbounded varint consumes be e4 40 from the
// following u32 and desyncs the next chain item by 3 bytes (the LocalScript
// spawn + bulk case, /tmp/stopdump).
static bool read_ref_lo(Cursor& c, uint64_t& lo, std::string* err,
                        const char* what) {
    uint64_t v = 0;
    for (int i = 0; i < 4; ++i) {
        uint8_t b = 0;
        if (!c.u8(&b, err, what))
            return false;
        v |= uint64_t(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) {
            lo = v;
            return true;
        }
    }
    // Fourth byte terminates the 4-byte form even when its high bit is set.
    lo = v;
    return true;
}

bool read_ref(Cursor& c, uint64_t& lo, int64_t& id, std::string* err,
              const char* what) {
    std::string lo_what = std::string(what) + " lo";
    if (!read_ref_lo(c, lo, err, lo_what.c_str()))
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

// One 336800E guid (entity/blob id): [varint V]([u32 hi] iff V != 0 and
// V != 0xFFFFFFFF). Both V == 0 and V == 0xFFFFFFFF are null (no hi
// bytes); anything else carries a 4B LE hi. (read_ref differs: it reads
// hi for every nonzero lo, so guids need this exact variant.)
bool read_guid(Cursor& c, uint64_t& lo, int64_t& id, std::string* err,
               const char* what) {
    std::string lo_what = std::string(what) + " lo";
    if (!c.varint(&lo, err, lo_what.c_str()))
        return false;
    if (lo == 0 || lo == 0xFFFFFFFFu) {
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

} // namespace detail
} // namespace rbxclient
