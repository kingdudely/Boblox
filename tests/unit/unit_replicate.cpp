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

namespace {
// LEB128 u64 decode (test-local; documents pinned varint values).
uint64_t leb(const std::vector<uint8_t>& v) {
    uint64_t r = 0;
    int s = 0;
    for (uint8_t b : v) {
        r |= uint64_t(b & 0x7F) << s;
        s += 7;
        if (!(b & 0x80))
            break;
    }
    return r;
}
} // namespace

TEST_CASE("chain07: 8 FreshFriendStatus events + clock walk complete") {
    const rbxclient::JoinSchema s = load_schema();
    const auto m = slurp(vec("msg07.bin"));
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m.data(), m.size(), s, w, &err));
    REQUIRE(w.complete);
    REQUIRE(w.items.size() == 9);
    // First seven invocations: identical target, varying payload.
    for (size_t i = 0; i < 7; ++i) {
        const auto& it = w.items[i];
        REQUIRE(it.type == 0x07);
        CHECK(it.event.lo == 1);
        CHECK(it.event.entity == 9104);
        CHECK(it.event.event == 2351);
        CHECK(s.classes[it.event.cls_idx].name == "FriendService");
        CHECK(s.classes[it.event.cls_idx].events[it.event.ev_idx].name ==
              "RemoteFriendStatusSignal");
        REQUIRE(it.event.args.size() == 3);
        CHECK(it.event.args[0].tag == 44);
        CHECK(it.event.args[1].tag == 44);
        CHECK(it.event.args[2].tag == 7);
        CHECK(it.event.args[2].extra == 0x00DC);
    }
    // First invocation pinned byte-exact (tag44 zigzag varints + enum).
    const auto& a0 = w.items[0].event.args;
    CHECK(a0[0].value ==
          std::vector<uint8_t>({0xDE, 0xE3, 0xDB, 0xD8, 0x22}));
    CHECK(leb(a0[0].value) == 9312858590ull); // zigzag -> user 4656429295
    CHECK(a0[1].value ==
          std::vector<uint8_t>({0xBA, 0xBD, 0xBC, 0xEE, 0x57}));
    CHECK(a0[2].value == std::vector<uint8_t>({0x01}));
    // Eighth invocation carries different args (same event, longer tail).
    const auto& a7 = w.items[7].event.args;
    CHECK(a7[0].value ==
          std::vector<uint8_t>({0xEC, 0xF2, 0xEC, 0xE4, 0x13}));
    CHECK(a7[1].value ==
          std::vector<uint8_t>({0xDE, 0xE3, 0xDB, 0xD8, 0x22}));
    CHECK(a7[2].value == std::vector<uint8_t>({0x02}));
    // Trailing clock tick, then the 00 end marker.
    CHECK(w.items[8].type == 0x23);
    CHECK(w.items[8].clock == 6562856u);
}

TEST_CASE("chain07/sound: 0x07 + clock chain inside tick messages") {
    const rbxclient::JoinSchema s = load_schema();
    // 30B message: tick, two Sound.playbackActionSync-class invocations
    // (events 5211/5206, zigzag-int params), tick, end.
    const auto m = slurp(vec("chain0730.bin"));
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m.data(), m.size(), s, w, &err));
    REQUIRE(w.complete);
    REQUIRE(w.items.size() == 4);
    CHECK(w.items[0].type == 0x23);
    CHECK(w.items[1].type == 0x07);
    CHECK(w.items[1].event.entity == 0x0B256E1Bu);
    CHECK(w.items[1].event.event == 5211);
    CHECK(s.classes[w.items[1].event.cls_idx].events[w.items[1].event.ev_idx]
              .name == "playbackActionSync");
    REQUIRE(w.items[1].event.args.size() == 1);
    CHECK(w.items[1].event.args[0].tag == 10);
    CHECK(w.items[1].event.args[0].value == std::vector<uint8_t>({0x00}));
    CHECK(w.items[2].type == 0x07);
    CHECK(w.items[2].event.entity == 0x0B256E1Bu);
    CHECK(w.items[2].event.event == 5206);
    REQUIRE(w.items[2].event.args.size() == 1);
    CHECK(w.items[3].type == 0x23);
    CHECK(w.items[3].clock == w.items[0].clock + 4);
}

TEST_CASE("spawn: smallest 0x02 walks spawn + clock, complete") {
    const rbxclient::JoinSchema s = load_schema();
    const auto m = slurp(vec("spawn_small.bin"));
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m.data(), m.size(), s, w, &err));
    REQUIRE(w.complete);
    REQUIRE(w.items.size() == 2);
    const auto& sp = w.items[0].spawn;
    CHECK(w.items[0].type == 0x02);
    CHECK(sp.lo == 4);
    CHECK(sp.id == 0x7F2B9E2Fu);
    CHECK(sp.cls == 347);
    CHECK(s.classes[sp.cls].name == "ObjectValue");
    // Loop A: Value (ref lo=1 id=0x00429148); loop B: Name "creator".
    REQUIRE(sp.props_a.size() == 1);
    CHECK(sp.props_a[0].name == "Value");
    CHECK(sp.props_a[0].tag == 28);
    CHECK(sp.props_a[0].value ==
          std::vector<uint8_t>({0x01, 0x48, 0x91, 0x42, 0x00}));
    REQUIRE(sp.props_b.size() == 1);
    CHECK(sp.props_b[0].name == "Name");
    CHECK(sp.props_b[0].tag == 49);
    CHECK(sp.props_b[0].value ==
          std::vector<uint8_t>(
              {0x07, 'c', 'r', 'e', 'a', 't', 'o', 'r'}));
    CHECK(sp.plo == 1);
    CHECK(sp.parent == 0x00428D5F);
    CHECK(w.items[1].type == 0x23);
    CHECK(w.items[1].clock == 0x47C44Cu);
}

