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

TEST_CASE("spawn: exotic parent + opaque tail stop pinned, never silent") {
    // Message @57504: four clean spawns (Player/PlayerGui/Folder/
    // LocalScript), then a LocalScript whose parent ref carries an
    // out-of-range scope (164917115, id 98643702 — seen once in the whole
    // session), then 3742B of structureless binary (no clocks, no spawn
    // heads, no zero runs, no zstd/Luau magic, no ASCII). The native
    // resolvers (319BE28/2495B8E) tolerate deferred parents; the tail's
    // grammar is open (v2: trace exotic guids natively). The walk must
    // stop pinned here, never desync silently.
    const rbxclient::JoinSchema s = load_schema();
    const auto m = slurp(vec("spawn_weird.bin"));
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m.data(), m.size(), s, w, &err));
    CHECK(!w.complete);
    CHECK(w.stop_type == 0x78);
    CHECK(w.stop_off == 331);
    REQUIRE(w.items.size() == 4);
    CHECK(w.items[0].spawn.id == 4366484u);
    CHECK(s.classes[w.items[0].spawn.cls].name == "Player");
    CHECK(w.items[3].spawn.id == 11969u);
    CHECK(s.classes[w.items[3].spawn.cls].name == "LocalScript");
    CHECK(w.items[3].spawn.plo == 164917115ull);
    CHECK(w.items[3].spawn.parent == 98643702);
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
    CHECK(t.b == 0u);
    CHECK(t.c == 0u);
    CHECK(t.tail == 0u);
    CHECK(!t.has_x);
    CHECK(t.has_y);
    CHECK(t.y == 0u);
    CHECK(w.items[1].type == 0x23);
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
