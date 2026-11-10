// unit_send.cpp — the upstream stream (client -> RCC).
//
// The frames are pinned byte-exact against input.cpp, which is the module the
// live captures produced: a regression here means the client is emitting a
// shape the RCC has never accepted (0x15 01 04 territory).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "common/hex.h"
#include "raknet/input.h"
#include "raknet/send.h"

#include <chrono>
#include <cstdint>
#include <vector>

namespace {

struct FakeSender : rbxclient::UpstreamSender {
    std::vector<std::vector<uint8_t>> unreliable;
    std::vector<std::vector<uint8_t>> ordered;
    bool fail = false;

    bool send_unreliable(const uint8_t* d, size_t n) override {
        if (fail) return false;
        unreliable.emplace_back(d, d + n);
        return true;
    }
    bool send_ordered(const uint8_t* d, size_t n) override {
        if (fail) return false;
        ordered.emplace_back(d, d + n);
        return true;
    }
};

using clock = std::chrono::steady_clock;

rbxclient::SendPath make(FakeSender* tx, int ms = 75) {
    rbxclient::SendConfig cfg;
    cfg.input_ms = ms;
    return rbxclient::SendPath(tx, cfg, [](const std::string&) {});
}

} // namespace

TEST_CASE("send: the first frame is the native route frame, byte-exact") {
    FakeSender tx;
    auto path = make(&tx);
    const auto t0 = clock::now();
    CHECK(path.tick(t0));
    REQUIRE(tx.unreliable.size() == 1);
    CHECK(tx.unreliable[0] == rbxclient::input::routeFrame(0, 0x14c));
    CHECK(path.step() == 0x150);
}

TEST_CASE("send: frame index walks 1,2,3 then stays at the camera frame") {
    FakeSender tx;
    auto path = make(&tx);
    const auto t0 = clock::now();
    for (int i = 0; i < 5; i++) path.tick(t0 + std::chrono::milliseconds(75 * i));
    REQUIRE(tx.unreliable.size() == 5);
    for (int i = 0; i < 3; i++)
        CHECK(tx.unreliable[size_t(i)] ==
              rbxclient::input::routeFrame(i, 0x14c + 4 * uint32_t(i)));
    CHECK(tx.unreliable[3] == rbxclient::input::routeFrame(3, 0x14c + 12));
    CHECK(tx.unreliable[4] == rbxclient::input::routeFrame(3, 0x14c + 16));
}

TEST_CASE("send: cadence is the native 75 ms, no catch-up burst") {
    FakeSender tx;
    auto path = make(&tx);
    const auto t0 = clock::now();
    CHECK(path.tick(t0));
    CHECK_FALSE(path.tick(t0 + std::chrono::milliseconds(10)));
    CHECK_FALSE(path.tick(t0 + std::chrono::milliseconds(74)));
    CHECK(path.tick(t0 + std::chrono::milliseconds(75)));
    CHECK(tx.unreliable.size() == 2);
    // A long stall must send one frame, not the missed backlog.
    CHECK(path.tick(t0 + std::chrono::seconds(5)));
    CHECK(tx.unreliable.size() == 3);
}

TEST_CASE("send: hold puts the ControlState block on the last sub-frame") {
    FakeSender tx;
    auto path = make(&tx);
    path.hold(0x01, 0xFF);
    const auto block = rbxclient::input::controlBlock(0x01, 0xFF);
    REQUIRE(block.size() > 0);
    CHECK(path.next_control_block() == block);

    const auto t0 = clock::now();
    CHECK(path.tick(t0));
    REQUIRE(tx.unreliable.size() == 1);
    const auto& f = tx.unreliable[0];
    // Empty route frame with its final nActions byte replaced.
    std::vector<uint8_t> expected0 = rbxclient::input::routeFrame(0, 0x14c);
    expected0.pop_back();
    expected0.insert(expected0.end(), block.begin(), block.end());
    CHECK(f == expected0);
    CHECK(path.controls_sent() == 1);
    CHECK(path.held());

    // Sustained: the next frame carries it too (count=2 this time).
    CHECK(path.tick(t0 + std::chrono::milliseconds(75)));
    REQUIRE(tx.unreliable.size() == 2);
    std::vector<uint8_t> expected1 = rbxclient::input::routeFrame(1, 0x150);
    expected1.pop_back();
    expected1.insert(expected1.end(), block.begin(), block.end());
    CHECK(tx.unreliable[1] == expected1);
}

TEST_CASE("send: every-frame placement repeats the block") {
    FakeSender tx;
    auto path = make(&tx);
    path.set_placement(rbxclient::SendPath::Placement::EveryFrame);
    path.hold(0x02, 0x00);
    const auto block = rbxclient::input::controlBlock(0x02, 0x00);
    const auto t0 = clock::now();
    // Third frame has count=3: three sub-frames, three blocks.
    for (int i = 0; i < 3; i++) path.tick(t0 + std::chrono::milliseconds(75 * i));
    REQUIRE(tx.unreliable.size() == 3);
    const auto& f = tx.unreliable[2];
    size_t hits = 0;
    for (size_t i = 0; i + block.size() <= f.size(); i++) {
        bool same = true;
        for (size_t k = 0; k < block.size(); k++)
            if (f[i + k] != block[k]) { same = false; break; }
        if (same) hits++;
    }
    CHECK(hits == 3);
}

