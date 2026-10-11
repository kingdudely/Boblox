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

} // namespace rbxclient