TEST_CASE("spawn: largest 0x02 walks complete end to end") {
    const rbxclient::JoinSchema s = load_schema();
    const auto m = slurp(vec("spawn_big.bin"));
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m.data(), m.size(), s, w, &err));
    REQUIRE(w.complete);
    // 7 spawns/events, five empty 0x0d items, then the 16-record entry
    // frame (mesh subtree), ending at clean EOF.
    REQUIRE(w.items.size() == 13);
    // First record: an Attachment (class 43) with named attributes.
    const auto& first = w.items[0].spawn;
    CHECK(w.items[0].type == 0x02);
    CHECK(first.lo == 4);
    CHECK(first.id == 0x7A1C609Au);
    CHECK(first.cls == 43);
    CHECK(s.classes[first.cls].name == "Attachment");
    CHECK(first.props_a.empty()); // loop A empty (immediate 0xFF)
    CHECK(!first.props_b.empty());
    // The entry frame: exactly 16 records + opaque trail.
    const auto& frame = w.items[12].delta0d;
    CHECK(w.items[12].type == 0x0d);
    CHECK(frame.entries.n == 16);
    REQUIRE(frame.entries.records.size() == 16);
    CHECK(frame.entries.records[0].id == 4363911u);
    CHECK(s.classes[frame.entries.records[0].cls].name ==
          "SurfaceAppearance");
    CHECK(frame.entries.records[15].id == 4364063u);
    CHECK(s.classes[frame.entries.records[15].cls].name == "WrapTarget");
    CHECK(frame.entries.trail.size() == 13262);
    REQUIRE(frame.elems.size() == 1);
    CHECK(frame.elems[0].flags == 0x04);
}

TEST_CASE("delta0d: N=0 items walk with clock") {
    const rbxclient::JoinSchema s = load_schema();
    for (const char* vf : {"od13.bin", "stop0d.bin"}) {
        const auto m = slurp(vec(vf));
        rbxclient::Walk83 w;
        std::string err;
        INFO("walk err (" << vf << "): " << err);
        REQUIRE(rbxclient::walk83Items(m.data(), m.size(), s, w, &err));
        REQUIRE(w.complete);
    }
    // od13: one N=0 item (single end-element) + clock + end.
    {
        const auto m = slurp(vec("od13.bin"));
        rbxclient::Walk83 w;
        std::string err;
        REQUIRE(rbxclient::walk83Items(m.data(), m.size(), s, w, &err));
        REQUIRE(w.items.size() == 2);
        CHECK(w.items[0].type == 0x0d);
        CHECK(w.items[0].delta0d.entries.n == 0);
        REQUIRE(w.items[0].delta0d.elems.size() == 1);
        CHECK(w.items[0].delta0d.elems[0].flags == 0x02);
        CHECK(w.items[0].delta0d.elems[0].payload.empty());
        CHECK(w.items[1].type == 0x23);
        CHECK(w.items[1].clock == 0x47D59Cu);
    }
    // od65: four chained N=0 items + clock + end.
    {
        const auto m = slurp(vec("od65.bin"));
        rbxclient::Walk83 w;
        std::string err;
        REQUIRE(rbxclient::walk83Items(m.data(), m.size(), s, w, &err));
        REQUIRE(w.complete);
        REQUIRE(w.items.size() == 5);
        for (size_t i = 0; i < 4; ++i) {
            CHECK(w.items[i].type == 0x0d);
            CHECK(w.items[i].delta0d.entries.n == 0);
        }
        CHECK(w.items[4].type == 0x23);
    }
}

TEST_CASE("delta0d: entry frames carry snapshot records") {
    const rbxclient::JoinSchema s = load_schema();
    // N=1: one Part + end-element + clock + end.
    {
        const auto m = slurp(vec("od124.bin"));
        rbxclient::Walk83 w;
        std::string err;
        INFO("walk err: " << err);
        REQUIRE(rbxclient::walk83Items(m.data(), m.size(), s, w, &err));
        REQUIRE(w.complete);
        REQUIRE(w.items.size() == 2);
        const auto& fr = w.items[0].delta0d;
        CHECK(fr.entries.n == 1);
        REQUIRE(fr.entries.records.size() == 1);
        CHECK(fr.entries.records[0].id == 4342983u);
        CHECK(s.classes[fr.entries.records[0].cls].name == "Part");
        CHECK(fr.entries.trail.empty());
        CHECK(w.items[1].type == 0x23);
    }
    // N=6: mesh subtree (StringValue/SurfaceAppearance/MeshPart +
    // children) + end-element + clock + end.
    {
        const auto m = slurp(vec("od463.bin"));
        rbxclient::Walk83 w;
        std::string err;
        INFO("walk err: " << err);
        REQUIRE(rbxclient::walk83Items(m.data(), m.size(), s, w, &err));
        REQUIRE(w.complete);
        REQUIRE(w.items.size() == 2);
        const auto& fr = w.items[0].delta0d;
        CHECK(fr.entries.n == 6);
        REQUIRE(fr.entries.records.size() == 6);
        CHECK(fr.entries.records[0].id == 4371113u);
        CHECK(s.classes[fr.entries.records[0].cls].name == "StringValue");
        CHECK(fr.entries.records[2].id == 4371111u);
        CHECK(s.classes[fr.entries.records[2].cls].name == "MeshPart");
        CHECK(fr.entries.trail.empty());
        CHECK(w.items[1].type == 0x23);
    }
}

