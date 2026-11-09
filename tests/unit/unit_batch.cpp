// unit_batch.cpp — pinned against the labeled native capture (2026-11-05).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "send/entity_batch.h"

#include "solve/replicate.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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
    // The 04 field is the millisecond clock, the 23 item the 240 Hz tick: the
    // reference frame carries 2518 and 98, not one number twice.
    const auto b = rbxclient::batch::ready_batch(0x09d6, 0x62);
    CHECK(b.size() == 12);
    CHECK(hex(b) == "8304d6090000236200000000");
}

TEST_CASE("clock frame is a 7-byte 0x83/23 record") {
    // Captured: 83 23 30 01 00 00 00 (tick 0x130).
    const auto b = rbxclient::batch::clock_batch(0x130);
    REQUIRE(b.size() == 7);
    CHECK(hex(b) == "83233001000000");
}

TEST_CASE("net tick runs at 240 Hz on the millisecond clock's origin") {
    const uint32_t ms0 = rbxclient::batch::net_clock_ms();
    const uint32_t t0 = rbxclient::batch::net_tick_240();
    const uint32_t ms1 = rbxclient::batch::net_clock_ms();
    const uint32_t t1 = rbxclient::batch::net_tick_240();
    CHECK(t1 >= t0);
    // Same origin: tick = ms * 0.24 (the measured 239.1/s slope).
    const uint32_t expect = uint32_t((uint64_t(ms1) * 240u) / 1000u);
    CHECK(t1 == expect);
    CHECK(t1 >= uint32_t((uint64_t(ms0) * 240u) / 1000u));
}

TEST_CASE("physics upload puts the position at offset 19, byte-exact") {
    // Captured (2026-11-08, place 1818, first 0x83/05 of an 811 s session):
    //   83 05 16 | 5659 | 0 | 5659 | 0 | 31.3209 14.6639 0 | 0 | 01
    //            | 5660 | 686.52 | 00
    // The two zero words around the clock pair are what put the position at
    // offset 19; the earlier layout placed it at 15 and is pinned against here.
    const auto b = rbxclient::batch::physics_batch_clocked(
        0x161b, 0x161b, 0x161c, 31.320852f, 14.663868f, 0.0f, 0u, 686.5234375f);
    REQUIRE(b.size() == 45);
    CHECK(hex(b) ==
          "830516" "1b160000" "00000000" "1b160000" "00000000"
          "1b91fa41" "349f6a41" "00000000" "00000000" "01"
          "1c160000" "80a12b44" "00");
    // Field accessors, so a future reorder cannot silently pass by matching
    // only the length: x/y/z must decode back out of offsets 19/23/27.
    auto f32 = [&](size_t o) {
        uint32_t v = uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8) |
                     (uint32_t(b[o + 2]) << 16) | (uint32_t(b[o + 3]) << 24);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    };
    CHECK(f32(19) == 31.320852f);
    CHECK(f32(23) == 14.663868f);
    CHECK(f32(27) == 0.0f);
    CHECK(f32(40) == 686.5234375f);
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

TEST_CASE("ref property write uses the tag-28 scope+id shape") {
    rbxclient::JoinSchema s;
    s.classes.resize(1);
    s.classes[0].name = "Player";
    rbxclient::PropTagTable t;
    t.tag[6073] = 28;
    const auto b = rbxclient::batch::ref_batch(0x00123456u, 6073, 0x00789abcu);
    rbxclient::Walk83 w;
    std::string err;
    REQUIRE_MESSAGE(rbxclient::walk83ItemsEx(b.data(), b.size(), s, &t, w, &err),
                    err);
    CHECK(w.complete);
    REQUIRE(w.items.size() == 1);
    CHECK(w.items[0].type == 0x03);
    CHECK(w.items[0].prop03.id == 0x00123456u);
    CHECK(w.items[0].prop03.prop == 6073);
    CHECK(w.items[0].prop03.tag == 28);
    CHECK(w.items[0].prop03.vfmt == 0x01);
    REQUIRE(w.items[0].prop03.value.size() == 5);
    CHECK(w.items[0].prop03.value[0] == 0x01);
    CHECK(w.items[0].prop03.value[1] == 0xbc);
    CHECK(w.items[0].prop03.value[2] == 0x9a);
    CHECK(w.items[0].prop03.value[3] == 0x78);
    CHECK(w.items[0].prop03.value[4] == 0x00);
}

// --- client -> server 0xA1 (InstanceStateSender) -------------------------
//
// Every vector below is a whole native frame copied out of the 700 s same-job
// capture run/parity/r7e1 (build 2.743.1136, place 1818). All 149 A1 frames in
// that capture close exactly under this grammar, and the four shapes below are
// one of each.
static std::vector<uint8_t> f32le(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    return {uint8_t(bits), uint8_t(bits >> 8), uint8_t(bits >> 16),
            uint8_t(bits >> 24)};
}

