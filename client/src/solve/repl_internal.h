// repl_internal.h — shared internals for the repl_* translation units.
//
// Private to librbx_solve (NOT installed, NOT included outside client/src):
// bounds-checked cursor, scalar helpers, and the value-reader primitives
// shared by the snapshot path (repl_snapshot.cpp) and the streaming path
// (repl_stream.cpp). Value readers live in repl_core.cpp. Pure code motion
// from replicate.cpp (recv v4 split); no behavior change.
#pragma once

#include "solve/replicate.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rbxclient {
namespace detail {

inline bool fail(std::string* err, const std::string& e) {
    if (err)
        *err = e;
    return false;
}

// Unsigned LEB128 at [p, end). Returns bytes consumed, 0 on truncation.
inline size_t leb128(const uint8_t* p, const uint8_t* end, uint64_t* v) {
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

inline uint32_t u32le(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

inline uint16_t u16le(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

inline uint64_t u64le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | p[i];
    return v;
}

// Upper bound for any single zstd frame we inflate (0x97 snapshot, 0x16
// batches, 0x0d entry frames): 64MB, ~200x the largest observed frame.
// Untrusted u32 lengths must never size an allocation directly
// (libFuzzer found a 4GB-decomp OOM via parseItem0d, recv v4).
inline bool checkDecompLen(uint32_t decomp, std::string* err,
                           const char* what) {
    if (decomp > (64u << 20)) {
        if (err)
            *err = std::string(what) + " decomp too big";
        return false;
    }
    return true;
}

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

bool read_name_value(Cursor& c, std::vector<uint8_t>& raw, std::string* err);
bool read_content_value(Cursor& c, bool prefix_mode, std::vector<uint8_t>& raw,
                        std::string* err);
bool read_tagged_value(Cursor& c, uint8_t tag, bool prefix_mode,
                       const std::string& ctx, std::vector<uint8_t>& raw,
                       std::string* err, int depth = 0);
bool read_tuple_value(Cursor& c, uint8_t tag, bool prefix_mode, int depth,
                      const std::string& ctx, EventArg& out, std::string* err);
bool read_ref(Cursor& c, uint64_t& lo, int64_t& id, std::string* err,
              const char* what);
bool read_guid(Cursor& c, uint64_t& lo, int64_t& id, std::string* err,
               const char* what);
// One sub_3617A08 triplet: [u8 flag]([3x u32] iff flag != 0, else
// [3x u8], zero-extended). IDA-verified (69F955A + 3x 69F914C/69F948A);
// NOT a string read. Same shape as 0x0d element payloads.
bool read_triplet(Cursor& c, ItemTriplet& out, std::string* err,
                  const char* what);
bool read_prop_loop(Cursor& c, const std::vector<SchemaMember>& members,
                    const std::string& cls_name, bool prefix_mode,
                    std::vector<JoinProp>& out, std::string* err);

} // namespace detail
} // namespace rbxclient