TEST_CASE("item01: entity refs chain with clock") {
    const rbxclient::JoinSchema s = load_schema();
    const auto m = slurp(vec("item01.bin"));
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m.data(), m.size(), s, w, &err));
    REQUIRE(w.complete);
    REQUIRE(w.items.size() == 3);
    CHECK(w.items[0].type == 0x01);
    CHECK(w.items[0].ref01.lo == 4);
    CHECK(w.items[0].ref01.entity == 0x54718456u);
    CHECK(w.items[1].type == 0x01);
    CHECK(w.items[1].ref01.lo == 1);
    CHECK(w.items[1].ref01.entity == 0x0042A26Du);
    CHECK(w.items[2].type == 0x23);
    CHECK(w.items[2].clock == 0x47C48Cu);
}

TEST_CASE("spawn: script-data bulk completes the weird message") {
    // Message @57504: four clean spawns (Player/PlayerGui/Folder/
    // LocalScript id 11969), then a 3741B script-data bulk: the spawn's
    // parent ref deterministically re-reads the first half of its own
    // Source-hash value (scope 164917115, id 98643702 — content-keyed, so
    // the native deferred-parent lookup misses and tolerates it), and the
    // bulk carries the key suffix + bytecode payload + anchor ref
    // (lo=01, id=11928 = the hosting Folder) to the exact end marker.
    // IDA-verified: 34956D2 consumes nothing after the parent ref, 0x78
    // is no item type, and no message-level catch exists — so the native
    // parses this very shape; the walk must complete exactly here.
    const rbxclient::JoinSchema s = load_schema();
    const auto m = slurp(vec("spawn_weird.bin"));
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    const rbxclient::PropTagTable pt = rbxclient::defaultPropTags();
    REQUIRE(rbxclient::walk83ItemsEx(m.data(), m.size(), s, &pt, w, &err));
    CHECK(w.complete);
    CHECK(w.end_off == m.size());
    REQUIRE(w.items.size() == 5);
    CHECK(w.items[0].spawn.id == 4366484u);
    CHECK(s.classes[w.items[0].spawn.cls].name == "Player");
    CHECK(w.items[3].spawn.id == 11969u);
    CHECK(s.classes[w.items[3].spawn.cls].name == "LocalScript");
    CHECK(w.items[3].spawn.plo == 164917115ull);
    CHECK(w.items[3].spawn.parent == 98643702);
    // The bulk: key suffix = Source-hash tail, 3725B payload, anchor =
    // the hosting Folder 11928, then the 00 end marker (exact close).
    CHECK(w.items[4].type == 0x78);
    const auto& bulk = w.items[4].bulk;
    CHECK(bulk.payload.size() == 3725);
    CHECK(bulk.anchor_lo == 1);
    CHECK(bulk.anchor == 11928);
}

TEST_CASE("ticks: double-tick message walks two clocks +4") {
    const rbxclient::JoinSchema s = load_schema();
    const auto m = slurp(vec("ticks23.bin"));
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m.data(), m.size(), s, w, &err));
    REQUIRE(w.complete);
    REQUIRE(w.items.size() == 2);
    CHECK(w.items[0].type == 0x23);
    CHECK(w.items[0].clock == 0x47C318u);
    CHECK(w.items[1].type == 0x23);
    CHECK(w.items[1].clock == 0x47C31Cu);
}

TEST_CASE("item1b: referent list chains with clock") {
    // stop1b.bin is now a complete walk (the 0x1b item parses).
    const rbxclient::JoinSchema s = load_schema();
    const auto m = slurp(vec("stop1b.bin"));
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m.data(), m.size(), s, w, &err));
    REQUIRE(w.complete);
    REQUIRE(w.items.size() == 3);
    CHECK(w.items[0].type == 0x23);
    CHECK(w.items[0].clock == 0x47DF38u);
    CHECK(w.items[1].type == 0x1b);
    REQUIRE(w.items[1].refs1b.refs.size() == 1);
    CHECK(w.items[1].refs1b.refs[0].lo == 4);
    CHECK(w.items[1].refs1b.refs[0].entity == 0x60A30176u);
    CHECK(w.items[2].type == 0x23);
    CHECK(w.items[2].clock == 0x47DF3Cu);
}

TEST_CASE("item05: timing item walks with clock") {
    const rbxclient::JoinSchema s = load_schema();
    const auto m = slurp(vec("item05.bin"));
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m.data(), m.size(), s, w, &err));
    REQUIRE(w.complete);
    REQUIRE(w.items.size() == 2);
    CHECK(w.items[0].type == 0x05);
    const auto& t = w.items[0].timing05;
    CHECK(t.flags == 0x10);
    CHECK(t.t == 0x000000000133BA5Eu);
    CHECK(t.a == 0x0133BA5Eu);
    CHECK(t.d == 0u);
    CHECK(t.e == 0u);
    CHECK(!t.has_extra);
    CHECK(t.tail == 0u);
    CHECK(!t.dropped);
    CHECK(!t.has_x);
    CHECK(t.has_y);
    CHECK(t.y == 0u);
    CHECK(w.items[1].type == 0x23);
}

