// repl_stream.cpp — live streaming path: spawn/event/chain items, 0x03 table, walk83 (see replicate.h for provenance).
//
// Pure-function parsers, no I/O, no state (fuzz-friendly). Split verbatim
// from replicate.cpp (recv v4); no behavior change.
#include "solve/replicate.h"

#include <cstdlib>

#include "solve/repl_internal.h"
#include "proptable.inc"

#include "common/util.h"

#include <cstring>

#include <zstd.h>

namespace rbxclient {
using namespace detail;

bool parseAppStateFrame(const uint8_t* d, size_t len, const JoinSchema& schema,
                        AppStateFrame* out, std::string* err) {
    if (!out || !parseAppStateHeader(d, len, &out->header)) {
        if (err) *err = "malformed 0xA1 envelope";
        return false;
    }
    out->records.clear();
    out->records.reserve(out->header.records);
    // Records are sequential; the body length is the u16 immediately after
    // the mask. Some records are followed by a 01 00 00 00 marker, some are
    // not, so accept either and fall back to the next valid record header.
    auto valid_header = [&](size_t q) {
        if (q + 19 > len)
            return false;
        if (d[q] != 1 && d[q] != 2 && d[q] != 4)
            return false;
        const uint16_t cls = u16le(d + q + 5);
        if (!schema.classes.empty() && cls >= schema.classes.size())
            return false;
        const uint16_t members = u16le(d + q + 7);
        const size_t words = (size_t(members) + 63) / 64;
        if (words == 0 || words > 1024)
            return false;
        const size_t ds = q + 17 + (words - 1) * 8;
        return ds + 2 <= len &&
               ds + 2 + size_t(u16le(d + ds)) <= len;
    };
    size_t p = out->header.body;
    for (uint32_t r = 0; r < out->header.records; ++r) {
        if (p + 19 > len) {
            if (err) *err = "truncated 0xA1 record " + std::to_string(r);
            return false;
        }
        Cursor c(d + p, len - p);
        AppStateRecord rec;
        int64_t id = -1;
        if (!read_ref(c, rec.lo, id, err, "A1 record ref"))
            return false;
        rec.id = uint32_t(id);
        const uint8_t* b = nullptr;
        if (!c.take(2, &b, err, "A1 record class"))
            return false;
        rec.cls = u16le(b);
        if (!schema.classes.empty() && rec.cls >= schema.classes.size()) {
            if (err) *err = "A1 class " + std::to_string(rec.cls) +
                            " out of range";
            return false;
        }
        if (!c.take(2, &b, err, "A1 member count"))
            return false;
        rec.member_count = u16le(b);
        const size_t words = (size_t(rec.member_count) + 63) / 64;
        if (words == 0 || words > 1024) {
            if (err) *err = "A1 member count out of range";
            return false;
        }
        if (!c.take(words * 8, &b, err, "A1 mask"))
            return false;
        rec.mask_words.resize(words);
        for (size_t i = 0; i < words; ++i)
            rec.mask_words[i] = u64le(b + i * 8);
        rec.mask = rec.mask_words[0];
        const size_t data_start = p + c.off();
        if (data_start + 2 > len) {
            if (err) *err = "truncated 0xA1 body length";
            return false;
        }
        const size_t body_end =
            data_start + 2 + size_t(u16le(d + data_start));
        size_t next = len;
        if (r + 1 < out->header.records) {
            // Between records sits a four-byte marker whose value is not
            // fixed (01 00 00 00 and 01 00 01 00 both occur). Prefer the
            // header after it, then the unprefixed boundary.
            if (body_end + 4 <= len && valid_header(body_end + 4)) {
                next = body_end + 4;
            } else if (valid_header(body_end)) {
                next = body_end;
            } else {
                next = 0;
                for (size_t q = body_end; q + 19 <= len; ++q) {
                    if (valid_header(q)) {
                        next = q;
                        break;
                    }
                }
                if (!next) {
                    if (err)
                        *err = "missing 0xA1 record boundary after record " +
                               std::to_string(r);
                    return false;
                }
            }
        }
        rec.data.assign(d + data_start, d + next);
        out->records.push_back(std::move(rec));
        p = next;
    }
    return true;
}

bool walk83ItemsEx(const uint8_t* body, size_t len, const JoinSchema& schema,
                   const PropTagTable* props, Walk83& out, std::string* err,
                   bool tag_rules);

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
    // A client-run script spawn carries its content bulk inline. The bulk is
    // keyed by the script's own Source hash (tag 47) and begins immediately
    // after loop B, so there is no separate parent ref: the bulk's anchor is
    // the parent. Detecting it by key equality is exact — a parent ref is 5
    // bytes and can never equal a 16-byte hash — and it leaves older spawns
    // that do carry a parent ref (spawn_weird) on the normal path.
    out.has_bulk = false;
    const JoinProp* source = nullptr;
    for (const auto& p : out.props_a)
        if (p.tag == 47 && p.value.size() == 16) {
            source = &p;
            break;
        }
    if (!source)
        for (const auto& p : out.props_b)
            if (p.tag == 47 && p.value.size() == 16) {
                source = &p;
                break;
            }
    const bool script_cls = cls_name == "LocalScript" ||
                            cls_name == "ModuleScript" ||
                            cls_name == "Script";
    if (script_cls && source && c.left >= 16 &&
        std::memcmp(c.p, source->value.data(), 16) == 0) {
        size_t used = 0;
        if (!parseBulkItem(c.p, c.left, 16, out.bulk, &used, err)) {
            if (err)
                *err = "spawn inline bulk: " + *err;
            return false;
        }
        if (used == 0)
            return fail(err, "spawn inline bulk consumed zero bytes");
        out.has_bulk = true;
        c.p += used;
        c.left -= used;
        out.plo = out.bulk.anchor_lo;
        out.parent = out.bulk.anchor;
        if (consumed)
            *consumed = c.off();
        return true;
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

bool parseBulkItem(const uint8_t* d, size_t n, size_t key_len, BulkItem& out,
                   size_t* consumed, std::string* err) {
    if (key_len != 8 && key_len != 16)
        return fail(err, "bulk key length must be 8 or 16");
    Cursor c(d, n);
    const uint8_t* b = nullptr;
    if (!c.take(key_len, &b, err, "bulk key"))
        return false;
    std::memcpy(out.key, b, key_len);
    out.key_len = uint8_t(key_len);
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
        // The last slot is the member-count sentinel. The native table stores
        // 28 there, but the wire tag is 25; keep this table byte-identical to
        // propTagsForSchema() so one parser cannot accept a prop id the other
        // rejects.
        const uint8_t tag = (i == kPropTagCount - 1) ? kPropSentinelTag
                                                     : kPropTags[i];
        if (tag == 255)
            continue;
        t.tag[uint16_t(i)] = tag;
        if (kPropPrefix[i])
            t.prefix[uint16_t(i)] = true;
        if (kPropFixedLen[i])
            t.fixed_len[uint16_t(i)] = kPropFixedLen[i];
        if (kPropAltLen[i])
            t.alt_len[uint16_t(i)] = kPropAltLen[i];
    }
    return t;
}

PropTagTable propTagsForSchema(const JoinSchema& schema) {
    // The native runtime table is the schema's member concatenation in
    // class/member order (verified exactly on the frozen schema), so
    // derive the ids instead of trusting a frozen dump: any member
    // insertion shifts every id after it.
    PropTagTable t;
    std::map<std::pair<std::string, std::string>, uint16_t> by_name;
    uint32_t id = 0;
    for (size_t ci = 0; ci < schema.classes.size(); ++ci) {
        const auto& cls = schema.classes[ci];
        for (const auto& m : cls.members) {
            if (id <= 0xFFFF) {
                const uint16_t pid = uint16_t(id);
                t.tag[pid] = m.tag;
                t.owner_class[pid] = uint16_t(ci);
                t.member_name[pid] = m.name;
                if (m.tag == 33 || m.tag == 61)
                    t.prefix[pid] = true;
                by_name.emplace(std::make_pair(cls.name, m.name), pid);
            }
            ++id;
        }
    }
    // Wire-shape overrides follow (class, member), never a frozen id.
    static const bool no_overrides = []() {
        const char* s = std::getenv("RBX_NO_PROP_OVERRIDES");
        return s && *s && *s != '0';
    }();
    for (int i = 0; i < kPropOverrideCount && !no_overrides; ++i) {
        const PropTagOverride& o = kPropOverrides[i];
        auto it = by_name.find(std::make_pair(std::string(o.cls),
                                              std::string(o.member)));
        if (it == by_name.end())
            continue; // class/member no longer in this schema
        const uint16_t pid = it->second;
        t.tag[pid] = o.tag;
        if (o.prefix)
            t.prefix[pid] = true;
        else
            t.prefix.erase(pid);
        if (o.fixed_len)
            t.fixed_len[pid] = o.fixed_len;
        else
            t.fixed_len.erase(pid);
        if (o.alt_len)
            t.alt_len[pid] = o.alt_len;
        else
            t.alt_len.erase(pid);
    }
    // Sentinel entry at the member-count bound (== prop_bound on every
    // observed schema). Native stores tag 28; the wire tag is 25.
    if (id <= 0xFFFF)
        t.tag[uint16_t(id)] = kPropSentinelTag;
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

// Skip helpers for the 0x11 appearance body (framing only; values are
// session state or diagnostics, never applied).
static bool skip11Blob(Cursor& c, std::string* err, const char* what) {
    // [u32 comp][u32 decomp][comp bytes]; decomp capped, comp bytes
    // skipped (never inflated for framing).
    const uint8_t* b = nullptr;
    if (!c.take(4, &b, err, what))
        return false;
    const uint32_t comp = u32le(b);
    if (!c.take(4, &b, err, what))
        return false;
    if (!checkDecompLen(u32le(b), err, what))
        return false;
    if (!c.take(comp, &b, err, what))
        return false;
    return true;
}
static bool skip11Str32(Cursor& c, std::string* err, const char* what) {
    // [u32 len][bytes], len capped at 64MB (state limit mirrored).
    const uint8_t* b = nullptr;
    if (!c.take(4, &b, err, what))
        return false;
    const uint32_t len = u32le(b);
    if (len > (64u << 20))
        return fail(err, std::string(what) + " string too big");
    if (!c.take(len, &b, err, what))
        return false;
    return true;
}
static bool skip11StrVar(Cursor& c, std::string* err, const char* what) {
    // [varint len][bytes], len capped at 64MB.
    uint64_t len = 0;
    if (!c.varint(&len, err, what))
        return false;
    if (len > (64u << 20))
        return fail(err, std::string(what) + " string too big");
    const uint8_t* b = nullptr;
    if (!c.take(size_t(len), &b, err, what))
        return false;
    return true;
}

bool parseItem11(const uint8_t* d, size_t n, Item11& out, size_t* consumed,
                 std::string* err) {
    // sub_32EB3EA (ex-32BA84E): version-gated appearance data. Every
    // count/sentinel below is wire-driven; string length limits are
    // session state (validation-only caps above).
    Cursor c(d, n);
    const uint8_t* b = nullptr;
    if (!c.take(4, &b, err, "11 version"))
        return false;
    const uint32_t v = u32le(b);
    out.version = v;
    out.n1 = out.n2 = out.n3 = out.n4 = out.loop1 = out.loop2 = 0;
    if (v >= 9) {
        if (!c.take(8, &b, err, "11 f32 pair"))
            return false;
    }
    const bool doA = (v >= 9 || v == 8);
    if (doA) {
        uint8_t present = 0;
        if (!c.u8(&present, err, "11 present"))
            return false;
        if (present) {
            if (!skip11Blob(c, err, "11 present blob"))
                return false;
            if (!skip11StrVar(c, err, "11 present str"))
                return false;
        }
    }
    if (doA || v >= 7) {
        if (!c.take(4, &b, err, "11 expect"))
            return false;
        if (!skip11Blob(c, err, "11 expect blob"))
            return false;
        if (!skip11StrVar(c, err, "11 expect str"))
            return false;
    }
    if (v >= 7 || v == 6) {
        if (!c.take(8, &b, err, "11 f64"))
            return false;
        uint32_t ns[4];
        for (int k = 0; k < 4; ++k) {
            if (!c.take(4, &b, err, "11 count"))
                return false;
            ns[k] = u32le(b);
            if (ns[k] > c.left)
                return fail(err, "11 count overruns");
        }
        out.n1 = ns[0];
        out.n2 = ns[1];
        out.n3 = ns[2];
        out.n4 = ns[3];
        // Principled count bounds (every element costs >= its minimum;
        // untrusted u32 must never drive a spin — cf. the fuzz-found
        // 4GB-decomp OOM class). N1 elems >= 4B(u32str) + 8B(f64);
        // N2-N4 elems >= 1B(vstr) + 8B(f64).
        if (ns[0] > c.left / 12)
            return fail(err, "11 n1 overruns");
        for (uint32_t i = 0; i < ns[0]; ++i) {
            if (!skip11Str32(c, err, "11 n1 str"))
                return false;
            if (!c.take(8, &b, err, "11 n1 f64"))
                return false;
        }
        for (int k = 1; k < 4; ++k) {
            if (ns[k] > c.left / 9)
                return fail(err, "11 nK overruns");
            for (uint32_t i = 0; i < ns[k]; ++i) {
                if (!skip11StrVar(c, err, "11 nK str"))
                    return false;
                if (!c.take(8, &b, err, "11 nK f64"))
                    return false;
            }
        }
    }
    if (v >= 3) {
        uint8_t hasX = 0;
        if (!c.u8(&hasX, err, "11 hasX"))
            return false;
        if (hasX) {
            if (!c.take(24, &b, err, "11 xwords"))
                return false;
        }
    }
    {
        uint8_t enter = 0;
        if (!c.u8(&enter, err, "11 loop1"))
            return false;
        if (enter == 0) {
            for (;;) {
                if (!skip11Str32(c, err, "11 l1 str"))
                    return false;
                if (!c.take(12, &b, err, "11 l1 f32s"))
                    return false;
                uint8_t cont = 0;
                if (!c.u8(&cont, err, "11 l1 cont"))
                    return false;
                if (++out.loop1 > c.total)
                    return fail(err, "11 loop1 overruns");
                if (cont != 0)
                    break;
            }
        }
    }
    {
        uint8_t enter = 0;
        if (!c.u8(&enter, err, "11 loop2"))
            return false;
        if (enter == 0) {
            for (;;) {
                if (!skip11Str32(c, err, "11 l2 str"))
                    return false;
                if (!c.take(4, &b, err, "11 l2 f32"))
                    return false;
                if (!c.take(4, &b, err, "11 l2 u32"))
                    return false;
                uint8_t cont = 0;
                if (!c.u8(&cont, err, "11 l2 cont"))
                    return false;
                if (++out.loop2 > c.total)
                    return fail(err, "11 loop2 overruns");
                if (cont != 0)
                    break;
            }
        }
    }
    if (!c.take(20, &b, err, "11 tail f32s"))
        return false;
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

bool parseItem09(const uint8_t* d, size_t n, Item09& out, size_t* consumed,
                 std::string* err) {
    // sub_361440A: a single u8; anything but 4 throws natively (packet
    // dropped) -> rejected, never a hard fail. Truncation stays loud.
    Cursor c(d, n);
    if (!c.u8(&out.v, err, "09 value"))
        return false;
    out.rejected = out.v != 4;
    if (consumed)
        *consumed = c.off();
    return true;
}

namespace {

// Inflate the 0x0B/0x25 entry batch payload ([u32 comp][u32 decomp][zstd]).
// Native sub_651756E reads both lengths from the wire, bounds the
// decompressed size, inflates, and requires the exact decompressed size.
bool inflateBatch(const uint8_t* src, size_t src_len, uint32_t decomp,
                  std::vector<uint8_t>& out, std::string* err) {
    if (!checkDecompLen(decomp, err, "0b batch"))
        return false;
    out.resize(size_t(decomp));
    if (decomp == 0) {
        if (src_len != 0)
            return fail(err, "0b batch has payload but zero decomp");
        return true;
    }
    const size_t got = ZSTD_decompress(out.data(), out.size(), src, src_len);
    if (ZSTD_isError(got))
        return fail(err, std::string("0b batch zstd: ") +
                             ZSTD_getErrorName(got));
    if (got != decomp)
        return fail(err, "0b batch decompressed " + std::to_string(got) +
                             " != " + std::to_string(decomp));
    return true;
}

// N entries from the decompressed batch (sub_65C578A entry loop):
// [guid ref1][u16 class][prop loop][guid ref2]([u8 extra] iff tag_rules).
bool parseBatchEntries(const uint8_t* d, size_t n, const JoinSchema& schema,
                       uint32_t count, bool tag_rules,
                       std::vector<JoinBatchEntry>& out, size_t* tail,
                       std::string* err) {
    Cursor c(d, n);
    out.clear();
    if (count > n)
        return fail(err, "0b batch entry count exceeds payload");
    out.reserve(count < 4096 ? count : 4096);
    for (uint32_t i = 0; i < count; ++i) {
        JoinBatchEntry e;
        int64_t id = -1;
        if (!read_guid(c, e.ref1.lo, id, err, "0b entry ref1"))
            return false;
        e.ref1.id = uint32_t(id);
        const uint8_t* b = nullptr;
        if (!c.take(2, &b, err, "0b entry class"))
            return false;
        e.cls = u16le(b);
        if (size_t(e.cls) >= schema.classes.size())
            return fail(err, "0b entry " + std::to_string(i) + " class " +
                                 std::to_string(e.cls) + " out of range");
        const SchemaClass& cls = schema.classes[e.cls];
        if (!read_prop_loop(c, cls.members, cls.name, false, e.props, err))
            return false;
        int64_t rid2 = -1;
        if (!read_guid(c, e.ref2.lo, rid2, err, "0b entry ref2"))
            return false;
        e.ref2.id = uint32_t(rid2);
        if (tag_rules) {
            if (!c.u8(&e.extra, err, "0b entry extra"))
                return false;
            e.has_extra = true;
        }
        out.push_back(std::move(e));
    }
    if (tail)
        *tail = c.left;
    return true;
}

bool parseItem0bInto(Cursor& c, const JoinSchema* schema, bool tag_rules,
                     Item0b& out, std::string* err) {
    out = Item0b{};
    const uint8_t* b = nullptr;
    if (!c.take(4, &b, err, "0b count"))
        return false;
    out.n = u32le(b);
    if (out.n == 0) {
        out.dropped = true;
        return true;
    }
    if (!c.take(4, &b, err, "0b comp"))
        return false;
    out.comp = u32le(b);
    if (!c.take(4, &b, err, "0b decomp"))
        return false;
    out.decomp = u32le(b);
    const uint8_t* src = nullptr;
    if (!c.take(size_t(out.comp), &src, err, "0b payload"))
        return false;
    std::vector<uint8_t> payload;
    if (!inflateBatch(src, out.comp, out.decomp, payload, err))
        return false;
    if (schema == nullptr) {
        out.entries_unparsed = true;
        return true;
    }
    if (!parseBatchEntries(payload.data(), payload.size(), *schema, out.n,
                           tag_rules, out.entries, &out.batch_tail, err))
        return false;
    return true;
}

} // namespace

bool parseItem0b(const uint8_t* d, size_t n, Item0b& out, size_t* consumed,
                 std::string* err, const JoinSchema* schema, bool tag_rules) {
    // sub_65C578A: [u32 N]; N == 0 null-drops (LABEL_112, no throw).
    // N != 0: [u32 comp][u32 decomp][zstd payload] -> N entries.
    Cursor c(d, n);
    if (!parseItem0bInto(c, schema, tag_rules, out, err))
        return false;
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseItem14(const uint8_t* d, size_t n, Item14& out, size_t* consumed,
                 std::string* err) {
    // sub_3578CB8: [triplet][u32]([u32] iff state&0x20000)([u32] iff
    // state&0x100000000000000). State-gated tails are greedy-optional.
    Cursor c(d, n);
    if (!read_triplet(c, out.t, err, "14 triple"))
        return false;
    const uint8_t* b = nullptr;
    if (!c.take(4, &b, err, "14 a"))
        return false;
    out.a = u32le(b);
    out.has_b = false;
    out.has_c = false;
    if (c.left >= 4) {
        if (!c.take(4, &b, err, "14 b"))
            return false;
        out.b = u32le(b);
        out.has_b = true;
    }
    if (c.left >= 4) {
        if (!c.take(4, &b, err, "14 c"))
            return false;
        out.c = u32le(b);
        out.has_c = true;
    }
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseItem18(const uint8_t* d, size_t n, Item18& out, size_t* consumed,
                 std::string* err) {
    // sub_35797E8: [triplet][u32][u32]. Fixed.
    Cursor c(d, n);
    if (!read_triplet(c, out.t, err, "18 triple"))
        return false;
    const uint8_t* b = nullptr;
    if (!c.take(4, &b, err, "18 a"))
        return false;
    out.a = u32le(b);
    if (!c.take(4, &b, err, "18 b"))
        return false;
    out.b = u32le(b);
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseItem19(const uint8_t* d, size_t n, Item19& out, size_t* consumed,
                 std::string* err) {
    // sub_3578E52: [3x f32 3614DE4][f64 69F989A][u32 69F914C]. Fixed 24B.
    Cursor c(d, n);
    const uint8_t* b = nullptr;
    if (!c.take(12, &b, err, "19 triple"))
        return false;
    for (int i = 0; i < 3; ++i)
        out.trip[i] = u32le(b + 4 * i);
    if (!c.take(8, &b, err, "19 double"))
        return false;
    out.dbl_bits = u64le(b);
    if (!c.take(4, &b, err, "19 tail"))
        return false;
    out.tail = u32le(b);
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseItem1a(const uint8_t* d, size_t n, Item1a& out, size_t* consumed,
                 std::string* err) {
    // sub_3578F48: [u32][triplet][u32][tail union 357874E/35788A2:
    // [f32][u8][u8]([u32] iff session-gated)]. Trailing u32 greedy.
    Cursor c(d, n);
    const uint8_t* b = nullptr;
    if (!c.take(4, &b, err, "1a a"))
        return false;
    out.a = u32le(b);
    if (!read_triplet(c, out.t, err, "1a triple"))
        return false;
    if (!c.take(4, &b, err, "1a b"))
        return false;
    out.b = u32le(b);
    if (!c.take(4, &b, err, "1a f"))
        return false;
    out.f_bits = u32le(b);
    if (!c.u8(&out.b1, err, "1a b1"))
        return false;
    if (!c.u8(&out.b2, err, "1a b2"))
        return false;
    out.has_u = false;
    out.u = 0;
    if (c.left >= 4) {
        if (!c.take(4, &b, err, "1a u"))
            return false;
        out.u = u32le(b);
        out.has_u = true;
    }
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseItem1d(const uint8_t* d, size_t n, Item1d& out, size_t* consumed,
                 std::string* err) {
    // sub_3579956: single 336800E guid.
    Cursor c(d, n);
    if (!read_guid(c, out.ref.lo, out.ref.id, err, "1d ref"))
        return false;
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseItem1e(const uint8_t* d, size_t n, Item1e& out, size_t* consumed,
                 std::string* err) {
    // sub_3579AAA: single 336800E guid.
    Cursor c(d, n);
    if (!read_guid(c, out.ref.lo, out.ref.id, err, "1e ref"))
        return false;
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseItem1f(const uint8_t* d, size_t n, Item1f& out, size_t* consumed,
                 std::string* err) {
    // sub_35C0CE6: [u8 a][u8 b]([u32 len][bytes] 69F9A5E cap 0x40000
    // iff (uint8_t)(a-3) <= 0xFD, i.e. a != 1 && a != 2). a == 1 || 2
    // null-drops (consumes a+b only). len > cap throws natively (bytes
    // present) -> rejected; truncation stays a hard fail.
    Cursor c(d, n);
    if (!c.u8(&out.a, err, "1f a"))
        return false;
    if (!c.u8(&out.b, err, "1f b"))
        return false;
    out.dropped = out.a == 1 || out.a == 2;
    out.rejected = false;
    out.blob.clear();
    if (out.dropped) {
        if (consumed)
            *consumed = c.off();
        return true;
    }
    const uint8_t* b = nullptr;
    uint32_t len = 0;
    if (!c.take(4, &b, err, "1f len"))
        return false;
    len = u32le(b);
    if (len > 0x40000) {
        out.rejected = true;
        if (consumed)
            *consumed = c.off();
        return true;
    }
    if (!c.take(len, &b, err, "1f blob"))
        return false;
    out.blob.assign(b, b + len);
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseItem24(const uint8_t* d, size_t n, Item24& out, size_t* consumed,
                 std::string* err) {
    // sub_357945C: a single u32 (69F921C). Fixed.
    Cursor c(d, n);
    const uint8_t* b = nullptr;
    if (!c.take(4, &b, err, "24 value"))
        return false;
    out.v = u32le(b);
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseItem25(const uint8_t* d, size_t n, Item25& out, size_t* consumed,
                 std::string* err, const JoinSchema* schema, bool tag_rules) {
    // sub_65C83BE -> sub_65C8292: the same [u32 N] + optional 0x0B
    // blob/entry batch (sub_65C578A), then [elements while flags&8]
    // [guid][u32][u8].
    Cursor c(d, n);
    const uint8_t* b = nullptr;
    if (!parseItem0bInto(c, schema, tag_rules, out.batch, err))
        return false;
    out.elems.clear();
    while (true) {
        uint8_t flags = 0;
        if (!c.u8(&flags, err, "25 elem flags"))
            return false;
        Elem0d e;
        e.flags = flags;
        if ((flags & 0xF7) == 0) {
            const size_t start = c.off();
            uint8_t vf = 0;
            if (!c.u8(&vf, err, "25 elem valflag"))
                return false;
            if (!c.take(vf != 0 ? 12 : 3, &b, err, "25 elem value"))
                return false;
            e.payload.assign(c.p - (c.off() - start), c.p);
        }
        out.elems.push_back(std::move(e));
        if (!(flags & 8))
            break;
    }
    if (!read_guid(c, out.ref.lo, out.ref.id, err, "25 ref"))
        return false;
    if (!c.take(4, &b, err, "25 v"))
        return false;
    out.v = u32le(b);
    if (!c.u8(&out.flag, err, "25 flag"))
        return false;
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseItem26(const uint8_t* d, size_t n, bool server_null, Item26& out,
                 size_t* consumed, std::string* err) {
    // sub_3498B2E: replicator+18768 set -> null item, zero bytes read
    // (server_null mirrors it; client context passes false). Else
    // [varint 69F89E8] zigzag s64 ((V >> 1) ^ -(V & 1)). Fixed.
    out.dropped = server_null;
    out.v = 0;
    if (server_null) {
        if (consumed)
            *consumed = 0;
        return true;
    }
    Cursor c(d, n);
    uint64_t v = 0;
    if (!c.varint(&v, err, "26 varint"))
        return false;
    out.v = int64_t((v >> 1) ^ (0ull - (v & 1)));
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseItem27(const uint8_t* d, size_t n, Item27& out, size_t* consumed,
                 std::string* err) {
    // sub_357A288: [u32 69F921C][15x f32 sub_35AB528 = 60B]. Fixed.
    Cursor c(d, n);
    const uint8_t* b = nullptr;
    if (!c.take(4, &b, err, "27 id"))
        return false;
    out.id = u32le(b);
    if (!c.take(60, &b, err, "27 floats"))
        return false;
    for (int i = 0; i < 15; ++i)
        out.f[i] = u32le(b + 4 * i);
    if (consumed)
        *consumed = c.off();
    return true;
}

bool parseItem28(const uint8_t* d, size_t n, Item28& out, size_t* consumed,
                 std::string* err) {
    // sub_357A18C: a single u32 (69F921C). Fixed.
    Cursor c(d, n);
    const uint8_t* b = nullptr;
    if (!c.take(4, &b, err, "28 value"))
        return false;
    out.v = u32le(b);
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
    // Fixed-length override (base shape + proven fixed trailer): framing
    // wins over the tag reader; raw bytes carried verbatim. Two-form
    // props (alt_len): the short form wins iff a valid next-triple
    // header sits exactly at +short_len (peeked, never consumed);
    // otherwise the long form reads.
    auto itl = table.fixed_len.find(out.prop);
    if (itl != table.fixed_len.end()) {
        size_t vlen = itl->second;
        auto ita = table.alt_len.find(out.prop);
        if (ita != table.alt_len.end()) {
            // Clean peek: [short value][0x03][ref][u16 prop].
            Cursor q = c;
            const uint8_t* qb = nullptr;
            bool short_ok = false;
            if (q.take(vlen, &qb, nullptr, "prop alt skip")) {
                uint8_t ty = 0;
                uint64_t plo = 0;
                int64_t pid = -1;
                const uint8_t* pb = nullptr;
                std::string noerr;
                if (q.u8(&ty, &noerr, "prop alt type") && ty == 0x03 &&
                    read_ref(q, plo, pid, &noerr, "prop alt ref") &&
                    q.take(2, &pb, &noerr, "prop alt prop")) {
                    const uint16_t cand =
                        uint16_t(pb[0] | (uint16_t(pb[1]) << 8));
                    auto itc = table.tag.find(cand);
                    short_ok = (itc != table.tag.end() &&
                                itc->second != 255);
                }
            }
            if (!short_ok)
                vlen = ita->second;
        }
        const uint8_t* b = nullptr;
        if (!c.take(vlen, &b, err, "prop fixed value"))
            return false;
        out.value.assign(b, b + vlen);
        if (consumed)
            *consumed = c.off();
        return true;
    }
    // Value-format byte (sub_64CC280 consumes it between the u16 prop id
    // and the value reader, via the replicator virtual at vtable+0x458).
    // Native gdb extents on 209 live items pin the layout exactly:
    //   [ref][u16 prop][u8 vfmt][value]
    // e.g. Humanoid.MoveDirectionInternal (tag 22, 12B) = 20B item, and
    // MoveDirectionInternal/numExpectedDirectChildren/NetworkHumanoidState
    // all carry vfmt=0x01. Without it every live 0x03 item under-reads by
    // one byte (an f32 item then stops on the float's exponent byte).
    if (!c.u8(&out.vfmt, err, "prop value format"))
        return false;
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
                 Walk83& out, std::string* err, bool tag_rules) {
    return walk83ItemsEx(body, len, schema, nullptr, out, err, tag_rules);
}

bool walk83ItemsEx(const uint8_t* body, size_t len, const JoinSchema& schema,
                   const PropTagTable* props, Walk83& out, std::string* err,
                   bool tag_rules) {
    if (len < 2 || body[0] != 0x83)
        return fail(err, "not a 0x83 message");
    out.items.clear();
    out.complete = false;
    out.stop_type = 0;
    out.stop_off = 0;
    size_t i = 1;
    out.end_off = 0;
    while (i < len) {
        const size_t item_off = i;
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
        } else if (type == 0x11) { // appearance data (sub_32EB3EA)
            if (!parseItem11(body + i, len - i, it.appearance11, &used,
                             err)) {
                if (err)
                    *err = "0x11 item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x11 item consumed zero bytes");
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
        } else if (type == 0x09) { // u8-checked item (sub_361440A)
            if (!parseItem09(body + i, len - i, it.v09, &used, err)) {
                if (err)
                    *err = "0x09 item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (it.v09.rejected) { // v != 4 throws natively: stop pinned
                out.stop_type = type;
                out.stop_off = i - 1;
                return true;
            }
            if (used == 0)
                return fail(err, "0x09 item consumed zero bytes");
            i += used;
        } else if (type == 0x0b) { // N-head batch item (sub_65C578A)
            if (!parseItem0b(body + i, len - i, it.head0b, &used, err,
                             &schema, tag_rules)) {
                if (err)
                    *err = "0x0b item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x0b item consumed zero bytes");
            i += used;
        } else if (type == 0x14) { // triplet + u32s (sub_3578CB8)
            if (!parseItem14(body + i, len - i, it.v14, &used, err)) {
                if (err)
                    *err = "0x14 item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x14 item consumed zero bytes");
            i += used;
        } else if (type == 0x18) { // triplet + u32s (sub_35797E8)
            if (!parseItem18(body + i, len - i, it.v18, &used, err)) {
                if (err)
                    *err = "0x18 item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x18 item consumed zero bytes");
            i += used;
        } else if (type == 0x19) { // 3xf32 + f64 + u32 (sub_3578E52)
            if (!parseItem19(body + i, len - i, it.v19, &used, err)) {
                if (err)
                    *err = "0x19 item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x19 item consumed zero bytes");
            i += used;
        } else if (type == 0x1a) { // u32 + triplet + tail (sub_3578F48)
            if (!parseItem1a(body + i, len - i, it.v1a, &used, err)) {
                if (err)
                    *err = "0x1a item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x1a item consumed zero bytes");
            i += used;
        } else if (type == 0x1d) { // entity ref (sub_3579956)
            if (!parseItem1d(body + i, len - i, it.ref1d, &used, err)) {
                if (err)
                    *err = "0x1d item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x1d item consumed zero bytes");
            i += used;
        } else if (type == 0x1e) { // entity ref (sub_3579AAA)
            if (!parseItem1e(body + i, len - i, it.ref1e, &used, err)) {
                if (err)
                    *err = "0x1e item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x1e item consumed zero bytes");
            i += used;
        } else if (type == 0x1f) { // u8s + bounded blob (sub_35C0CE6)
            if (!parseItem1f(body + i, len - i, it.blob1f, &used, err)) {
                if (err)
                    *err = "0x1f item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (it.blob1f.rejected) { // over-cap blob throws natively
                out.stop_type = type;
                out.stop_off = i - 1;
                return true;
            }
            if (used == 0 && !it.blob1f.dropped)
                return fail(err, "0x1f item consumed zero bytes");
            i += used;
        } else if (type == 0x24) { // u32 item (sub_357945C)
            if (!parseItem24(body + i, len - i, it.v24, &used, err)) {
                if (err)
                    *err = "0x24 item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x24 item consumed zero bytes");
            i += used;
        } else if (type == 0x25) { // N-head + elems + ref (sub_65C83BE)
            if (!parseItem25(body + i, len - i, it.v25, &used, err, &schema,
                             tag_rules)) {
                if (err)
                    *err = "0x25 item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x25 item consumed zero bytes");
            i += used;
        } else if (type == 0x26) { // zigzag varint (sub_3498B2E)
            if (!parseItem26(body + i, len - i, false, it.v26, &used,
                             err)) {
                if (err)
                    *err = "0x26 item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0 && !it.v26.dropped)
                return fail(err, "0x26 item consumed zero bytes");
            i += used;
        } else if (type == 0x27) { // u32 + 15x f32 (sub_357A288)
            if (!parseItem27(body + i, len - i, it.v27, &used, err)) {
                if (err)
                    *err = "0x27 item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x27 item consumed zero bytes");
            i += used;
        } else if (type == 0x28) { // u32 item (sub_357A18C)
            if (!parseItem28(body + i, len - i, it.v28, &used, err)) {
                if (err)
                    *err = "0x28 item at offset " + std::to_string(i) +
                           ": " + *err;
                return false;
            }
            if (used == 0)
                return fail(err, "0x28 item consumed zero bytes");
            i += used;
        } else {
        deferred:
            // Typeless script-data bulk fallback (content-addressed
            // LocalScript bytecode): only attempted at an otherwise-unknown
            // chain byte, never in place of a known item. A validated bulk
            // continues the chain; anything else stops pinned here.
            if (type != 0x00) {
                // Typeless: the unknown byte is key data, not a type, so
                // rewind one (i already advanced past it). Current servers
                // key the bulk with the full 16-byte Source hash; older
                // captures used its trailing 8-byte half.
                //
                // Only a spawn of a script class can own a content bulk, and
                // the key must equal that spawn's own Source hash. Without
                // that anchor the test is far too loose: ordinary chain bytes
                // can satisfy [16B][00][len][payload][ref], which produced a
                // phantom 0x78 (last=0, matches=0) in the 2096-spawn run.
                // Adjacency is exact in every capture: the bulk immediately
                // follows its spawn.
                const SpawnRecord* host = nullptr;
                if (!out.items.empty() && out.items.back().type == 0x02)
                    host = &out.items.back().spawn;
                std::string host_source;
                if (host && host->cls < schema.classes.size()) {
                    const std::string& hn = schema.classes[host->cls].name;
                    if (hn == "LocalScript" || hn == "ModuleScript" ||
                        hn == "Script") {
                        for (const auto& p : host->props_a)
                            if (p.tag == 47 && p.value.size() == 16) {
                                host_source.assign(p.value.begin(),
                                                   p.value.end());
                                break;
                            }
                        if (host_source.empty())
                            for (const auto& p : host->props_b)
                                if (p.tag == 47 && p.value.size() == 16) {
                                    host_source.assign(p.value.begin(),
                                                       p.value.end());
                                    break;
                                }
                    }
                }
                bool took = false;
                if (!host_source.empty()) {
                    const size_t start = i - 1;
                    size_t key_len = 0;
                    if (len - start >= 16 &&
                        std::memcmp(body + start, host_source.data(), 16) == 0)
                        key_len = 16;
                    else if (len - start >= 8 &&
                             std::memcmp(body + start, host_source.data() + 8,
                                         8) == 0)
                        key_len = 8;
                    if (key_len) {
                        BulkItem bulk;
                        size_t bulk_used = 0;
                        std::string bulk_err;
                        if (parseBulkItem(body + start, len - start, key_len,
                                          bulk, &bulk_used, &bulk_err) &&
                            bulk_used > 1) {
                            const size_t next = start + bulk_used;
                            // A subsequent 0x00 ends the chain, and any real
                            // item type is small (the outer table tops out at
                            // 0x41).
                            if (next >= len || body[next] == 0x00 ||
                                body[next] <= 0x41) {
                                it.type = 0x78; // pseudo-type (no wire type)
                                it.bulk = std::move(bulk);
                                i = next;
                                out.spans.push_back({item_off, i, 0x78});
                                out.items.push_back(std::move(it));
                                took = true;
                            }
                        }
                    }
                }
                if (took)
                    continue;
            }
            // Deferred-type item (0x03 prop updates need the runtime
            // prop-id table; anything else needs its own handler's
            // grammar), so the walk stops here with the stop
            // point pinned (coverage is measured, never silent).
            out.stop_type = type;
            out.stop_off = i - 1;
            out.spans.push_back({item_off, i - 1, type});
            return true;
        }
        out.spans.push_back({item_off, i, type});
        const uint16_t spawn_cls = type == 0x02 ? it.spawn.cls : 0;
        const bool spawn_inline_bulk = type == 0x02 && it.spawn.has_bulk;
        BulkItem spawn_bulk;
        std::string spawn_source;
        if (type == 0x02) {
            for (const auto& p : it.spawn.props_a)
                if (p.tag == 47 && p.value.size() == 16) {
                    spawn_source.assign(p.value.begin(), p.value.end());
                    break;
                }
            if (spawn_source.empty())
                for (const auto& p : it.spawn.props_b)
                    if (p.tag == 47 && p.value.size() == 16) {
                        spawn_source.assign(p.value.begin(), p.value.end());
                        break;
                    }
            if (spawn_inline_bulk)
                spawn_bulk = it.spawn.bulk;
        }
        out.items.push_back(std::move(it));
        // A script spawn's inline content bulk was parsed as part of the
        // spawn item (its anchor is the parent). Surface it as the 0x78 item
        // the apply layer executes, immediately after its spawn.
        if (spawn_inline_bulk) {
            ChainItem b;
            b.type = 0x78;
            b.bulk = std::move(spawn_bulk);
            out.spans.push_back({item_off, i, 0x78});
            out.items.push_back(std::move(b));
        }
        // A script spawn can also be followed by a standalone content bulk.
        // The bulk has no type byte; its first key byte can collide with a
        // real item type (live LocalScript: 0x1f), so the generic unknown-type
        // fallback would never try it. Parse it only in the script-spawn
        // context, and only when the key matches the spawn's own Source hash
        // (full 16 bytes now, trailing 8 bytes in older captures).
        // A following 0x02 is another spawn, not a bulk; its bytes can
        // coincidentally satisfy the bulk key/marker test (live case at
        // offset 558), so never attempt a bulk over a known spawn type.
        if (type == 0x02 && !spawn_inline_bulk && i < len &&
            body[i] != 0x00 && body[i] != 0x02 &&
            spawn_cls < schema.classes.size()) {
            const std::string& cn = schema.classes[spawn_cls].name;
            if (cn == "LocalScript" || cn == "ModuleScript" || cn == "Script") {
                size_t key_len = 0;
                if (spawn_source.size() == 16 && len - i >= 16 &&
                    std::memcmp(body + i, spawn_source.data(), 16) == 0)
                    key_len = 16;
                else if (spawn_source.size() == 16 && len - i >= 8 &&
                         std::memcmp(body + i, spawn_source.data() + 8, 8) == 0)
                    key_len = 8;
                if (key_len) {
                    BulkItem bulk;
                    size_t bulk_used = 0;
                    std::string bulk_err;
                    if (parseBulkItem(body + i, len - i, key_len, bulk,
                                      &bulk_used, &bulk_err) &&
                        bulk_used > 0) {
                        ChainItem b;
                        b.type = 0x78;
                        b.bulk = std::move(bulk);
                        out.spans.push_back({i, i + bulk_used, 0x78});
                        out.items.push_back(std::move(b));
                        i += bulk_used;
                    }
                }
            }
        }
    }
    if (i >= len)
        out.complete = true; // clean EOF counts as complete (length-framed)
    if (out.complete && out.end_off == 0)
        out.end_off = i;
    return true;
}

} // namespace rbxclient
