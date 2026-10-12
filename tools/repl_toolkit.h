// repl_toolkit.h — shared offline helpers for the repl_* dev tools.
//
// Header-only (tools are ad-hoc g++ builds, not CMake targets; compile
// with -Itools -Iclient/src). Pure code motion from repl_coverage.cpp +
// solve_proptags.cpp; no behavior change. Anything that walks saved
// chan1 captures uses this instead of a third copy of the framing loop.
#pragma once

#include "replicate.h"

#include <cstdint>
#include <fstream>
#include <istream>
#include <string>
#include <vector>

namespace repltool {

// One length-framed message body (INCLUDES the id byte) + file offset.
struct CapMsg {
    std::vector<uint8_t> body;
    size_t off = 0;
};

inline std::vector<uint8_t> slurpFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

// CompactVarint length prefix (util.h framing): returns value, sets *k to
// prefix bytes, or UINT64_MAX when truncated.
inline uint64_t capVarint(const uint8_t* p, size_t n, size_t* k) {
    size_t nn = size_t(1) << (p[0] >> 6);
    if (nn > n)
        return UINT64_MAX;
    uint64_t v = p[0] & 0x3F;
    for (size_t i = 1; i < nn; i++)
        v = (v << 8) | p[i];
    *k = nn;
    return v;
}

// Split a saved chan1 stream into frames (all ids; callers filter).
inline std::vector<CapMsg> splitFrames(const std::vector<uint8_t>& d) {
    std::vector<CapMsg> out;
    size_t off = 0;
    while (off < d.size()) {
        size_t k = 0;
        uint64_t ln = capVarint(d.data() + off, d.size() - off, &k);
        if (k == 0 || ln == 0 || off + k + ln > d.size())
            break;
        CapMsg m;
        m.body.assign(d.data() + off + k, d.data() + off + k + ln);
        m.off = off;
        out.push_back(std::move(m));
        off += k + ln;
    }
    return out;
}

// Decompressed 0x97 inner from split frames (first 0x97 wins).
inline bool extractInner97(const std::vector<CapMsg>& frames,
                           std::vector<uint8_t>& inner, std::string* err) {
    for (const auto& m : frames) {
        if (!m.body.empty() && m.body[0] == 0x97)
            return rbxclient::parseJoinOuter(m.body.data(), m.body.size(),
                                             inner, err);
    }
    if (err)
        *err = "no 0x97 snapshot in capture";
    return false;
}

} // namespace repltool