TEST_CASE("item05: flags&1 reads six extra u32s (Brookhaven shape)") {
    // sub_3486B9E flags&1 branch: [u8=0x03][u64 t][u32 a][6x u32]
    // [u32 d][u32 e][u8 tail=0] = 46B. The old parser stopped after a/d/e
    // (24B short) and derailed every such message (Brookhaven's 92
    // "tail>1" hard-fails were this short read landing mid-item).
    const rbxclient::JoinSchema s = load_schema();
    const uint8_t m[] = {
        0x83, 0x05, 0x03, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0xAA, 0xBB, 0xCC, 0xDD, 0x01, 0x00, 0x00, 0x00, 0x02,
        0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
        0x00, 0x05, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0xDD,
        0xCC, 0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x00, 0x23, 0x11,
        0x22, 0x33, 0x44, 0x00};
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m, sizeof m, s, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == sizeof m);
    REQUIRE(w.items.size() == 2);
    CHECK(w.items[0].type == 0x05);
    const auto& t = w.items[0].timing05;
    CHECK(t.flags == 0x03);
    CHECK(t.t == 0x8877665544332211ull);
    CHECK(t.a == 0xDDCCBBAAu);
    CHECK(t.has_extra);
    CHECK(t.extra[0] == 1u);
    CHECK(t.extra[5] == 6u);
    CHECK(t.d == 0xAABBCCDDu);
    CHECK(t.e == 0x66778899u);
    CHECK(t.tail == 0u);
    CHECK(!t.dropped);
    CHECK(w.items[1].type == 0x23);
}

TEST_CASE("item05: tail>1 drops the item, chain continues") {
    // sub_3486B9E returns null (no throw) when tail > 1: the item is
    // dropped natively and the chain continues. Synthetic: [83][05…tail
    // =7][23][clock][00]. Exact bytes: flags=0x10 (no extra), t=8B,
    // a/d/e, tail=7, then clock + end.
    const rbxclient::JoinSchema s = load_schema();
    const uint8_t m[] = {
        0x83, 0x05, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03,
        0x00, 0x00, 0x00, 0x07, 0x23, 0x11, 0x22, 0x33, 0x44, 0x00};
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m, sizeof m, s, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == sizeof m);
    REQUIRE(w.items.size() == 2);
    CHECK(w.items[0].type == 0x05);
    CHECK(w.items[0].timing05.dropped);
    CHECK(w.items[1].type == 0x23);
    CHECK(w.items[1].clock == 0x44332211u);
}

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
    // Global event index = concatenated per-class lists (schema order);
    // the total equals the ev_bound prealloc exactly (6937 frozen).
    size_t nev_total = 0;
    for (const auto& c : s.classes)
        nev_total += c.events.size();
    CHECK(nev_total == 6937);
    CHECK(nev_total == s.ev_bound);
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

TEST_CASE("prop03: tiny u16 pair + tick walks complete") {
    // @885401 (27B): two prop-5469 triples (tag 18, constant 0x0101) on
    // two entities, then a clock tick and the end marker. Prop 5469's tag
    // is extent-ambiguous ({2,8,18,33,49} all read 2B here); u16 is the
    // shipped representative (constant 257, BrickColor-index hypothesis).
    const rbxclient::JoinSchema s = load_schema();
    const auto m = slurp(vec("prop03_tiny.bin"));
    REQUIRE(m.size() == 27);
    const rbxclient::PropTagTable pt = rbxclient::defaultPropTags();
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83ItemsEx(m.data(), m.size(), s, &pt, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == m.size());
    REQUIRE(w.items.size() == 3);
    for (size_t i = 0; i < 2; ++i) {
        CHECK(w.items[i].type == 0x03);
        CHECK(w.items[i].prop03.lo == 1);
        CHECK(w.items[i].prop03.prop == 5469);
        CHECK(w.items[i].prop03.tag == 18);
        CHECK(w.items[i].prop03.value ==
              std::vector<uint8_t>({0x01, 0x01}));
    }
    CHECK(w.items[0].prop03.id == 0x00428D5Du);
    CHECK(w.items[1].prop03.id == 0x00428D5Bu);
    CHECK(w.items[2].type == 0x23);
}

TEST_CASE("prop03: Content emote-table triple walks complete") {
    // 92B message: prop-3796 triple (tag 61 Content form 1: 01 4b + 75B
    // emote table) then a clock tick and the end marker. This is the
    // message that refuted the schema-dense prop map (dense-3796 would be
    // HumanoidDescription.DepthScale/f32).
    const rbxclient::JoinSchema s = load_schema();
    const auto m = slurp(vec("prop03_content.bin"));
    const rbxclient::PropTagTable pt = rbxclient::defaultPropTags();
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83ItemsEx(m.data(), m.size(), s, &pt, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == m.size());
    REQUIRE(w.items.size() == 2);
    CHECK(w.items[0].type == 0x03);
    CHECK(w.items[0].prop03.prop == 3796);
    CHECK(w.items[0].prop03.tag == 61);
    REQUIRE(w.items[0].prop03.value.size() == 77);
    CHECK(w.items[0].prop03.value[0] == 0x01);
    CHECK(w.items[0].prop03.value[1] == 0x4B);
    CHECK(w.items[1].type == 0x23);
}

