// unit_batch.cpp — pinned against the labeled native capture (2026-11-05).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "send/entity_batch.h"

#include "solve/replicate.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static std::string hex(const std::vector<uint8_t>& v) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (uint8_t b : v) { s += h[b >> 4]; s += h[b & 0xF]; }
    return s;
}

TEST_CASE("event batch: string arg, byte-exact") {
    // Captured: ref 0x0002993c, event 0x11bb, argc 1, string tag 02 len 0e.
    const auto b = rbxclient::batch::event_batch(
        0x0002993c, 0x11bb, {rbxclient::batch::arg_string("STRINGARG_0001")});
    CHECK(hex(b) ==
          "8307013c990200bb1101020e535452494e474152475f30303031" "00");
}

TEST_CASE("event batch: number, bool, vector3, no-arg") {
    using namespace rbxclient::batch;
    CHECK(hex(event_batch(1, 2, {arg_number(1234.5)})) ==
          "830701010000000200010c00000000004a934000");
    CHECK(hex(event_batch(1, 2, {arg_bool(true)})) ==
          "83070101000000020001090100");
    CHECK(hex(event_batch(1, 2, {arg_vector3(1, 2, 3)})) ==
          "83070101000000020001160000803f000000400000404000");
    CHECK(hex(event_batch(1, 2, {})) == "8307010100000002000000");
}

TEST_CASE("vector3 property batch matches the captured move write") {
    // Captured: 83 03 01 9e f8 3a 22 08 0f <0,0,-1> 00
    const auto b = rbxclient::batch::vector3_batch(0x223af89e, 0x0f08, 0.f, 0.f, -1.f);
    CHECK(hex(b) ==
          "830301" "9ef83a22" "080f" "0000000000000000000080bf" "00");
}

TEST_CASE("event batch: instance arg is tag 1c, leb lo, u32 id") {
    // Captured: 1c a6 01 25 6c 06 00 (executor-local Part, lo 0xa6).
    using namespace rbxclient::batch;
    CHECK(hex(event_batch(1, 2, {arg_instance(0x00066c25, 0xa6)})) ==
          "830701010000000200011ca601256c060000");
}

TEST_CASE("ready marker matches the native 12-byte 0x83/04") {
    // Captured: 83 04 d6 09 00 00 | 23 62 00 00 00 | 00 (place 189707).
    const auto b = rbxclient::batch::ready_batch(0x09d6);
    CHECK(b.size() == 12);
    CHECK(hex(b) == "8304d609000023d609000000");
}

TEST_CASE("physics06 carries the three clocks in the 49-byte shape") {
    const uint32_t before = rbxclient::batch::net_clock_ms();
    const auto b = rbxclient::batch::physics06_batch(0x03c37ce2);
    const uint32_t after = rbxclient::batch::net_clock_ms();
    REQUIRE(b.size() == 49);
    CHECK(b[0] == 0x83);
    CHECK(b[1] == 0x06);
    CHECK(b[2] == 0x01);
    auto u32 = [&](size_t o) {
        return uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8) |
               (uint32_t(b[o + 2]) << 16) | (uint32_t(b[o + 3]) << 24);
    };
    CHECK(u32(3) == 0x03c37ce2u);
    CHECK(u32(11) == 0x03c37ce2u);
    const uint32_t a = u32(15), c = u32(19), d = u32(23);
    CHECK(a >= before);
    CHECK(a <= after);
    CHECK(c >= a);
    CHECK(d >= c);
    CHECK(b[47] == 0x01);
    CHECK(b[48] == 0x00);
}

TEST_CASE("client spawn round-trips through the spawn parser") {
    // Upstream 0x83/02 must be exactly what the inbound parser accepts:
    // 83 02 | 01 <u32 ref> | u16 class | FF FF | 01 <u32 parent> | 00.
    rbxclient::JoinSchema s;
    s.classes.resize(2);
    s.classes[0].name = "Model";
    s.classes[1].name = "Part";
    const auto b = rbxclient::batch::client_spawn(0x00b00000u, 1, 0x00001234u);
    rbxclient::Walk83 w;
    std::string err;
    REQUIRE(rbxclient::walk83ItemsEx(b.data(), b.size(), s, nullptr, w, &err));
    CHECK(w.complete);
    REQUIRE(w.items.size() == 1);
    CHECK(w.items[0].type == 0x02);
    const auto& sp = w.items[0].spawn;
    CHECK(sp.lo == 1);
    CHECK(sp.id == 0x00b00000u);
    CHECK(sp.cls == 1);
    CHECK(sp.plo == 1);
    CHECK(sp.parent == 0x1234);
    CHECK(sp.props_a.empty());
    CHECK(sp.props_b.empty());
}

TEST_CASE("cframe property write uses the tag-27 rotation table") {
    // 0x83/03 Part.CFrame (wire prop 5446 on this schema): 12B position +
    // kind 2 (rotation table +X/+Y/cross == identity). Kind 0 would need a
    // 6B quantized quaternion, and a 36B matrix is the tag-26 shape.
    rbxclient::JoinSchema s;
    s.classes.resize(1);
    s.classes[0].name = "Part";
    rbxclient::PropTagTable t;
    t.tag[5446] = 27;
    const auto b =
        rbxclient::batch::cframe_batch(0x00b00001u, 5446, 1.0f, 2.0f, 3.0f);
    rbxclient::Walk83 w;
    std::string err;
    REQUIRE_MESSAGE(rbxclient::walk83ItemsEx(b.data(), b.size(), s, &t, w, &err),
                    err);
    CHECK(w.complete);
    REQUIRE(w.items.size() == 1);
    CHECK(w.items[0].type == 0x03);
    CHECK(w.items[0].prop03.id == 0x00b00001u);
    CHECK(w.items[0].prop03.prop == 5446);
    CHECK(w.items[0].prop03.tag == 27);
    REQUIRE(w.items[0].prop03.value.size() == 13);
    CHECK(w.items[0].prop03.value[12] == 0x02);
}

TEST_CASE("net clock is monotonic and rebases to the session base") {
    const uint32_t t0 = rbxclient::batch::net_clock_ms();
    const uint32_t t1 = rbxclient::batch::net_clock_ms();
    CHECK(t1 >= t0);
    rbxclient::batch::rebase_net_clock(2730);
    const uint32_t t2 = rbxclient::batch::net_clock_ms();
    CHECK(t2 >= 2730);
    CHECK(t2 < 2730 + 5000);
}
