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
#include <cstring>
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

TEST_CASE("0x02 player records: url/entity/name triple") {    const auto m = slurp(vec("msg02.bin"));
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

namespace {

rbxclient::JoinSchema load_schema() {
    const auto inner =
        slurp(fs::path(REPO_ROOT) / "run" / "join97_1818.raw");
    rbxclient::JoinSchema schema;
    std::string err;
    const bool ok = rbxclient::parseJoinSchema(inner.data(), inner.size(),
                                               schema, &err);
    INFO("schema err: " << err);
    REQUIRE(ok);
    return schema;
}

// frameXXX.bin (varint-prefixed 0x83/0x16 frame) -> batch inner -> records.
std::vector<rbxclient::JoinRecord> load_records(const rbxclient::JoinSchema& s,
                                                const std::string& frame) {
    const auto f = slurp(vec(frame));
    size_t blen = 0;
    const uint8_t* body = frame_body(f, true, &blen);
    std::vector<uint8_t> inner;
    std::string err;
    REQUIRE(rbxclient::parseDeltaBatch(body, blen, inner, &err));
    std::vector<rbxclient::JoinRecord> recs;
    REQUIRE(rbxclient::parseJoinRecords(inner.data(), inner.size(), s, recs,
                                        &err));
    return recs;
}

const rbxclient::JoinProp* find_prop(const rbxclient::JoinRecord& r,
                                     const std::string& name) {
    for (const auto& p : r.props)
        if (p.name == name)
            return &p;
    return nullptr;
}

} // namespace

TEST_CASE("joindata schema: version/flags/tables + Sound row") {
    const rbxclient::JoinSchema s = load_schema();
    CHECK(s.version == 3);
    CHECK(s.flags == 15);
    CHECK(s.enums.size() == 602);
    CHECK(s.classes.size() == 630);
    CHECK(s.prop_bound == 10145);
    CHECK(s.ev_bound == 6937);
    CHECK(s.urls.size() == 26);
    CHECK(s.shared.size() == 127);
    // The opaque tail (per-class/id tables) starts right after the shared
    // table; its grammar is still open, so only the extent is pinned.
    CHECK(s.tail_off == 322160);
    CHECK(s.classes[0].name == "Accessory");
    CHECK(s.classes[0].members.size() == 12);
    // Class network ids match schema order (Sound == 482 on the wire).
    REQUIRE(s.classes[482].name == "Sound");
    const auto& members = s.classes[482].members;
    REQUIRE(members.size() == 27);
    CHECK(members[1].name == "AudioContent");
    CHECK(members[1].tag == 61);
    CHECK(members[22].name == "Name");
    CHECK(members[22].tag == 49);
    // Shared-Name table spot checks (resolves all 348 shared uses).
    CHECK(s.shared[27] == "Handle");
    CHECK(s.shared[120] == "Weight");
    CHECK(s.shared[122] == "idle");
    CHECK(s.urls[0] == "rbxassetid://");
}

TEST_CASE("joindata records012: 146 script/sound records, EOF-exact") {
    const rbxclient::JoinSchema s = load_schema();
    const std::vector<rbxclient::JoinRecord> recs = load_records(s, "frame012.bin");
    REQUIRE(recs.size() == 146);
    // First record == the native log's first Done line (1_16098, 63B).
    const rbxclient::JoinRecord& r = recs[0];
    CHECK(r.lo == 1);
    CHECK(r.id == 16098);
    CHECK(r.cls == 482);
    CHECK(s.classes[r.cls].name == "Sound");
    CHECK(r.parent == 7438);
    const rbxclient::JoinProp* audio = find_prop(r, "AudioContent");
    REQUIRE(audio != nullptr);
    CHECK(audio->tag == 61);
    // Content form 1: [01][varint len][bytes].
    REQUIRE(audio->value.size() > 2);
    CHECK(audio->value[0] == 0x01);
    CHECK(audio->value[1] == 0x29);
    CHECK(std::string(audio->value.begin() + 2, audio->value.end()) ==
          "rbxasset://Sounds/electronicpingshort.wav");
    const rbxclient::JoinProp* name = find_prop(r, "Name");
    REQUIRE(name != nullptr);
    CHECK(name->tag == 49);
    REQUIRE(name->value.size() == 5);
    CHECK(name->value[0] == 0x04);
    CHECK(std::string(name->value.begin() + 1, name->value.end()) == "Ping");
    // Class mix is scripts + sounds only in this batch.
    std::map<uint16_t, int> hist;
    for (const auto& q : recs)
        ++hist[q.cls];
    CHECK(hist[332] == 80);  // ModuleScript
    CHECK(hist[440] == 38);  // Script
    CHECK(hist[482] == 26);  // Sound
    CHECK(hist[301] == 2);   // LocalScript
}

TEST_CASE("joindata records030: 29 Parts + Terrain, CFrame identity") {
    const rbxclient::JoinSchema s = load_schema();
    const std::vector<rbxclient::JoinRecord> recs = load_records(s, "frame030.bin");
    REQUIRE(recs.size() == 30);
    // First record: a Tool Handle (shared Name 27) at an identity rotation.
    const rbxclient::JoinRecord& r = recs[0];
    CHECK(r.id == 4363439);
    CHECK(r.cls == 355);
    CHECK(s.classes[r.cls].name == "Part");
    CHECK(r.parent == 4363438);
    CHECK(r.props.size() == 5);
    const rbxclient::JoinProp* cf = find_prop(r, "CFrame");
    REQUIRE(cf != nullptr);
    REQUIRE(cf->value.size() == 13); // pos + kind 2 (no quat bytes)
    float px = 0, py = 0, pz = 0;
    std::memcpy(&px, cf->value.data(), 4);
    std::memcpy(&py, cf->value.data() + 4, 4);
    std::memcpy(&pz, cf->value.data() + 8, 4);
    CHECK(px == doctest::Approx(-6.8200679f));
    CHECK(py == doctest::Approx(3.219636f));
    CHECK(pz == doctest::Approx(34.070084f));
    CHECK(cf->value[12] == 2); // table kind 2 = identity rotation
    const rbxclient::JoinProp* nm = find_prop(r, "Name");
    REQUIRE(nm != nullptr);
    REQUIRE(nm->value.size() == 1);
    CHECK(nm->value[0] == (0x80 | 27)); // shared "Handle"
    CHECK(s.shared[nm->value[0] & 0x7F] == "Handle");
}