static std::vector<uint8_t> bytes_of(std::initializer_list<uint8_t> v) {
    return std::vector<uint8_t>(v);
}

TEST_CASE("A1 idle frame is 13 bytes: tick, records=0, ack") {
    const auto b = rbxclient::batch::isr_empty(0x70c, 0xfffe6d3cu);
    CHECK(hex(b) == "a10c070000000000003c6dfeff");
    CHECK(b.size() == 13);
}

TEST_CASE("A1 Player.CameraViewportSize matches the native 44-byte frame") {
    // a1 7c000000 01000000 786ffeff 01000000 | 01 055d0100 | 7f01 | 0900
    //   00 0100000000000000 | 0800 | <1366.0, 736.0>
    rbxclient::batch::IsrRecord r;
    r.ref = 0x00015d05;
    r.cls = 383; // Player
    r.member_count = 9;
    r.member_index = 8; // CameraViewportSize, tag 21 (Vector2)
    r.value = f32le(1366.0f);
    const auto rest = f32le(736.0f);
    r.value.insert(r.value.end(), rest.begin(), rest.end());
    const auto b = rbxclient::batch::isr_batch(0x7c, 0xfffe6f78u, {r});
    CHECK(hex(b) ==
          "a17c00000001000000786ffeff01000000" "01" "055d0100" "7f01"
          "0900" "0001000000000000" "0800" "00c0aa4400003844");
    CHECK(b.size() == 44);
}

TEST_CASE("A1 Player Vector3 member matches the native 48-byte frame") {
    rbxclient::batch::IsrRecord r;
    r.ref = 0x00015d05;
    r.cls = 383;
    r.member_count = 12;
    r.member_index = 11;
    for (int i = 0; i < 3; ++i) {
        const auto f = f32le(3.402823466e38f); // FLT_MAX, as captured
        r.value.insert(r.value.end(), f.begin(), f.end());
    }
    const auto b = rbxclient::batch::isr_batch(0xac, 0xfffe6e50u, {r});
    CHECK(hex(b) ==
          "a1ac00000001000000506efeff01000000" "01" "055d0100" "7f01"
          "0c00" "0008000000000000" "0c00"
          "ffff7f7fffff7f7fffff7f7f");
    CHECK(b.size() == 48);
}

TEST_CASE("A1 Humanoid enum member matches the native 37-byte frame") {
    // Humanoid.NetworkHumanoidState = 15 (Physics) on a client standing still.
    rbxclient::batch::IsrRecord r;
    r.ref = 0x00015e44;
    r.cls = 266; // Humanoid
    r.member_count = 48;
    r.member_index = 28; // the enum member the capture's mask bit selects
    r.value = bytes_of({0x0f});
    const auto b = rbxclient::batch::isr_batch(0xa66c, 0xfffe6d34u, {r});
    CHECK(hex(b) ==
          "a16ca6000001000000346dfeff01000000" "01" "445e0100" "0a01"
          "3000" "0000001000000000" "0100" "0f");
    CHECK(b.size() == 37);
}

TEST_CASE("A1 frames round-trip through the 0xA1 receiver") {
    rbxclient::JoinSchema s;
    s.classes.resize(384);
    s.classes[383].name = "Player";
    s.classes[383].members.resize(9);
    s.classes[383].members[8].name = "CameraViewportSize";
    s.classes[383].members[8].tag = 21;
    rbxclient::batch::IsrRecord r;
    r.ref = 0x00015d05;
    r.cls = 383;
    r.member_count = 9;
    r.member_index = 8;
    r.value = f32le(1366.0f);
    const auto rest = f32le(736.0f);
    r.value.insert(r.value.end(), rest.begin(), rest.end());
    const auto b = rbxclient::batch::isr_batch(0x7c, 0, {r});
    rbxclient::AppStateFrame f;
    std::string err;
    REQUIRE_MESSAGE(rbxclient::parseAppStateFrame(b.data(), b.size(), s, &f,
                                                  &err),
                    err);
    CHECK(f.header.records == 1);
    REQUIRE(f.records.size() == 1);
    CHECK(f.records[0].id == 0x00015d05u);
    CHECK(f.records[0].cls == 383);
    CHECK(f.records[0].member_count == 9);
    CHECK(f.records[0].mask_words[0] == (uint64_t(1) << 8));
    REQUIRE(f.records[0].data.size() == 10); // u16 length + 8 value bytes
    CHECK(f.records[0].data[0] == 8);
    CHECK(f.records[0].data[1] == 0);
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