TEST_CASE("prop03: default table pins the full native map + overrides") {
    // 10146 entries (== prop_bound+1, register-dumped); spot pins mix
    // native numbers (0:7 varint, 1:27 CFrame, 2:10 zigzag, 3:9 bool)
    // with solver-proven wire overrides (3796:61 Content, ...).
    const rbxclient::PropTagTable pt = rbxclient::defaultPropTags();
    REQUIRE(pt.tag.size() == 10146);
    CHECK(pt.tag.at(248) == 68);
    CHECK(pt.tag.at(3796) == 61);
    CHECK(pt.tag.at(4989) == 18);
    CHECK(pt.tag.at(5018) == 61);
    CHECK(pt.tag.at(5469) == 18);
    CHECK(pt.tag.at(5868) == 3);
    CHECK(pt.tag.at(5918) == 46);
    CHECK(pt.tag.at(10145) == 25);
}

TEST_CASE("item10: tiny u32 item walks with end") {
    const rbxclient::JoinSchema s = load_schema();
    const auto m = slurp(vec("item10.bin"));
    REQUIRE(m.size() == 7);
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m.data(), m.size(), s, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == m.size());
    REQUIRE(w.items.size() == 1);
    CHECK(w.items[0].type == 0x10);
    CHECK(w.items[0].v10.v == 12u);
}

TEST_CASE("item20: sized blob chains into entry frames") {
    // @577084 (43700B): clock tick, 0x20 blob (a=3, 48B), six 0x0d
    // items, end marker. The blob consumes exactly 9+48B; the rest of
    // the 43KB chains on (sub_35C3A7E reads [u32][u32 len][bytes]).
    const rbxclient::JoinSchema s = load_schema();
    const auto m = slurp(vec("item20.bin"));
    REQUIRE(m.size() == 43700);
    const rbxclient::PropTagTable pt = rbxclient::defaultPropTags();
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83ItemsEx(m.data(), m.size(), s, &pt, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == m.size());
    REQUIRE(w.items.size() == 8);
    CHECK(w.items[0].type == 0x23);
    CHECK(w.items[1].type == 0x20);
    CHECK(w.items[1].blob20.a == 3u);
    CHECK(w.items[1].blob20.blob.size() == 48);
    for (size_t i = 2; i < 8; ++i)
        CHECK(w.items[i].type == 0x0d);
}

TEST_CASE("event tuple: OnClientEvent self-describing elements") {
    // @621575 (92B): AvatarChatService.OnClientFeatures + RemoteEvent.
    // OnClientEvent (event 4452, one tag-29 arg: N=2 self-tagged elements
    // in 15B) + six 0x0d items + tick. Tuple elements carry their own
    // tags on the wire (sub_340887C) — no compiled-in table needed.
    const rbxclient::JoinSchema s = load_schema();
    const auto m = slurp(vec("event_tuple.bin"));
    const rbxclient::PropTagTable pt = rbxclient::defaultPropTags();
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83ItemsEx(m.data(), m.size(), s, &pt, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == m.size());
    REQUIRE(w.items.size() == 9);
    CHECK(w.items[0].type == 0x07);
    CHECK(w.items[0].event.event == 785);
    CHECK(w.items[1].type == 0x07);
    CHECK(w.items[1].event.event == 4452);
    CHECK(s.classes[w.items[1].event.cls_idx].name == "RemoteEvent");
    CHECK(s.classes[w.items[1].event.cls_idx].events[w.items[1].event.ev_idx]
              .name == "OnClientEvent");
    REQUIRE(w.items[1].event.args.size() == 1);
    const auto& arg = w.items[1].event.args[0];
    CHECK(arg.tag == 29);
    CHECK(arg.value.size() == 15);
    REQUIRE(arg.elements.size() == 2);
    for (size_t i = 6; i < 8; ++i)
        CHECK(w.items[i].type == 0x0d);
    CHECK(w.items[8].type == 0x23);
}

TEST_CASE("shared01: flags=01 batch inner parses as shared strings") {
    // @61592 (24561B message, 24539B inner): 9 shared-string entries
    // ([16B key][marker][varint len][bytes], sub_342550C). Markers are
    // all 0 (plain); the first value is 1428B.
    const auto b = slurp(vec("batch01.bin"));
    size_t blen = 0;
    const uint8_t* body = frame_body(b, true, &blen);
    REQUIRE(blen > 3);
    CHECK(body[1] == 0x16);
    CHECK(body[2] == 0x01);
    std::vector<uint8_t> inner;
    std::string err;
    REQUIRE(rbxclient::parseDeltaBatch(body, blen, inner, &err));
    const auto golden = slurp(vec("shared01_inner.bin"));
    CHECK(inner == golden);
    std::vector<rbxclient::SharedString> ss;
    REQUIRE(rbxclient::parseSharedStrings(inner.data(), inner.size(), ss,
                                          &err));
    REQUIRE(ss.size() == 9);
    for (const auto& e : ss)
        CHECK((e.marker == 0 || e.marker == 1));
    CHECK(ss[0].value.size() == 1428);
}

