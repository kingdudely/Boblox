// send.cpp — upstream builders (see send.h).
#include "send.h"

namespace rbx {
namespace send {

namespace {

// Hex-pair decoder (same 12 lines as session.cpp's file-static hx; kept
// local so this TU stays dependency-free).
std::vector<uint8_t> hx(const char* s) {
    std::vector<uint8_t> out;
    auto v = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return 0;
    };
    for (size_t i = 0; s[i] && s[i + 1]; i += 2)
        out.push_back(uint8_t((v(s[i]) << 4) | v(s[i + 1])));
    return out;
}

const char* kRoutes[] = {
    "a60101000114000000140000000000",
    "a60201000118000000180000000000040400",
    "a6030100011c0000001c0000000000040400040400",
    "a60401000120000000200000000000040400040400040400",
};

} // namespace

std::vector<uint8_t> heartbeat() { return {0xA6, 0x00}; }

std::vector<uint8_t> routeSample(int idx) {
    if (idx < 0 || idx > 3)
        return {};
    return hx(kRoutes[idx]);
}

std::vector<uint8_t> routeWithSteps(int idx, size_t off, uint32_t a,
                                    uint32_t b) {
    std::vector<uint8_t> v = routeSample(idx);
    if (v.empty() || off + 8 > v.size())
        return {};
    v[off] = uint8_t(a & 0xFF);
    v[off + 1] = uint8_t((a >> 8) & 0xFF);
    v[off + 2] = uint8_t((a >> 16) & 0xFF);
    v[off + 3] = uint8_t((a >> 24) & 0xFF);
    v[off + 4] = uint8_t(b & 0xFF);
    v[off + 5] = uint8_t((b >> 8) & 0xFF);
    v[off + 6] = uint8_t((b >> 16) & 0xFF);
    v[off + 7] = uint8_t((b >> 24) & 0xFF);
    return v;
}

} // namespace send
} // namespace rbx
