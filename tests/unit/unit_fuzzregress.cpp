// unit_fuzzregress.cpp — permanent regressions for the Phase K libFuzzer
// harnesses (fuzz/fuzz_*.cpp, driven by tools/fuzz_all.sh).
//
// Each case is a crashing (or aborting) input found by fuzzing the PRE-FIX
// parsers, pinned here so the fix can never regress: the parsers must
// reject them cleanly (false + error, no crash/hang/throw). The harnesses
// themselves keep exploring for NEW findings on every fuzz run.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "blob.h"
#include "standardize.h"
#include "util.h" // compactVarint (framing the oracle for the walk test)

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
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

std::vector<uint8_t> framed(const std::vector<uint8_t>& msg) {
    std::vector<uint8_t> out;
    rbx::compactVarint(out, msg.size());
    out.insert(out.end(), msg.begin(), msg.end());
    return out;
}

} // namespace

TEST_CASE("extractChallenge rejects a 4GB blob-length claim") {
    // fuzz_blob vs pre-fix code: ln=0xFFFFFFFF wrapped `13u + ln` (32-bit)
    // to 12, blessing a 4GB vector::assign (libFuzzer OOM abort). Now false.
    const uint8_t evil[] = {0x9b, 0x95, 0xdd, 0x29, 0x00, 0x95, 0xdd, 0x29, 0x00,
                            0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0xd9, 0x9b, 0x95, 0xdd, 0x29, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    rbxclient::Challenge ch;
    std::string err;
    CHECK(!rbxclient::extractChallenge(evil, sizeof(evil), ch, &err));
    CHECK(ch.blob.empty());
    CHECK(!err.empty());
}

TEST_CASE("decodeBlob rejects a valid-checksum blob with a 4GB mode") {
    // Forged offline (brute-forced header so xxh32 matches; decoded
    // mode=0xFFFFFFFF): pre-fix code allocated a 4GB zstd-output vector and
    // aborted. Now the cap rejects it before any allocation.
    const uint8_t forged[] = {0xbf, 0xcc, 0xac, 0xba, 0x6e, 0xbc, 0x6d, 0xd0,
                              0x35, 0xe7, 0x36, 0xd3, 0xd9};
    std::vector<uint8_t> wire;
    std::string err;
    CHECK(!rbxclient::decodeBlob(forged, sizeof(forged), wire, &err));
    CHECK(err == "zstd output size unreasonable");
    CHECK(wire.empty());
}

TEST_CASE("decodeBlob rejects short and garbage blobs") {
    std::vector<uint8_t> wire;
    std::string err;
    CHECK(!rbxclient::decodeBlob(nullptr, 0, wire, &err)); // empty
    const uint8_t tiny[] = {0x52, 0x53, 0x42, 0x31};
    CHECK(!rbxclient::decodeBlob(tiny, sizeof(tiny), wire, &err)); // < 13B
    const std::vector<uint8_t> zeros(64, 0);
    CHECK(!rbxclient::decodeBlob(zeros.data(), zeros.size(), wire, &err)); // bad checksum
}

TEST_CASE("standardize rejects the proto-count reserve bomb") {
    // fuzz_standardize vs pre-fix code: protoCount~=165M made
    // protos.reserve() throw length_error (libFuzzer OOM abort). Now false.
    const uint8_t bomb[] = {0x0d, 0x01, 0x02, 0x01, 0xfc, 0x05, 0x01, 0x00, 0x01,
                            0x00, 0x07, 0xa2, 0xaf, 0xec, 0x4e, 0xd9, 0xf1, 0x99};
    std::vector<uint8_t> buf(std::begin(bomb), std::end(bomb));
    rbxclient::Stats st;
    std::string err;
    CHECK(!rbxclient::standardize(buf, &st, &err));
    CHECK(!err.empty());
}

TEST_CASE("standardize rejects an oversized sizecode up front") {
    // Well-formed prefix, then sizeCode=0xFFFFFFFF with no code bytes: the
    // walk's own check would only fire after the loop, so parse rejects it
    // at the door (fast, bounded).
    const uint8_t bad[] = {0x04, 0x00, 0x00, 0x01, 0x09, 0x00, 0x00, 0x00, 0x00,
                           0x00, 0xff, 0xff, 0xff, 0xff, 0x0f};
    std::vector<uint8_t> buf(std::begin(bad), std::end(bad));
    rbxclient::Stats st;
    std::string err;
    CHECK(!rbxclient::standardize(buf, &st, &err));
}

TEST_CASE("standardize still accepts real wire programs") {
    // The guards above must not change the verdict on genuine programs:
    // every tracked wire fixture standardizes cleanly (also covered by
    // solver-regress + unit-vectors; this pins it at the parser level).
    for (const char* name : {"comb1.wire.bin", "comb2.wire.bin", "comb3.wire.bin",
                             "full.wire.bin"}) {
        INFO("fixture: " << name);
        std::vector<uint8_t> buf = slurp(fs::path(REPO_ROOT) / "tests/vectors" / name);
        rbxclient::Stats st;
        std::string err;
        CHECK(rbxclient::standardize(buf, &st, &err));
    }
}

TEST_CASE("findChallengeFrame on the real oracle stream") {
    const std::vector<uint8_t> oracle =
        slurp(fs::path(REPO_ROOT) / "run/oracle_challenge.bin");
    REQUIRE(oracle.size() > 13);
    REQUIRE(oracle[0] == 0x9B);

    const std::vector<uint8_t> fr = framed(oracle);
    size_t off = 0, len = 0;
    CHECK(rbxclient::findChallengeFrame(fr.data(), fr.size(), &off, &len));
    CHECK(off == fr.size() - oracle.size()); // past the length prefix
    CHECK(len == oracle.size());

    // Junk-prefixed stream: still found, at the shifted offset.
    std::vector<uint8_t> junk = {0x05, 'h', 'e', 'l', 'l', 'o'};
    junk.insert(junk.end(), fr.begin(), fr.end());
    CHECK(rbxclient::findChallengeFrame(junk.data(), junk.size(), &off, &len));
    CHECK(off == 6 + fr.size() - oracle.size());
    CHECK(len == oracle.size());

    // Null out-params are legal (presence probe).
    CHECK(rbxclient::findChallengeFrame(fr.data(), fr.size(), nullptr, nullptr));
}

TEST_CASE("findChallengeFrame rejects adversarial streams") {
    size_t off = 0, len = 0;
    CHECK(!rbxclient::findChallengeFrame(nullptr, 0, &off, &len)); // empty

    const std::vector<uint8_t> oracle =
        slurp(fs::path(REPO_ROOT) / "run/oracle_challenge.bin");
    const std::vector<uint8_t> fr = framed(oracle);
    const std::vector<uint8_t> half(fr.begin(), fr.begin() + fr.size() / 2);
    CHECK(!rbxclient::findChallengeFrame(half.data(), half.size(), &off, &len)); // truncated

    const std::vector<uint8_t> marks(64, 0x9B); // maximal varint values, no body
    CHECK(!rbxclient::findChallengeFrame(marks.data(), marks.size(), &off, &len));

    // The exact fuzz_blob OOM input: unframed, must not hang or crash.
    const uint8_t evil[] = {0x9b, 0x95, 0xdd, 0x29, 0x00, 0x95, 0xdd, 0x29, 0x00,
                            0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0xd9, 0x9b, 0x95, 0xdd, 0x29, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(!rbxclient::findChallengeFrame(evil, sizeof(evil), &off, &len));
}
