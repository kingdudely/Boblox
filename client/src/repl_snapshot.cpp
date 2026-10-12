// repl_snapshot.cpp — JoinData snapshot path: schema, records, delta batches (see replicate.h for provenance).
//
// Pure-function parsers, no I/O, no state (fuzz-friendly). Split verbatim
// from replicate.cpp (recv v4); no behavior change.
#include "replicate.h"

#include "repl_internal.h"

#include "util.h"

#include <cstring>

#include <zstd.h>

namespace rbxclient {
using namespace detail;

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
    // 2B codec-version magic: a3 cf (Crossroads 1818) or 31 8b
    // (Brookhaven 4924922222). Same layout after it ([00 00][u32 size]
    // [zstd]); unknown magics fail loudly, never guessed.
    if (!((body[1] == 0xA3 && body[2] == 0xCF) ||
          (body[1] == 0x31 && body[2] == 0x8B)) ||
        body[3] != 0x00 || body[4] != 0x00)
        return fail(err, "bad 0x97 magic");
    const uint32_t size = u32le(body + 5);
    static const uint8_t kZstdMagic[4] = {0x28, 0xB5, 0x2F, 0xFD};
    if (std::memcmp(body + 9, kZstdMagic, 4) != 0)
        return fail(err, "0x97 zstd magic missing");
    // Unlike 0x16 batches (whose comp length counts frame-only), the 0x97
    // frame runs to the end: no trailing byte (verified by decode).
    const size_t flen = len - 9;
    if (!checkDecompLen(size, err, "0x97 outer"))
        return false;
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
    if (!checkDecompLen(decomp, err, "0x16 batch"))
        return false;
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

// One ([u8 delta][value])* [0xFF] property loop (sub_3493C2E). Deltas are
// absolute member indexes (validated EOF-exact on all 2911 snapshot
// records; the native's relative id-walk absorbs its gappy global table
// onto the same members — see replicate.h).

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

bool parseNetAssetBatch(const uint8_t* d, size_t n,
                        std::vector<NetAssetEntry>& out, std::string* err) {
    // sub_348C62C flags=05 loop: [NetAssetId][varint payLen][payLen bytes]*
    // until the buffer is exhausted. NetAssetId uses the tag-60 shape
    // (varint schema (+ [8B id][varint v] iff schema != 0xFFFF)); the
    // payload pointer is recorded, never interpreted (factory 3647152
    // creates by schema with no extra wire reads).
    Cursor c(d, n);
    out.clear();
    const uint8_t* b = nullptr;
    while (c.left > 0) {
        NetAssetEntry e;
        const size_t id_start = c.off();
        uint64_t idx = 0;
        if (!c.varint(&idx, err, "netasset schema"))
            return false;
        if (idx > 0xFFFF)
            return fail(err, "netasset schema out of range at offset " +
                                 std::to_string(id_start));
        if (idx != 0xFFFF) {
            if (!c.take(8, &b, err, "netasset id"))
                return false;
            uint64_t vv = 0;
            if (!c.varint(&vv, err, "netasset v"))
                return false;
        }
        e.id_raw.assign(d + id_start, d + c.off());
        uint64_t paylen = 0;
        if (!c.varint(&paylen, err, "netasset paylen"))
            return false;
        if (!c.take(size_t(paylen), &b, err, "netasset payload"))
            return false;
        e.payload.assign(b, b + size_t(paylen));
        out.push_back(std::move(e));
    }
    return true;
}

namespace detail {

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

} // namespace detail

} // namespace rbxclient