TEST_CASE("prop03: 4x-varint triple walks complete") {
    // Fresh-session prop 248 (25B): one triple (tag 68: four varints
    // 01 01 03 <7B>) + clock + end. The only all-closing shape (native#
    // 61 in either Content mode fails here); exact close.
    const rbxclient::JoinSchema s = load_schema();
    const auto m = slurp(vec("prop03_248.bin"));
    REQUIRE(m.size() == 25);
    const rbxclient::PropTagTable pt = rbxclient::defaultPropTags();
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83ItemsEx(m.data(), m.size(), s, &pt, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == m.size());
    REQUIRE(w.items.size() == 2);
    CHECK(w.items[0].type == 0x03);
    CHECK(w.items[0].prop03.prop == 248);
    CHECK(w.items[0].prop03.tag == 68);
    CHECK(w.items[0].prop03.value ==
          std::vector<uint8_t>(
              {0x01, 0x01, 0x03, 0xFC, 0xF9, 0x87, 0xA4, 0xB2, 0xE7, 0x29}));
    CHECK(w.items[1].type == 0x23);
}

TEST_CASE("prop03: constant-u16 run walks complete") {
    // Fresh-session prop 4989 (309B): a run of constant-0x0101 u16
    // triples (tag 18, same ambiguity class as 5469: the 2B
    // string/asset/Name shapes tie on all observed bytes; u16 shipped
    // for the constant-257 story), then more chain items to exact close.
    const rbxclient::JoinSchema s = load_schema();
    const auto m = slurp(vec("prop03_4989.bin"));
    const rbxclient::PropTagTable pt = rbxclient::defaultPropTags();
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83ItemsEx(m.data(), m.size(), s, &pt, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == m.size());
    REQUIRE(w.items.size() >= 7);
    for (size_t i = 0; i < 7; ++i) {
        CHECK(w.items[i].type == 0x03);
        CHECK(w.items[i].prop03.prop == 4989);
        CHECK(w.items[i].prop03.tag == 18);
        CHECK(w.items[i].prop03.value ==
              std::vector<uint8_t>({0x01, 0x01}));
    }
}

TEST_CASE("prop03: Content touch-text triple chains on") {
    // Fresh-session prop 5018 (421B): Content triple (tag 61: 01 23 +
    // "Touch the beacon a...") chained with 01/1b items, 10145 triples,
    // and a spawn to exact close. Same version-skew class as 3796
    // (native# is string-shaped 2).
    const rbxclient::JoinSchema s = load_schema();
    const auto m = slurp(vec("prop03_5018.bin"));
    const rbxclient::PropTagTable pt = rbxclient::defaultPropTags();
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83ItemsEx(m.data(), m.size(), s, &pt, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == m.size());
    REQUIRE(w.items.size() == 14);
    CHECK(w.items[0].type == 0x03);
    CHECK(w.items[0].prop03.prop == 5018);
    CHECK(w.items[0].prop03.tag == 61);
    REQUIRE(w.items[0].prop03.value.size() == 37);
    CHECK(w.items[0].prop03.value[0] == 0x01);
    CHECK(w.items[0].prop03.value[1] == 0x23);
    CHECK(std::string(w.items[0].prop03.value.begin() + 2,
                      w.items[0].prop03.value.end()) ==
          "Touch the beacon again to heal you!");
}

TEST_CASE("prop03: guid triple stops at native-throwing 0x70") {
    // @603831 (570B): 0x20 blob + spawns + prop-5918 triple (tag 46:
    // 16B guid starting 01cae6...) then chain byte 0x70. Type 112 is
    // outside both native dispatch tables (9-41 outer, 1-22 inner), so
    // the native throws here and its packet is dropped; the walker stops
    // pinned with the applied prefix intact — faithful replication, not
    // a coverage gap (the 7 NumberValue spawns after it parse cleanly in
    // isolation, verified by probe).
    const rbxclient::JoinSchema s = load_schema();
    const auto m = slurp(vec("prop03_5918.bin"));
    REQUIRE(m.size() == 570);
    const rbxclient::PropTagTable pt = rbxclient::defaultPropTags();
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83ItemsEx(m.data(), m.size(), s, &pt, w, &err));
    CHECK(!w.complete);
    CHECK(w.stop_type == 0x70);
    CHECK(w.stop_off == 154);
    REQUIRE(w.items.size() == 4);
    CHECK(w.items[3].type == 0x03);
    CHECK(w.items[3].prop03.prop == 5918);
    CHECK(w.items[3].prop03.tag == 46);
    CHECK(w.items[3].prop03.value ==
          std::vector<uint8_t>({0x01, 0xCA, 0xE6, 0x69, 0x41, 0xD9, 0xEF,
                                 0xBD, 0x40, 0x4E, 0x4D, 0x88, 0x75, 0x8E,
                                 0xA6, 0x76}));
}

TEST_CASE("prop03: backbone pins native numbers for unobserved props") {
    const rbxclient::PropTagTable pt = rbxclient::defaultPropTags();
    CHECK(pt.tag.at(0) == 7);
    CHECK(pt.tag.at(1) == 27);
    CHECK(pt.tag.at(2) == 10);
    CHECK(pt.tag.at(3) == 9);
    CHECK(pt.tag.at(4) == 2);
    CHECK(pt.tag.at(5906) == 68); // the only native-68 prop (248 overrides)
    CHECK(pt.tag.at(8079) == 25);
    CHECK(pt.tag.at(8080) == 25);
}

