// standardize.cpp — see standardize.h / tools/standardize_wire.py.
#include "standardize.h"

#include "tables.inc"

#include <cstring>

namespace rbxclient {

namespace {

// sub_27509DA: "mov ecx, offset byte_17E7B; bt ecx, edi" — the tested value is
// the address constant 0x17E7B itself, not the memory at that address.
constexpr uint64_t MASK1 = 0x829400003F02323ull;
constexpr uint64_t MASK2 = 0x00017E7Bull;

// Loader instruction-length semantics on the STANDARD opcode.
int opLength(int stdOp) {
    int v2 = stdOp - 7;
    if (v2 >= 0 && v2 <= 0x3B && ((MASK1 >> v2) & 1))
        return 2;
    int v4 = stdOp - 74;
    if (v4 >= 0 && v4 <= 0x10 && ((MASK2 >> v4) & 1))
        return 2;
    return 1;
}

bool fail(std::string* err, const std::string& msg) {
    if (err)
        *err = msg;
    return false;
}

bool varint(const std::vector<uint8_t>& buf, size_t& off, uint64_t& v) {
    v = 0;
    int sh = 0;
    while (true) {
        if (off >= buf.size())
            return false;
        uint8_t b = buf[off++];
        v |= (uint64_t)(b & 0x7F) << sh;
        if (!(b & 0x80))
            return true;
        sh += 7;
        if (sh > 63)
            return false;
    }
}

struct Proto {
    size_t codeOff = 0;
    uint64_t sizeCode = 0;
};

// Parse the v13 header (protoSize skipping) and collect per-proto code
// locations — mirrors standardize_wire.parse_protos.
bool parseProtos(const std::vector<uint8_t>& buf, std::vector<Proto>& protos, std::string* err) {
    size_t off = 0;
    if (buf.empty())
        return fail(err, "empty program");
    off += 1; // version
    if (off >= buf.size())
        return fail(err, "truncated header");
    uint8_t tv = buf[off++];
    uint64_t nStrings;
    if (!varint(buf, off, nStrings))
        return fail(err, "bad string count");
    for (uint64_t i = 0; i < nStrings; i++) {
        uint64_t ln;
        // Subtraction form throughout: `off + ln` is 64-bit arithmetic that
        // wraps for ln near 2^64, blessing a wrapped-small offset. off only
        // ever advances inside the buffer, so `buf.size() - off` cannot wrap.
        if (!varint(buf, off, ln) || ln > buf.size() - off)
            return fail(err, "bad string table");
        off += (size_t)ln;
    }
    if (tv == 3) {
        if (off >= buf.size())
            return fail(err, "truncated remap table");
        uint8_t idx = buf[off++];
        while (idx != 0) {
            uint64_t skip;
            if (!varint(buf, off, skip))
                return fail(err, "bad remap entry");
            if (off >= buf.size())
                return fail(err, "truncated remap table");
            idx = buf[off++];
        }
    }
    uint64_t protoCount;
    if (!varint(buf, off, protoCount))
        return fail(err, "bad proto count");
    // Each proto needs at least its 1-byte psize varint past this point, so
    // protoCount is bounded by the remaining bytes. Without this,
    // reserve(protoCount) on an attacker u64 throws length_error and aborts
    // (fuzzer-found class).
    if (protoCount > buf.size() - off)
        return fail(err, "proto count exceeds program");
    protos.clear();
    protos.reserve((size_t)protoCount);
    for (uint64_t i = 0; i < protoCount; i++) {
        uint64_t psize;
        if (!varint(buf, off, psize))
            return fail(err, "bad proto size");
        size_t pstart = off;
        if (off + 5 > buf.size())
            return fail(err, "truncated proto");
        off += 4 + 1;
        if (tv == 1 || tv == 2 || tv == 3) {
            uint64_t ts;
            if (!varint(buf, off, ts) || ts > buf.size() - off)
                return fail(err, "bad proto types");
            off += (size_t)ts;
        }
        Proto p;
        if (!varint(buf, off, p.sizeCode))
            return fail(err, "bad sizecode");
        p.codeOff = off;
        protos.push_back(p);
        // Subtraction form again (pstart + psize wraps near 2^64).
        if (psize > buf.size() - pstart)
            return fail(err, "proto overruns program");
        off = pstart + (size_t)psize;
    }
    return true;
}

} // namespace

bool standardize(std::vector<uint8_t>& buf, Stats* stats, std::string* err) {
    Stats local;
    Stats& st = stats ? *stats : local;

    std::vector<Proto> protos;
    if (!parseProtos(buf, protos, err))
        return false;
    st.protos = (int)protos.size();
    st.starts = 0;
    st.bad_walk = false;
    st.bad_proto = -1;

    for (size_t i = 0; i < protos.size(); i++) {
        const Proto& p = protos[i];
        // The walk below consumes 4*sizeCode bytes from codeOff and can only
        // fail partway through, after up to size/4 wasted iterations — reject
        // the impossible case at the door. (The walk itself is inherently
        // bounded: it fails at the first out-of-buffer word, so this changes
        // no verdict, only how fast garbage is rejected. It also makes the
        // `codeOff + 4*pc` arithmetic in the loop overflow-free.)
        if (p.sizeCode > (buf.size() - p.codeOff) / 4)
            return fail(err, "sizecode exceeds program");
        uint64_t pc = 0;
        while (pc < p.sizeCode) {
            size_t wordOff = p.codeOff + 4 * pc;
            if (wordOff + 4 > buf.size())
                return fail(err, "code overruns program");
            uint32_t wireWord = (uint32_t)buf[wordOff] | ((uint32_t)buf[wordOff + 1] << 8) |
                                ((uint32_t)buf[wordOff + 2] << 16) | ((uint32_t)buf[wordOff + 3] << 24);
            uint8_t wop = wireWord & 0xFF;
            uint8_t internal = REMAP[wop];
            uint8_t std = OPLEN[internal];
            uint32_t newWord = (wireWord & 0xFFFFFF00u) | std;
            buf[wordOff] = (uint8_t)newWord;
            buf[wordOff + 1] = (uint8_t)(newWord >> 8);
            buf[wordOff + 2] = (uint8_t)(newWord >> 16);
            buf[wordOff + 3] = (uint8_t)(newWord >> 24);
            pc += opLength(std);
            st.starts++;
        }
        if (pc != p.sizeCode) {
            st.bad_walk = true;
            st.bad_proto = (int)i;
            return fail(err, "proto walk failed: proto " + std::to_string(i) + " pc " +
                                 std::to_string(pc) + " != sizecode " + std::to_string(p.sizeCode));
        }
    }
    return true;
}

} // namespace rbxclient
