// blob.cpp — see blob.h / tools/challenge_blob.py (the Python reference).
#include "blob.h"

#include "util.h"
#include "xxh32.h"

#include <zstd.h>

#include <cstdio>
#include <cstring>

namespace rbxclient {

namespace {

constexpr uint8_t MAGIC[4] = {'R', 'S', 'B', '1'};
constexpr uint8_t ADD[4] = {0x00, 0xD7, 0xAE, 0x85};
constexpr uint8_t ZSTD_MAGIC[4] = {0x28, 0xB5, 0x2F, 0xFD};

uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool fail(std::string* err, const std::string& msg) {
    if (err)
        *err = msg;
    return false;
}

} // namespace

bool extractChallenge(const uint8_t* msg, size_t len, Challenge& out, std::string* err) {
    if (len < 13 || msg[0] != 0x9B)
        return fail(err, "not a 0x9B message");
    out.u1 = le32(msg + 1);
    out.u2 = le32(msg + 5);
    // Subtraction form: `13u + ln` below would be 32-bit arithmetic and wrap
    // for ln near 2^32, blessing a gigabyte-scale assign (fuzzer-found class).
    const uint32_t ln = le32(msg + 9);
    if ((uint64_t)ln > len - 13)
        return fail(err, "truncated 0x9B blob");
    out.blob.assign(msg + 13, msg + 13 + ln);
    return true;
}

bool findChallengeFrame(const uint8_t* d, size_t n, size_t* msg_off, size_t* msg_len) {
    size_t off = 0;
    while (off < n) {
        uint64_t v = 0;
        size_t k = rbx::compactVarintDecode(d + off, n - off, &v);
        if (!k)
            return false; // incomplete length prefix: wait for more data
        if (v == 0) {
            off += k;
            continue;
        }
        // Subtraction form: v tops out at 2^62-1 (compact varint) and
        // off/k stay within the buffer, so `n - off - k` cannot wrap.
        if (v > n - off - k)
            return false; // incomplete body: wait for more data
        const size_t mlen = (size_t)v;
        if (mlen >= 13 && d[off + k] == 0x9B) {
            if (msg_off)
                *msg_off = off + k;
            if (msg_len)
                *msg_len = mlen;
            return true;
        }
        off += k + mlen; // terminates: k >= 1 and mlen >= 1 here
    }
    return false;
}

bool decodeBlob(const uint8_t* blob, size_t len, std::vector<uint8_t>& wire, std::string* err) {
    if (len < 13)
        return fail(err, "blob too short");

    // key[i] = ((blob[i] ^ "RSB1"[i]) + ADD[i]) & 0xFF   (i < 4)
    uint8_t key[4];
    for (int i = 0; i < 4; i++)
        key[i] = (uint8_t)((blob[i] ^ MAGIC[i]) + ADD[i]);

    // plain[p] = blob[p] ^ ((41*p + key[p & 3]) & 0xFF)
    std::vector<uint8_t> dec(len);
    for (size_t p = 0; p < len; p++)
        dec[p] = (uint8_t)(blob[p] ^ (uint8_t)((41 * p + key[p & 3]) & 0xFF));

    // validation: xxhash32(plain, seed=0x2A) == LE(key)
    uint32_t want = le32(key);
    uint32_t got = xxh32(dec.data(), dec.size(), 0x2A);
    if (got != want) {
        char buf[64];
        snprintf(buf, sizeof(buf), "checksum mismatch: got 0x%08x want 0x%08x", got, want);
        return fail(err, buf);
    }

    uint32_t mode = le32(dec.data() + 4);
    const uint8_t* payload = dec.data() + 8;
    size_t plen = dec.size() - 8;

    if (mode == 0) { // raw wire program
        wire.assign(payload, payload + plen);
        return true;
    }

    // mode is the attacker-controlled zstd output size: cap it before the
    // allocation (production wire programs are ~7KB; 64MB is already absurd).
    // Without this, mode near 2^32 turns the vector below into a 4GB
    // allocation and aborts the process (fuzzer-found class).
    constexpr uint32_t kMaxWireBytes = 64u << 20;
    if (mode > kMaxWireBytes)
        return fail(err, "zstd output size unreasonable");

    if (plen < 4 || std::memcmp(payload, ZSTD_MAGIC, 4) != 0)
        return fail(err, "zstd magic missing");

    std::vector<uint8_t> out(mode);
    size_t r = ZSTD_decompress(out.data(), out.size(), payload, plen);
    if (ZSTD_isError(r))
        return fail(err, std::string("zstd: ") + ZSTD_getErrorName(r));
    if (r != mode)
        return fail(err, "zstd size mismatch");
    wire = std::move(out);
    return true;
}

} // namespace rbxclient
