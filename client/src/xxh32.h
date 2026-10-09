// xxh32.h — XXHash32 (https://github.com/Cyan4973/xxHash, BSD-2-Clause).
//
// Only the streaming-core algorithm is needed (one-shot, seedable) — it is
// implemented here to keep the client dependency-free; correctness is proven
// by the challenge blobs themselves: a decode only passes if
// xxh32(plaintext, 0x2A) equals the key embedded in the blob, and all
// captured datasets decode.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rbxclient {

namespace detail {

inline uint32_t rotl32(uint32_t x, int r) {
    return (x << r) | (x >> (32 - r));
}

// Little-endian 32-bit read, alignment-safe.
inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

} // namespace detail

inline uint32_t xxh32(const uint8_t* data, size_t n, uint32_t seed = 0) {
    constexpr uint32_t P1 = 0x9E3779B1u;
    constexpr uint32_t P2 = 0x85EBCA77u;
    constexpr uint32_t P3 = 0xC2B2AE3Du;
    constexpr uint32_t P4 = 0x27D4EB2Fu;
    constexpr uint32_t P5 = 0x165667B1u;
    using detail::rotl32;
    using detail::rd32;

    size_t i = 0;
    uint32_t h;
    if (n >= 16) {
        uint32_t v1 = seed + P1 + P2;
        uint32_t v2 = seed + P2;
        uint32_t v3 = seed;
        uint32_t v4 = seed - P1;
        while (i + 16 <= n) {
            v1 = rotl32(v1 + rd32(data + i) * P2, 13) * P1; i += 4;
            v2 = rotl32(v2 + rd32(data + i) * P2, 13) * P1; i += 4;
            v3 = rotl32(v3 + rd32(data + i) * P2, 13) * P1; i += 4;
            v4 = rotl32(v4 + rd32(data + i) * P2, 13) * P1; i += 4;
        }
        h = rotl32(v1, 1) + rotl32(v2, 7) + rotl32(v3, 12) + rotl32(v4, 18);
    } else {
        h = seed + P5;
    }
    h += (uint32_t)n;
    while (i + 4 <= n) {
        h = (rotl32(h + rd32(data + i) * P3, 17)) * P4;
        i += 4;
    }
    while (i < n) {
        h = rotl32(h + (uint32_t)data[i] * P5, 11) * P1;
        i += 1;
    }
    h ^= h >> 15;
    h *= P2;
    h ^= h >> 13;
    h *= P3;
    h ^= h >> 16;
    return h;
}

} // namespace rbxclient