TEST_CASE("item04: u32 item walks with clock") {
    // Synthetic: [83][04][u32=0x12345678][23][clock][00]. sub_3485DD4
    // reads a single u32; never observed live, IDA-only until seen.
    const rbxclient::JoinSchema s = load_schema();
    const uint8_t m[] = {0x83, 0x04, 0x78, 0x56, 0x34, 0x12, 0x23,
                         0x11, 0x22, 0x33, 0x44, 0x00};
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m, sizeof m, s, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == sizeof m);
    REQUIRE(w.items.size() == 2);
    CHECK(w.items[0].type == 0x04);
    CHECK(w.items[0].v04.v == 0x12345678u);
    CHECK(w.items[1].type == 0x23);
}

TEST_CASE("item06: timing item shares the 0x05 grammar") {
    // Synthetic: [83][06][05-shape 26B][00]. Handler sub_3486B9E serves
    // both 5 and 6; session bits (false, true) as in the walker.
    const rbxclient::JoinSchema s = load_schema();
    std::vector<uint8_t> m = {0x83, 0x06, 0x10};
    m.insert(m.end(), 8, 0x00);             // u64 t
    m.insert(m.end(), {0x01, 0x00, 0x00, 0x00}); // a
    m.insert(m.end(), {0x02, 0x00, 0x00, 0x00}); // b
    m.insert(m.end(), {0x03, 0x00, 0x00, 0x00}); // c
    m.push_back(0x00);                      // tail
    m.insert(m.end(), {0x99, 0x88, 0x77, 0x66}); // y (flags&0x10, bit set)
    m.push_back(0x00);                      // end
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m.data(), m.size(), s, w, &err));
    REQUIRE(w.complete);
    REQUIRE(w.items.size() == 1);
    CHECK(w.items[0].type == 0x06);
    CHECK(w.items[0].timing05.flags == 0x10);
    CHECK(w.items[0].timing05.y == 0x66778899u);
}

TEST_CASE("item13: guid pair walks") {
    // Synthetic: [83][13][guid null][guid lo=1 id=5][00]. sub_34908E4.
    const rbxclient::JoinSchema s = load_schema();
    const uint8_t m[] = {0x83, 0x13, 0x00, 0x01, 0x05, 0x00, 0x00, 0x00,
                         0x00};
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m, sizeof m, s, w, &err));
    REQUIRE(w.complete);
    REQUIRE(w.items.size() == 1);
    CHECK(w.items[0].type == 0x13);
    CHECK(w.items[0].pair13.a.lo == 0);
    CHECK(w.items[0].pair13.a.id == -1);
    CHECK(w.items[0].pair13.b.lo == 1);
    CHECK(w.items[0].pair13.b.id == 5);
}

TEST_CASE("item15: ref plus two strings walks") {
    // Synthetic: [83][15][guid lo=2 id=7]["hi"]["bye"][00]. sub_3475046.
    const rbxclient::JoinSchema s = load_schema();
    const uint8_t m[] = {0x83, 0x15, 0x02, 0x07, 0x00, 0x00, 0x00,
                         0x02, 'h',  'i',  0x03, 'b',  'y',  'e',  0x00};
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m, sizeof m, s, w, &err));
    REQUIRE(w.complete);
    REQUIRE(w.items.size() == 1);
    CHECK(w.items[0].type == 0x15);
    CHECK(w.items[0].info15.ref.lo == 2);
    CHECK(w.items[0].info15.ref.id == 7);
    CHECK(w.items[0].info15.s1 == "hi");
    CHECK(w.items[0].info15.s2 == "bye");
}

TEST_CASE("item29: ref-list with flags walks") {
    // Synthetic: [83][29][N=2][null,0xAA][lo=3 id=9,0xBB][00].
    // sub_3579BFE. N > remaining/2 is a structural violation.
    const rbxclient::JoinSchema s = load_schema();
    const uint8_t m[] = {0x83, 0x29, 0x02, 0x00, 0x00, 0x00, 0x00, 0xAA,
                         0x03, 0x09, 0x00, 0x00, 0x00, 0xBB, 0x00};
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m, sizeof m, s, w, &err));
    REQUIRE(w.complete);
    REQUIRE(w.items.size() == 1);
    CHECK(w.items[0].type == 0x29);
    REQUIRE(w.items[0].list29.entries.size() == 2);
    CHECK(w.items[0].list29.entries[0].ref.id == -1);
    CHECK(w.items[0].list29.entries[0].flag == 0xAA);
    CHECK(w.items[0].list29.entries[1].ref.lo == 3);
    CHECK(w.items[0].list29.entries[1].ref.id == 9);
    CHECK(w.items[0].list29.entries[1].flag == 0xBB);
}

TEST_CASE("tag35: NumberSequence triple bounds N x 12B") {
    // Custom table maps prop 6000 -> tag 35. Value: [u32 N=2][24B].
    // sub_3614E86; N > 20 throws natively (checked).
    rbxclient::PropTagTable pt;
    pt.tag[6000] = 35;
    const uint8_t body[] = {0x01, 0x05, 0x00, 0x00, 0x00, 0x70, 0x17,
                            0x02, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04,
                            0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
                            0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14,
                            0x15, 0x16, 0x17, 0x18};
    rbxclient::PropTriple t;
    size_t used = 0;
    std::string err;
    INFO("triple err: " << err);
    REQUIRE(rbxclient::parsePropTriple(body, sizeof body, pt, t, &used,
                                       &err));
    CHECK(t.prop == 6000);
    CHECK(t.tag == 35);
    CHECK(t.value.size() == 28);
    CHECK(used == sizeof body);
}

