// repl_stream.cpp — live streaming path: spawn/event/chain items, 0x03 table, walk83 (see replicate.h for provenance).
//
// Pure-function parsers, no I/O, no state (fuzz-friendly). Split verbatim
// from replicate.cpp (recv v4); no behavior change.
#include "replicate.h"

#include "repl_internal.h"
#include "proptable.inc"

#include "util.h"

#include <cstring>

#include <zstd.h>

namespace rbxclient {
using namespace detail;

bool walk83ItemsEx(const uint8_t* body, size_t len, const JoinSchema& schema,
                   const PropTagTable* props, Walk83& out, std::string* err);

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
        if (!checkDecompLen(decomp, err, "0d entries"))
            return false;
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
        // buffer bytes are never read — carried opaque, counted). Every
        // record consumes >= 1 byte, so N > decomp is structurally
        // impossible (fail fast instead of looping billions of times).
        if (out.entries.n > inner.size())
            return fail(err, "0d count exceeds decomp bytes");
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
    // Every ref costs >= 1 byte (minimal varint lo), so cnt > remaining
    // is structurally impossible (same principled bound as 0x0d/0x29).
    if (cnt > c.left)
        return fail(err, "1b count exceeds remaining bytes");
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
    // flags&1: six more u32s (three 69F921C + three 69F914C — both 4B LE,
    // so one 24B take bounds them). Else branch is the version-gated 3x
    // f32, whose gate is false in every session observed (Crossroads
    // flags=0x10 items close at 26B without it) — zero bytes here. If a
    // future session derails systematically on flags&1==0 0x05s, this is
    // the first place to look (see FINDINGS).
    out.has_extra = (out.flags & 1) != 0;
    if (out.has_extra) {
        if (!c.take(24, &b, err, "05 extra"))
            return false;
        for (int i = 0; i < 6; ++i)
            out.extra[i] = u32le(b + 4 * i);
    }
    if (!c.take(4, &b, err, "05 d"))
        return false;
    out.d = u32le(b);
    if (!c.take(4, &b, err, "05 e"))
        return false;
    out.e = u32le(b);
    if (!c.u8(&out.tail, err, "05 tail"))
        return false;
    // tail > 1: the native returns a null item WITHOUT throwing — the
    // chain continues past it. Consume exactly and flag dropped (callers
    // count, never fail).
    out.dropped = out.tail > 1;
    if (out.dropped) {
        if (consumed)
            *consumed = c.off();
        return true;
    }
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

bool parseItem04(const uint8_t* d, size_t n, Item04& out, size_t* consumed,
                 std::string* err) {
    // sub_3485DD4: a single u32 (4 bytes, stored verbatim).
    Cursor c(d, n);
    const uint8_t* b = nullptr;
    if (!c.take(4, &b, err, "04 value"))
        return false;
    out.v = u32le(b);
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseItem13(const uint8_t* d, size_t n, Item13& out, size_t* consumed,
                 std::string* err) {
    // sub_34908E4: two 336800E guids (entity refs).
    Cursor c(d, n);
    if (!read_guid(c, out.a.lo, out.a.id, err, "13 a"))
        return false;
    if (!read_guid(c, out.b.lo, out.b.id, err, "13 b"))
        return false;
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseItem15(const uint8_t* d, size_t n, Item15& out, size_t* consumed,
                 std::string* err) {
    // sub_3475046: [guid][string][string] (69F8CC2 strings: varint len +
    // bytes; no explicit cap documented — bounded by remaining bytes).
    Cursor c(d, n);
    if (!read_guid(c, out.ref.lo, out.ref.id, err, "15 ref"))
        return false;
    uint64_t len = 0;
    const uint8_t* b = nullptr;
    if (!c.varint(&len, err, "15 s1 len"))
        return false;
    if (!c.take(size_t(len), &b, err, "15 s1 bytes"))
        return false;
    out.s1.assign(reinterpret_cast<const char*>(b), size_t(len));
    if (!c.varint(&len, err, "15 s2 len"))
        return false;
    if (!c.take(size_t(len), &b, err, "15 s2 bytes"))
        return false;
    out.s2.assign(reinterpret_cast<const char*>(b), size_t(len));
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseItem29(const uint8_t* d, size_t n, Item29& out, size_t* consumed,
                 std::string* err) {
    // sub_3579BFE: [u32 N][N x [guid 336800E][u8 flag]]. N is capacity-
    // only for the native (loop runs N); every entry costs >= 2 bytes
    // (1B varint + 1B flag), so N > remaining is a structural violation.
    Cursor c(d, n);
    const uint8_t* b = nullptr;
    if (!c.take(4, &b, err, "29 count"))
        return false;
    const uint32_t cnt = u32le(b);
    if (cnt > c.left / 2)
        return fail(err, "29 count exceeds remaining bytes");
    out.entries.clear();
    for (uint32_t k = 0; k < cnt; ++k) {
        Item29Entry e;
        if (!read_guid(c, e.ref.lo, e.ref.id, err, "29 ref"))
            return false;
        if (!c.u8(&e.flag, err, "29 flag"))
            return false;
        out.entries.push_back(e);
    }
    if (consumed)
        *consumed = c.off();
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
    // Full native number map (10146 entries) with solver-proven wire
    // overrides baked in by tools/gen_proptable.py (see proptable.inc
    // header). Unobserved props read their native snapshot shape — the
    // best prior available; any skew drops its message loudly (counted),
    // exactly like the native's effective behavior (FINDINGS Recv v3).
    // Slots past the dumped range (rolling builds grow prop_bound) carry
    // sentinel 255 unless solver-overridden, and stay unmapped (loud on
    // first sight — the honest prior for unknown numbers).
    PropTagTable t;
    for (int i = 0; i < kPropTagCount; ++i) {
        if (kPropTags[i] == 255)
            continue;
        t.tag[uint16_t(i)] = kPropTags[i];
        if (kPropPrefix[i])
            t.prefix[uint16_t(i)] = true;
    }
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
    // Prefix shape is per-prop (see defaultPropTags provenance): the 0x03
    // call always passes flag=1, but several Content props provably read
    // the plain snapshot shape on the wire (exact closes).
    bool prefix_mode = false;
    auto itp = table.prefix.find(out.prop);
    if (itp != table.prefix.end())
        prefix_mode = itp->second;
    if (!read_tagged_value(c, out.tag, prefix_mode, "prop", out.value, err))
        return false;
    if (consumed)
        *consumed = c.off();
    return true;
}

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
        } else if (type == 0x04) { // u32 item (sub_3485DD4)
            if (!parseItem04(body + i, len - i, it.v04, &used, err)) {
                if (err)
                    *err = "0x04 item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x04 item consumed zero bytes");
            i += used;
        } else if (type == 0x06) { // timing (sub_3486B9E, same as 0x05)
            if (!parseItem05(body + i, len - i, false, true, it.timing05,
                             &used, err)) {
                if (err)
                    *err = "0x06 item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x06 item consumed zero bytes");
            i += used;
        } else if (type == 0x13) { // guid pair (sub_34908E4)
            if (!parseItem13(body + i, len - i, it.pair13, &used, err)) {
                if (err)
                    *err = "0x13 item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x13 item consumed zero bytes");
            i += used;
        } else if (type == 0x15) { // ref + strings (sub_3475046)
            if (!parseItem15(body + i, len - i, it.info15, &used, err)) {
                if (err)
                    *err = "0x15 item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x15 item consumed zero bytes");
            i += used;
        } else if (type == 0x29) { // ref-list (sub_3579BFE)
            if (!parseItem29(body + i, len - i, it.list29, &used, err)) {
                if (err)
                    *err = "0x29 item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x29 item consumed zero bytes");
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
