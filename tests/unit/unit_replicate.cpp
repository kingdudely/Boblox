// unit_replicate.cpp — replication frame parsers vs frozen captures.
//
// Goldens in tests/vectors/repl/ come from live Crossroads 1818 sessions
// (our rbxplay capture + a native mocktail capture; see
// tools/repl_survey.py and FINDINGS.md "Replication capture + frame
// survey"). The 0x97 snapshot is byte-identical across instances, so
// these goldens are stable, not session-bound.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "replicate.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    REQUIRE(f);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
}

fs::path vec(const std::string& n) {
    return fs::path(REPO_ROOT) / "tests" / "vectors" / "repl" / n;
}

// Split a varint-framed message file into its body. Stream slices carry
// the length prefix; native gdb dumps start at the id byte directly.
const uint8_t* frame_body(const std::vector<uint8_t>& f, bool prefixed,
                          size_t* len) {
    REQUIRE(!f.empty());
    if (!prefixed) {
        *len = f.size();
        return f.data();
    }
    const size_t k = size_t(1) << (f[0] >> 6);
    REQUIRE(k < f.size());
    *len = f.size() - k;
    return f.data() + k;
}

} // namespace

TEST_CASE("frame walk: real stream head walks clean") {
    const auto d = slurp(vec("stream_head.bin"));
    std::vector<rbxclient::ReplMessage> msgs;
    std::string err;
    REQUIRE(rbxclient::walkReplFrames(d.data(), d.size(), msgs, &err));
    REQUIRE(msgs.size() >= 6);
    // Stream opens 0x93, 0x9B, 0x97, 0x81, 0x84, then 0x83 deltas.
    CHECK(msgs[0].id == 0x93);
    CHECK(msgs[1].id == 0x9B);
    CHECK(msgs[2].id == 0x97);
    CHECK(msgs[3].id == 0x81);
    CHECK(msgs[4].id == 0x84);
    CHECK(msgs[5].id == 0x83);
    // Every message round-trips its id as the first body byte.
    for (const auto& m : msgs)
        CHECK(m.body[0] == m.id);
}

TEST_CASE("0x97 outer: header validates, zstd matches frozen inner") {
    const auto frame = slurp(vec("frame97.bin"));
    size_t blen = 0;
    const uint8_t* body = frame_body(frame, true, &blen);
    REQUIRE(body[0] == 0x97);
    std::vector<uint8_t> inner;
    std::string err;
    const bool ok = rbxclient::parseJoinOuter(body, blen, inner, &err);
    INFO("join outer err: " << err);
    REQUIRE(ok);
    CHECK(inner.size() == 352276);
    const auto golden =
        slurp(fs::path(REPO_ROOT) / "run" / "join97_1818.raw");
    CHECK(inner == golden);
}

TEST_CASE("0x81 peer init: flags + peer id on both captures") {
    // Ours (peer 696).
    const auto f81 = slurp(vec("frame81.bin"));
    size_t blen = 0;
    const uint8_t* fbody = frame_body(f81, true, &blen);
    rbxclient::PeerInit p;
    std::string err;
    REQUIRE(rbxclient::parsePeerInit(fbody, blen, p, &err));
    CHECK(p.ena == 1);
    CHECK(p.isr_read == 1);
    CHECK(p.isr_write == 0);
    CHECK(p.peer == 696);
    CHECK(!p.tail.empty());
    // Native (peer 31 == its "Assigned peer id" log line).
    const auto n81 = slurp(fs::path(REPO_ROOT) / "run" / "repl_native81.bin");
    size_t nlen = 0;
    const uint8_t* nbody = frame_body(n81, false, &nlen);
    rbxclient::PeerInit q;
    REQUIRE(rbxclient::parsePeerInit(nbody, nlen, q, &err));
    CHECK(q.peer == 31);
    CHECK(q.ena == p.ena);
    CHECK(q.isr_read == p.isr_read);
    CHECK(q.isr_write == p.isr_write);
    // Tails carry session-varying fields after a 29B constant run
    // (v60 u32 + flags); pin sizes + the constant prefix only.
    REQUIRE(q.tail.size() == p.tail.size());
    CHECK(std::vector<uint8_t>(q.tail.begin() + 4, q.tail.begin() + 33) ==
          std::vector<uint8_t>(p.tail.begin() + 4, p.tail.begin() + 33));
}

TEST_CASE("0x16 batch: lengths assert, inner decompresses") {
    const auto b = slurp(vec("batch16.bin"));
    size_t blen = 0;
    const uint8_t* body = frame_body(b, true, &blen);
    std::vector<uint8_t> inner;
    std::string err;
    REQUIRE(rbxclient::parseDeltaBatch(body, blen, inner, &err));
    CHECK(inner.size() == 24539);
    // Spot-check the head (dense binary property data).
    REQUIRE(inner.size() >= 8);
    const uint8_t head[8] = {0x45, 0x26, 0x40, 0x80, 0x84, 0x3D, 0x8A, 0x8D};
    CHECK(std::vector<uint8_t>(inner.begin(), inner.begin() + 8) ==
          std::vector<uint8_t>(head, head + 8));
}

TEST_CASE("0x07 records: 19B segmentation on the spawn message") {
    const auto m = slurp(vec("msg07.bin"));
    std::vector<rbxclient::PropUpdate> recs;
    std::string err;
    REQUIRE(rbxclient::parsePropUpdates(m.data(), m.size(), recs, &err));
    REQUIRE(recs.size() == 7);
    // All seven target the player entity; first record pinned exactly.
    for (const auto& r : recs)
        CHECK(r.owner == 3638289374u);
    CHECK(recs[0].val == 4005346746u);
    CHECK(recs[0].x == 87);
}

TEST_CASE("0x02 player records: url/entity/name triple") {
    const auto m = slurp(vec("msg02.bin"));
    std::vector<rbxclient::PlayerRec> recs;
    size_t skipped = 0;
    std::string err;
    REQUIRE(rbxclient::parsePlayerRecs(m.data(), m.size(), recs, &skipped,
                                       &err));
    bool found = false;
    for (const auto& r : recs) {
        if (r.entity == 3638289374u) {
            found = true;
            CHECK(r.url.find("4656429295") != std::string::npos);
            CHECK(r.name == "Chat GPT"); // field len is 8; the 9th byte
                                         // starts the next field
        }
    }
    CHECK(found); // the spawning player's record resolves fully
}