TEST_CASE("tag58: meshscale 7B + C bytes triple bounds") {
    // Custom table maps prop 6001 -> tag 58. Value: [7B][C=3][3B].
    // sub_36172DA; C >= 51 throws natively (checked).
    rbxclient::PropTagTable pt;
    pt.tag[6001] = 58;
    const uint8_t body[] = {0x01, 0x05, 0x00, 0x00, 0x00, 0x71, 0x17,
                            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x03,
                            0xAA, 0xBB, 0xCC};
    rbxclient::PropTriple t;
    size_t used = 0;
    std::string err;
    INFO("triple err: " << err);
    REQUIRE(rbxclient::parsePropTriple(body, sizeof body, pt, t, &used,
                                       &err));
    CHECK(t.prop == 6001);
    CHECK(t.tag == 58);
    CHECK(t.value.size() == 11);
    CHECK(used == sizeof body);
}

TEST_CASE("tag60: netasset id triple bounds modern shape") {
    // Custom table maps prop 6002 -> tag 60. Value: [varint schema=7]
    // [8B id][varint v=1]. Null schema (0xFFFF) carries no id.
    rbxclient::PropTagTable pt;
    pt.tag[6002] = 60;
    const uint8_t body[] = {0x01, 0x05, 0x00, 0x00, 0x00, 0x72, 0x17,
                            0x07, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                            0x08, 0x01};
    rbxclient::PropTriple t;
    size_t used = 0;
    std::string err;
    INFO("triple err: " << err);
    REQUIRE(rbxclient::parsePropTriple(body, sizeof body, pt, t, &used,
                                       &err));
    CHECK(t.prop == 6002);
    CHECK(t.tag == 60);
    CHECK(t.value.size() == 10);
    CHECK(used == sizeof body);
    // Null form: schema 0xFFFF (LEB128 FF FF 03), no id follows.
    const uint8_t nullbody[] = {0x01, 0x05, 0x00, 0x00, 0x00, 0x72, 0x17,
                                    0xFF, 0xFF, 0x03};
    rbxclient::PropTriple t2;
    INFO("triple err: " << err);
    REQUIRE(rbxclient::parsePropTriple(nullbody, sizeof nullbody, pt, t2,
                                       &used, &err));
    CHECK(t2.value.size() == 3);
}

TEST_CASE("netasset batch: id + payload entries concatenate") {
    // Synthetic flags=05 body (after the 3B header): [schema=1][8B][v]
    // [paylen=3][3B] x2. sub_348C62C loop shape, IDA-only until observed.
    const uint8_t body[] = {
        0x01, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x01, 0x03,
        'a',  'b',  'c',  0xFF, 0xFF, 0x03, 0x00};
    std::vector<rbxclient::NetAssetEntry> out;
    std::string err;
    INFO("batch err: " << err);
    REQUIRE(rbxclient::parseNetAssetBatch(body, sizeof body, out, &err));
    REQUIRE(out.size() == 2);
    CHECK(out[0].payload == std::vector<uint8_t>({'a', 'b', 'c'}));
    CHECK(out[1].payload.empty());
    CHECK(out[1].id_raw ==
          std::vector<uint8_t>({0xFF, 0xFF, 0x03}));
}

TEST_CASE("oom-guard: 0x0d giant decomp fails without allocating") {
    // libFuzzer-found OOM (recv v4 split): a 0x0d frame claiming decomp
    // 0xffff005e made vector<uint8_t>(decomp) allocate 4GB. Lengths are
    // now capped (64MB) before any allocation.
    const rbxclient::JoinSchema s = load_schema();
    const uint8_t m[] = {0x83, 0x0d, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00,
                         0x00, 0x00, 0x5e, 0x00, 0xff, 0xff, 0x28, 0xb5,
                         0x2f, 0xfd};
    rbxclient::Walk83 w;
    std::string err;
    CHECK(!rbxclient::walk83Items(m, sizeof m, s, w, &err));
    CHECK(err.find("decomp too big") != std::string::npos);
}

TEST_CASE("oom-guard: 0x16 batch giant decomp fails without allocating") {
    const uint8_t m[] = {0x83, 0x16, 0x02, 0x04, 0x00, 0x00, 0x00, 0xff,
                         0xff, 0xff, 0xff, 0x28, 0xb5, 0x2f, 0xfd};
    std::vector<uint8_t> inner;
    std::string err;
    CHECK(!rbxclient::parseDeltaBatch(m, sizeof m, inner, &err));
    CHECK(err.find("decomp too big") != std::string::npos);
}

TEST_CASE("oom-guard: 0x97 giant size fails without allocating") {
    const uint8_t m[] = {0x97, 0xa3, 0xcf, 0x00, 0x00, 0xff, 0xff, 0xff,
                         0xff, 0x28, 0xb5, 0x2f, 0xfd};
    std::vector<uint8_t> inner;
    std::string err;
    CHECK(!rbxclient::parseJoinOuter(m, sizeof m, inner, &err));
    CHECK(err.find("decomp too big") != std::string::npos);
}
