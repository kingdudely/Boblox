// unit_rupp.cpp — RUPP datagram framing (client/src/rupp.h).
//
// Golden: a native-captured game-path datagram (run/quicdump/udp/rupp_tx001.bin,
// tracked) — parsing it and re-wrapping its payload with the extracted header
// must reproduce the capture byte for byte, which pins the exact wire format
// (31-byte header, TLV layout, big-endian total/port) against reality.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "rupp.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;
using rbx::RUPP_HEADER_LEN;
using rbx::RuppHeader;

namespace {

std::vector<uint8_t> slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    REQUIRE(f);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("native capture: strip + re-wrap is byte-exact") {
    const std::vector<uint8_t> pkt =
        slurp(fs::path(REPO_ROOT) / "run" / "quicdump" / "udp" / "rupp_tx001.bin");
    REQUIRE(pkt.size() > RUPP_HEADER_LEN);

    size_t plen = 0;
    const uint8_t* payload = rbx::rupp_strip(pkt.data(), pkt.size(), &plen);
    REQUIRE(payload != nullptr);
    CHECK(plen == pkt.size() - RUPP_HEADER_LEN); // total field = 31 (header only)
    CHECK(payload == pkt.data() + RUPP_HEADER_LEN);

    // extract the header per the documented TLV layout (offsets from rupp.h)
    RuppHeader h;
    std::memcpy(h.token, pkt.data() + 7, 16);   // 01 11 01 <16B token>
    REQUIRE(pkt[23] == 0x02);                   // 02 06 <ip><port>
    REQUIRE(pkt[24] == 0x06);
    std::memcpy(h.rcc_ip, pkt.data() + 25, 4);
    h.rcc_port = uint16_t((pkt[29] << 8) | pkt[30]);
    CHECK(h.rcc_port == 61269); // documented NetStackPort of the capture

    std::vector<uint8_t> out(RUPP_HEADER_LEN + plen);
    const size_t n = rbx::rupp_wrap(out.data(), out.size(), h, payload, plen);
    REQUIRE(n == pkt.size());
    CHECK(out == pkt); // byte-exact round-trip of the native datagram
}

TEST_CASE("wrap/strip round-trip on a synthetic payload") {
    const std::vector<uint8_t> payload(1200, 0xAB);
    RuppHeader h;
    for (int i = 0; i < 16; ++i)
        h.token[i] = uint8_t(i + 1);
    const uint8_t ip[4] = {10, 0, 0, 1};
    std::memcpy(h.rcc_ip, ip, 4);
    h.rcc_port = 12345;

    std::vector<uint8_t> out(RUPP_HEADER_LEN + payload.size());
    const size_t n = rbx::rupp_wrap(out.data(), out.size(), h, payload.data(), payload.size());
    REQUIRE(n == RUPP_HEADER_LEN + payload.size());

    size_t plen = 0;
    const uint8_t* p = rbx::rupp_strip(out.data(), out.size(), &plen);
    REQUIRE(p != nullptr);
    CHECK(plen == payload.size());
    CHECK(std::vector<uint8_t>(p, p + plen) == payload);
}

TEST_CASE("wrap refuses undersized buffers") {
    RuppHeader h;
    const uint8_t payload[8] = {};
    CHECK(rbx::rupp_wrap(nullptr, 0, h, payload, sizeof(payload)) == 0);
    std::vector<uint8_t> tiny(RUPP_HEADER_LEN + 7);
    CHECK(rbx::rupp_wrap(tiny.data(), tiny.size(), h, payload, sizeof(payload)) == 0);
}

TEST_CASE("strip drops malformed datagrams") {
    size_t plen = 0;
    const std::vector<uint8_t> empty = {0x01, 0x00, 0x00, 0x1f}; // < 31 + payload
    CHECK(rbx::rupp_strip(empty.data(), empty.size(), &plen) == nullptr);

    const std::vector<uint8_t> notrupp(40, 0x00); // [0] != 0x01
    CHECK(rbx::rupp_strip(notrupp.data(), notrupp.size(), &plen) == nullptr);

    const std::vector<uint8_t> total_oob = {0x01, 0x00, 0xFF, 0xFF, 0x00}; // total > len
    CHECK(rbx::rupp_strip(total_oob.data(), total_oob.size(), &plen) == nullptr);

    const std::vector<uint8_t> tiny = {0x01}; // len < 4
    CHECK(rbx::rupp_strip(tiny.data(), tiny.size(), &plen) == nullptr);
}