TEST_CASE("send: release clears the block, one-shot control applies once") {
    FakeSender tx;
    auto path = make(&tx);
    const auto t0 = clock::now();
    path.hold(0x04, 0xFF);
    CHECK(path.tick(t0));
    path.release();
    CHECK_FALSE(path.held());
    CHECK(path.next_control_block().empty());

    path.queue_control(0x08, 0x00);
    CHECK(path.tick(t0 + std::chrono::milliseconds(75)));
    CHECK(path.next_control_block().empty());
    CHECK(path.tick(t0 + std::chrono::milliseconds(150)));
    // Both one-shot frames differ from the empty route shape.
    CHECK(tx.unreliable[1] != rbxclient::input::routeFrame(1, 0x150));
    CHECK(tx.unreliable[2] == rbxclient::input::routeFrame(2, 0x154));
}

TEST_CASE("send: raw messages reach the transport with the right reliability") {
    FakeSender tx;
    auto path = make(&tx);
    const uint8_t a[] = {0x04, 0x07, 0x00, 0x00, 0x00};
    CHECK(path.raw(a, sizeof a, false));
    CHECK(path.raw(a, sizeof a, true));
    REQUIRE(tx.unreliable.size() == 1);
    REQUIRE(tx.ordered.size() == 1);
    CHECK(tx.unreliable[0] == std::vector<uint8_t>(a, a + sizeof a));
    CHECK(tx.ordered[0] == std::vector<uint8_t>(a, a + sizeof a));
    CHECK(path.raw_sent() == 2);
}

TEST_CASE("send: transport failure is counted, not swallowed") {
    FakeSender tx;
    auto path = make(&tx);
    tx.fail = true;
    CHECK_FALSE(path.tick(clock::now()));
    CHECK(path.send_failures() == 1);
    CHECK(path.frames_sent() == 0);
    // The step must not advance on a failed send.
    CHECK(path.step() == 0x14c);
}

TEST_CASE("input: 141-byte long frame is byte-exact against the native capture") {
    // r7e1 native, 4 0xA6 ControlState actions per frame. The header keeps
    // the 14-byte route, the step pair, and flag 0x0c; each frame is
    // `02` + two 13-byte UnitVector3 actions, separated by `04 04`.
    const std::string capture =
        "a604010010880ba4538d70015ae6d7ff57f544ab0000083e02000c"
        "027f5d01000400c57c440045e043115d01000400c57c440045e0430404"
        "027f5d01000400057d4400c5e043115d01000400057d4400c5e0430404"
        "027f5d01000400c57d440045e143115d01000400c57d440045e1430404"
        "027f5d01000400457e4400c5e143115d01000400457e4400c5e143";
    const auto want = rbx::hexDecode(capture);
    REQUIRE(want.size() == 141);

    const uint8_t route[14] = {0x10, 0x88, 0x0b, 0xa4, 0x53, 0x8d, 0x70,
                               0x01, 0x5a, 0xe6, 0xd7, 0xff, 0x57, 0xf5};
    const int16_t dirs[4][3] = {{31941, 68, -8123},
                                {32005, 68, -7995},
                                {32197, 68, -7867},
                                {32325, 68, -7739}};
    CHECK(rbxclient::input::longFrameQuantized(0xab44, 0x23e08, 0x15d, dirs,
                                               route) == want);

    const double unit[4][3] = {{31941.0 / 32767.0, 68.0 / 32767.0,
                                -8123.0 / 32767.0},
                               {32005.0 / 32767.0, 68.0 / 32767.0,
                                -7995.0 / 32767.0},
                               {32197.0 / 32767.0, 68.0 / 32767.0,
                                -7867.0 / 32767.0},
                               {32325.0 / 32767.0, 68.0 / 32767.0,
                                -7739.0 / 32767.0}};
    CHECK(rbxclient::input::longFrame(0xab44, 0x23e08, 0x15d, unit, route) ==
          want);

    const auto default_route =
        rbxclient::input::longFrame(0, 0, 1, unit);
    CHECK(default_route.size() == 141);
    CHECK(default_route[4] == 0x10);
}

TEST_CASE("send: raw control override wins over hold") {
    FakeSender tx;
    auto path = make(&tx);
    path.hold(0x01, 0xFF);
    path.set_raw_control({0x01, 0xAA});
    CHECK(path.next_control_block() == std::vector<uint8_t>({0x01, 0xAA}));
    path.clear_raw_control();
    CHECK(path.next_control_block() == rbxclient::input::controlBlock(0x01, 0xFF));
}
