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

std::vector<uint8_t> controlBlock(int slot, uint8_t tag,
                                  std::vector<uint8_t> value) {
    if (slot < 0 || slot > 7)
        return {};
    // Default value: all zeros sized by tag (bool 1B; s32/f32/ref 4B;
    // vec2 8B; vec3-quantized 5B minimal). Unknown tags fail closed.
    if (value.empty()) {
        size_t n = 0;
        switch (tag) {
        case 0:
            n = 1;
            break;
        case 1:
        case 2:
        case 7:
            n = 4;
            break;
        case 3:
            n = 8;
            break;
        case 4:
            n = 5;
            break;
        default:
            return {};
        }
        value.assign(n, 0);
    }
    std::vector<uint8_t> out;
    out.push_back(0x01); // N = one block
    out.insert(out.end(), 4, 0x00); // A (framing-OK hypothesis)
    out.insert(out.end(), 4, 0x00); // B (same)
    const uint16_t inner = uint16_t(1 + value.size()); // presence + value
    out.push_back(uint8_t(inner & 0xFF));
    out.push_back(uint8_t((inner >> 8) & 0xFF));
    out.push_back(uint8_t(1u << slot)); // presence, bit0 = slot0 LSB-first
    out.insert(out.end(), value.begin(), value.end());
    return out;
}

std::vector<uint8_t> withControl(int idx,
                                 const std::vector<uint8_t>& block) {
    std::vector<uint8_t> v = routeSample(idx);
    if (v.empty() || block.empty())
        return {};
    v.pop_back(); // drop the idle N=00 the block replaces
    v.insert(v.end(), block.begin(), block.end());
    return v;
}

} // namespace send
} // namespace rbx
