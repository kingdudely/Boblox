// unit_vectors.cpp — golden per-stage vectors (tests/vectors/) through the
// C++ pipeline: extract -> decode -> standardize -> solve.
//
// The vectors are derived from the tracked native captures by
// tools/gen_vectors.py (which ctest `vectors-py` verifies stays in sync), so
// each stage is byte-compared against a frozen golden — a failure pinpoints
// WHICH stage drifted, independently of the solver regressions.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "blob.h"
#include "solve.h"
#include "standardize.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::vector<uint8_t> slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    REQUIRE(f);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
}

json slurp_json(const fs::path& p) {
    std::ifstream f(p);
    REQUIRE(f);
    json j;
    f >> j;
    return j;
}

} // namespace

TEST_CASE("golden vectors: extract -> decode -> standardize -> solve") {
    const fs::path vecDir = fs::path(REPO_ROOT) / "tests" / "vectors";
    REQUIRE(fs::exists(vecDir));

    int datasets = 0;
    for (const auto& entry : fs::directory_iterator(vecDir)) {
        if (entry.path().extension() != ".json")
            continue;
        const json meta = slurp_json(entry.path());
        const std::string name = meta.at("name").get<std::string>();
        INFO("dataset: " << name);
        datasets++;

        // input: the tracked native capture the vector was derived from
        const std::vector<uint8_t> msg =
            slurp(fs::path(REPO_ROOT) / meta.at("source").get<std::string>());
        REQUIRE(msg.size() == meta.at("msg_len").get<size_t>());

        // stage 1: [9b][u1][u2][len][blob]
        rbxclient::Challenge ch;
        std::string err;
        REQUIRE(rbxclient::extractChallenge(msg.data(), msg.size(), ch, &err));
        CHECK(ch.u1 == meta.at("u1").get<uint32_t>());
        CHECK(ch.u2 == meta.at("u2").get<uint32_t>());

        // stage 2: RSB1 xor + xxhash32(0x2A) + zstd -> wire program
        std::vector<uint8_t> wire;
        REQUIRE(rbxclient::decodeBlob(ch.blob.data(), ch.blob.size(), wire, &err));
        CHECK(wire == slurp(vecDir / (name + ".wire.bin")));

        // stage 3: opcode standardization (in place)
        rbxclient::Stats st;
        REQUIRE(rbxclient::standardize(wire, &st, &err));
        CHECK_FALSE(st.bad_walk);
        CHECK(wire == slurp(vecDir / (name + ".standard.bin")));

        // stage 4: full solve vs the answer the NATIVE sent (when recorded)
        if (!meta.at("answer").is_null() && !meta.at("job").is_null()) {
            uint32_t got = 0;
            REQUIRE(rbxclient::solveMessage(msg.data(), msg.size(),
                                            meta.at("job").get<std::string>(), got, &err));
            CHECK(got == meta.at("answer").get<uint32_t>());
        }
    }
    // guard: a path/permission mistake must not read as "all passed"
    CHECK(datasets >= 6);
}
