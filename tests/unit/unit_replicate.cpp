// unit_replicate.cpp — replication frame parsers vs frozen captures.
//
// Goldens in tests/vectors/repl/ come from live Crossroads 1818 sessions
// (our rbxplay capture + a native mocktail capture; see
// tools/repl_survey.py and FINDINGS.md "Replication capture + frame
// survey"). The 0x97 snapshot is byte-identical across instances, so
// these goldens are stable, not session-bound.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "solve/repl_pv.h"
#include "solve/replicate.h"

#include <zstd.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
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

// zstd helper for the synthetic 0x0B/0x25 batch goldens below.
std::vector<uint8_t> zstd_compress(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out(ZSTD_compressBound(raw.size()));
    const size_t n =
        ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), 1);
    REQUIRE(!ZSTD_isError(n));
    out.resize(n);
    return out;
}

void put32le(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 24));
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
    // A script spawn carries its bulk inline and has no separate parent ref:
    // the 16 bytes after loop B are the bulk's Source-hash key. Treating
    // that key's first 8 bytes as a ref was the old misparse (it invented
    // parent 98643702); the bulk's anchor is the real parent, Folder 11928.
    CHECK(w.items[3].spawn.has_bulk);
    CHECK(w.items[3].spawn.bulk.key_len == 16);
    CHECK(w.items[3].spawn.plo == 1ull);
    CHECK(w.items[3].spawn.parent == 11928);
    CHECK(w.items[4].type == 0x78);
    const auto& bulk = w.items[4].bulk;
    CHECK(bulk.key_len == 16);
    CHECK(bulk.payload.size() == 3725);
    CHECK(bulk.anchor_lo == 1);
    CHECK(bulk.anchor == 11928);
}

TEST_CASE("spawn: script content bulk is inline and 16-byte keyed") {
    const rbxclient::JoinSchema s = load_schema();
    size_t cls = s.classes.size();
    for (size_t i = 0; i < s.classes.size(); ++i)
        if (s.classes[i].name == "LocalScript") {
            cls = i;
            break;
        }
    REQUIRE(cls < s.classes.size());
    size_t src_member = 255;
    for (size_t i = 0; i < s.classes[cls].members.size(); ++i)
        if (s.classes[cls].members[i].name == "Source") {
            src_member = i;
            break;
        }
    REQUIRE(src_member < 255);

    uint8_t hash[16];
    for (int i = 0; i < 16; ++i)
        hash[i] = uint8_t(0xA0 + i);
    const uint8_t payload[5] = {6, 0xAA, 0xBB, 0xCC, 0xDD};

    std::vector<uint8_t> body{0x83, 0x02, 0x01, 0x34, 0x12, 0x00, 0x00,
                              uint8_t(cls), uint8_t(cls >> 8),
                              uint8_t(src_member)};
    body.insert(body.end(), hash, hash + 16);
    body.push_back(0xFF); // loop A end
    body.push_back(0xFF); // loop B end
    body.insert(body.end(), hash, hash + 16); // bulk key = Source hash
    body.push_back(0x00);                     // bulk marker
    body.push_back(sizeof payload);           // payload length
    body.insert(body.end(), payload, payload + sizeof payload);
    body.push_back(0x01); // anchor ref lo
    put32le(body, 0x5678);
    body.push_back(0x00); // chain end

    rbxclient::PropTagTable pt = rbxclient::propTagsForSchema(s);
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83ItemsEx(body.data(), body.size(), s, &pt, w,
                                     &err));
    CHECK(w.complete);
    CHECK(w.end_off == body.size());
    REQUIRE(w.items.size() == 2);
    CHECK(w.items[0].type == 0x02);
    CHECK(w.items[0].spawn.has_bulk);
    CHECK(w.items[0].spawn.parent == 0x5678);
    CHECK(w.items[0].spawn.props_a.size() == 1);
    CHECK(w.items[0].spawn.props_a[0].name == "Source");
    CHECK(w.items[1].type == 0x78);
    CHECK(w.items[1].bulk.key_len == 16);
    CHECK(std::memcmp(w.items[1].bulk.key, hash, 16) == 0);
    REQUIRE(w.items[1].bulk.payload.size() == sizeof payload);
    CHECK(std::memcmp(w.items[1].bulk.payload.data(), payload,
                      sizeof payload) == 0);
}

TEST_CASE("spawn: Tags-keyed content bulk closes the chat envelope") {
    // Live 2026-11-06 (two sessions, two accounts): the chat envelope is a
    // Folder "Message" whose spawn carries an inline content bulk keyed by
    // its own Tags hash, followed by a StringValue "MessageText" parented to
    // it. The 16-byte key repeats the Tags value byte for byte, so reading
    // the key as a parent ref consumed 6 of its bytes and left the walk
    // pinned at the key's 7th byte (0xa3), which had been counted as a
    // phantom 24-byte item. The bulk is what the bytes actually are.
    const rbxclient::JoinSchema s = load_schema();
    auto class_idx = [&](const char* n) {
        for (size_t i = 0; i < s.classes.size(); ++i)
            if (s.classes[i].name == n)
                return i;
        return s.classes.size();
    };
    auto member_idx = [&](size_t c, const char* n) {
        for (size_t i = 0; i < s.classes[c].members.size(); ++i)
            if (s.classes[c].members[i].name == n)
                return i;
        return size_t(255);
    };
    const size_t folder_cls = class_idx("Folder");
    const size_t sv_cls = class_idx("StringValue");
    REQUIRE(folder_cls < s.classes.size());
    REQUIRE(sv_cls < s.classes.size());
    const size_t tags_m = member_idx(folder_cls, "Tags");
    const size_t value_m = member_idx(sv_cls, "Value");
    const size_t name_m = member_idx(sv_cls, "Name");
    REQUIRE(tags_m < 255);
    REQUIRE(value_m < 255);
    REQUIRE(name_m < 255);
    REQUIRE(s.classes[folder_cls].members[tags_m].tag == 46);

    uint8_t hash[16];
    for (int i = 0; i < 16; ++i)
        hash[i] = uint8_t(0x40 + i);
    const std::string name = "Message";
    const std::string text = "Regenerating Lost Temple...";

    // Folder "Message": Tags in loop A, then the content bulk keyed by that
    // very Tags hash, whose anchor is the Folder's parent (12019).
    std::vector<uint8_t> body{0x83, 0x02, 0x01, 0x78, 0x56, 0x00, 0x00,
                              uint8_t(folder_cls), uint8_t(folder_cls >> 8),
                              uint8_t(tags_m)};
    body.insert(body.end(), hash, hash + 16);
    body.push_back(0xFF); // loop A end
    body.push_back(0xFF); // loop B end
    body.insert(body.end(), hash, hash + 16); // bulk key == Tags value
    body.push_back(0x00);                     // bulk marker
    body.push_back(uint8_t(name.size()));      // payload length
    body.insert(body.end(), name.begin(), name.end());
    body.push_back(0x01); // anchor ref lo
    put32le(body, 12019);

    // StringValue "MessageText" parented to the Folder.
    body.push_back(0x02);
    body.push_back(0x01);
    put32le(body, 0x5679);
    body.push_back(uint8_t(sv_cls));
    body.push_back(uint8_t(sv_cls >> 8));
    body.push_back(0xFF); // loop A end
    body.push_back(uint8_t(value_m));
    body.push_back(uint8_t(text.size()));
    body.insert(body.end(), text.begin(), text.end());
    body.push_back(uint8_t(name_m - value_m));
    body.push_back(uint8_t(11)); // "MessageText"
    const std::string sv_name = "MessageText";
    body.insert(body.end(), sv_name.begin(), sv_name.end());
    body.push_back(0xFF); // loop B end
    body.push_back(0x01); // parent ref: the Folder
    put32le(body, 0x5678);
    body.push_back(0x23); // clock
    put32le(body, 0x0D6828);
    body.push_back(0x00); // chain end

    rbxclient::PropTagTable pt = rbxclient::propTagsForSchema(s);
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83ItemsEx(body.data(), body.size(), s, &pt, w,
                                     &err));
    CHECK(w.complete);
    CHECK(w.end_off == body.size());
    REQUIRE(w.items.size() == 3);
    const auto& folder = w.items[0].spawn;
    CHECK(s.classes[folder.cls].name == "Folder");
    CHECK(folder.id == 0x5678u);
    REQUIRE(folder.has_bulk);
    CHECK(folder.bulk.key_tag == 46); // Tags, not Source (47)
    CHECK(folder.bulk.key_len == 16);
    CHECK(folder.parent == 12019); // the bulk's anchor ref
    REQUIRE(folder.bulk.payload.size() == name.size());
    CHECK(std::string(folder.bulk.payload.begin(), folder.bulk.payload.end()) ==
          name);
    CHECK(std::memcmp(folder.bulk.key, hash, 16) == 0);
    // The opaque content must never surface as a 0x78 script bulk.
    for (const auto& it : w.items)
        CHECK(it.type != 0x78);
    const auto& sv = w.items[1].spawn;
    CHECK(s.classes[sv.cls].name == "StringValue");
    CHECK(sv.id == 0x5679u);
    CHECK(sv.parent == int64_t(folder.id));
    bool named = false, valued = false;
    for (const auto& p : sv.props_b) {
        if (p.tag == 49)
            named = std::string(p.value.begin() + 1, p.value.end()) == sv_name;
        else if (p.tag == 2)
            valued = std::string(p.value.begin() + 1, p.value.end()) == text;
    }
    CHECK(named);
    CHECK(valued);
    CHECK(w.items[2].type == 0x23);
}

TEST_CASE("spawn: non-script bytes are never taken for a content bulk") {
    // A bulk is content-addressed by a 16-byte hash the spawn itself carries
    // (Source for scripts, Tags for the chat Folder). A spawn with no such
    // prop must stop the walk pinned instead of inventing a phantom 0x78
    // (seen live: a 0x0d chain's ordinary bytes matched the old 16-byte key
    // test).
    const rbxclient::JoinSchema s = load_schema();
    size_t cls = s.classes.size();
    for (size_t i = 0; i < s.classes.size(); ++i)
        if (s.classes[i].name == "Part") {
            cls = i;
            break;
        }
    REQUIRE(cls < s.classes.size());

    std::vector<uint8_t> body{0x83, 0x02, 0x01, 0x34, 0x12, 0x00, 0x00,
                              uint8_t(cls), uint8_t(cls >> 8)};
    body.push_back(0xFF); // loop A end
    body.push_back(0xFF); // loop B end
    body.push_back(0x01); // parent ref lo
    put32le(body, 0x5678);
    const size_t fake_key = body.size();
    for (int i = 0; i < 16; ++i)
        body.push_back(uint8_t(0x90 + i));
    body.push_back(0x00); // marker
    body.push_back(3);    // payload length
    body.insert(body.end(), {'a', 'b', 'c'});
    body.push_back(0x01); // anchor ref lo
    put32le(body, 0x5678);
    body.push_back(0x00); // chain end

    rbxclient::PropTagTable pt = rbxclient::propTagsForSchema(s);
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83ItemsEx(body.data(), body.size(), s, &pt, w,
                                     &err));
    CHECK_FALSE(w.complete);
    CHECK(w.stop_off == fake_key);
    CHECK(w.stop_type == 0x90);
    for (const auto& it : w.items)
        CHECK(it.type != 0x78);
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
        CHECK(w.items[i].prop03.tag == 34);
        CHECK(w.items[i].prop03.vfmt == 0x01);
        CHECK(w.items[i].prop03.value == std::vector<uint8_t>({0x01}));
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
    CHECK(w.items[0].prop03.tag == 2);
    CHECK(w.items[0].prop03.vfmt == 0x01);
    REQUIRE(w.items[0].prop03.value.size() == 76);
    CHECK(w.items[0].prop03.value[0] == 0x4B);
    CHECK(w.items[1].type == 0x23);
}

TEST_CASE("prop03: default table pins the full native map + overrides") {
    // 10146 native entries (== old prop_bound+1, register-dumped) plus
    // solver-proven extensions past the dumped range (10268:25 s16x3 —
    // rolling builds grow prop_bound; the 122 in-between stay unmapped
    // and fail loudly). Spot pins mix native numbers (0:7 varint, 1:27
    // CFrame, 2:10 zigzag, 3:9 bool) with wire overrides (3796:61 ...).
    const rbxclient::PropTagTable pt = rbxclient::defaultPropTags();
    REQUIRE(pt.tag.size() == 10146); // 10145 members + sentinel
    CHECK(pt.tag.at(248) == 61);
    CHECK(pt.tag.at(3796) == 2);
    CHECK(pt.tag.at(4989) == 34);
    CHECK(pt.tag.at(5018) == 2);
    CHECK(pt.tag.at(5469) == 34);
    CHECK(pt.tag.at(5868) == 28);
    CHECK(pt.tag.at(5918) == 46);
    CHECK(pt.tag.at(10145) == 28); // sentinel slot: the Parent ref property
    CHECK(pt.tag.count(10146) == 0); // past the bound: loud on sight
}

TEST_CASE("propTagsForSchema: frozen schema reproduces the native map") {
    // The native runtime prop table is the schema's class-member
    // concatenation + one sentinel entry; deriving it must reproduce the
    // register dump and every solver override exactly (10145 members +
    // sentinel). This is the scaling path for builds that insert members.
    const rbxclient::JoinSchema s = load_schema();
    const rbxclient::PropTagTable got = rbxclient::propTagsForSchema(s);
    const rbxclient::PropTagTable want = rbxclient::defaultPropTags();
    CHECK(got.tag == want.tag);
    CHECK(got.prefix == want.prefix);
    CHECK(got.fixed_len == want.fixed_len);
    CHECK(got.alt_len == want.alt_len);
    CHECK(got.tag.size() == 10146);
    CHECK(got.tag.at(10145) == 28); // sentinel = out-of-band Parent ref
    // Schema tags are the wire truth; the retired solver overrides had
    // pinned 5446:61 (Content) and 5033:14B, which the vfmt byte disproved.
    CHECK(got.tag.at(5446) == 27); // Part.CFrame
    CHECK(got.tag.at(5033) == 9);
    CHECK(got.fixed_len.count(5033) == 0);
    CHECK(got.alt_len.count(5033) == 0);
}

TEST_CASE("prop03: the member-count sentinel is the Parent ref") {
    // Live 2096-spawn capture: every sentinel value is 5 bytes and the byte
    // right after it starts a child spawn whose parent ref == the triple's
    // entity. Reading it as tag 25 (6 bytes) swallowed that type byte.
    const rbxclient::JoinSchema s = load_schema();
    const rbxclient::PropTagTable pt = rbxclient::propTagsForSchema(s);
    REQUIRE(pt.bound == 10145);
    CHECK(pt.tag.at(pt.bound) == 28); // ref

    const uint16_t bound = pt.bound;
    std::vector<uint8_t> body{0x83, 0x03, 0x01, 0x34, 0x12, 0x00, 0x00,
                              uint8_t(bound & 0xff), uint8_t(bound >> 8),
                              0x01}; // vfmt
    body.push_back(0x01); // value ref lo
    put32le(body, 0x5678);
    body.push_back(0x00); // chain end

    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83ItemsEx(body.data(), body.size(), s, &pt, w,
                                     &err));
    CHECK(w.complete);
    CHECK(w.end_off == body.size());
    REQUIRE(w.items.size() == 1);
    CHECK(w.items[0].type == 0x03);
    CHECK(w.items[0].prop03.prop == bound);
    CHECK(w.items[0].prop03.tag == 28);
    CHECK(w.items[0].prop03.id == 0x1234u);
    REQUIRE(w.items[0].prop03.value.size() == 5);
    CHECK(w.items[0].prop03.value[0] == 0x01);
    CHECK(w.items[0].prop03.value[4] == 0x00);
}

TEST_CASE("propTagsForSchema: insertions shift ids, overrides follow names") {
    rbxclient::JoinSchema s = load_schema();
    // Global id of Part's first member and of Part.CFrame under this
    // schema (frozen ground truth: CFrame == 5446).
    uint32_t base = 0;
    size_t part = 0;
    bool found = false;
    for (size_t ci = 0; ci < s.classes.size(); ++ci) {
        if (s.classes[ci].name == "Part") {
            part = ci;
            found = true;
            break;
        }
        base += uint32_t(s.classes[ci].members.size());
    }
    REQUIRE(found);
    uint32_t cframe = base;
    bool have_cframe = false;
    for (const auto& m : s.classes[part].members) {
        if (m.name == "CFrame") {
            have_cframe = true;
            break;
        }
        ++cframe;
    }
    REQUIRE(have_cframe);
    CHECK(cframe == 5446);
    // Insert a synthetic member at the head of Part: every later id
    // shifts by one. A static id-keyed table would now mis-tag CFrame;
    // the name-keyed override must follow the member.
    rbxclient::SchemaMember extra;
    extra.name = "__inserted_member";
    extra.tag = 9;
    s.classes[part].members.insert(s.classes[part].members.begin(), extra);
    const rbxclient::PropTagTable got = rbxclient::propTagsForSchema(s);
    CHECK(got.tag.at(base) == 9);        // the inserted member
    CHECK(got.tag.at(cframe + 1) == 27); // Part.CFrame schema tag
    CHECK(got.tag.at(10146) == 28);      // sentinel moved with the bound
    CHECK(got.tag.count(10145) == 1);    // old sentinel is now a member id
}

TEST_CASE("propTagsForSchema: new-build schema reproduces the runtime table") {
    // run/join97_new_frame.bin is the live 0x97 frame from the updated
    // client (md5 30ecaff9..., new outer magic 50 d3);
    // run/proptable_new.tsv is the replicator table register-dumped from
    // the same build. The schema member-tag concatenation must match the
    // runtime table, including the sentinel entry (the Parent property).
    const auto frame =
        slurp(fs::path(REPO_ROOT) / "run" / "join97_new_frame.bin");
    REQUIRE(frame.size() == 54105); // exact stream frame (decomp 356983)
    CHECK(frame[0] == 0x97);
    CHECK(frame[1] == 0x50);
    CHECK(frame[2] == 0xD3);
    std::vector<uint8_t> inner;
    rbxclient::JoinSchema s;
    std::string err;
    INFO("outer err: " << err);
    REQUIRE(rbxclient::parseJoinOuter(frame.data(), frame.size(), inner, &err));
    CHECK(inner.size() == 356983);
    INFO("schema err: " << err);
    REQUIRE(rbxclient::parseJoinSchema(inner.data(), inner.size(), s, &err));
    CHECK(s.version == 3);
    CHECK(s.classes.size() == 636);
    CHECK(s.prop_bound == 10301);
    size_t members = 0;
    for (const auto& c : s.classes)
        members += c.members.size();
    CHECK(members == 10301);

    std::map<uint32_t, uint8_t> want;
    {
        std::ifstream f(fs::path(REPO_ROOT) / "run" / "proptable_new.tsv");
        REQUIRE(f);
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            std::istringstream is(line);
            uint32_t id = 0;
            int tag = 0;
            is >> id >> tag;
            want[id] = uint8_t(tag);
        }
    }
    REQUIRE(want.size() == 10302);

    // The schema member-tag concatenation is exactly the native runtime
    // table (the derivation's input). Wire-shape overrides are applied
    // afterwards and intentionally differ at their members.
    size_t bad = 0;
    uint32_t id = 0;
    for (const auto& c : s.classes)
        for (const auto& m : c.members) {
            if (m.tag != want.at(id))
                ++bad;
            ++id;
        }
    CHECK(bad == 0);
    const rbxclient::PropTagTable got = rbxclient::propTagsForSchema(s);
    CHECK(got.tag.size() == 10302); // 10301 members + sentinel
    CHECK(got.tag.at(10301) == 28); // sentinel = out-of-band Parent ref

    // Name-keyed overrides follow the shifted member ids on this build.
    auto id_of = [&s](const char* cls, const char* member) -> int {
        uint32_t n = 0;
        for (const auto& c : s.classes)
            for (const auto& m : c.members) {
                if (c.name == cls && m.name == member)
                    return int(n);
                ++n;
            }
        return -1;
    };
    const int cframe = id_of("Part", "CFrame");
    const int text = id_of("Message", "Text");
    const int owner = id_of("MeshPart", "NetworkOwnerV3");
    REQUIRE(cframe >= 0);
    REQUIRE(text >= 0);
    REQUIRE(owner >= 0);
    CHECK(cframe == 5533); // was 5446 in the frozen 630-class schema
    CHECK(text == 5104);   // was 5018
    CHECK(owner == 5075);  // was 4989
    CHECK(got.tag.at(uint16_t(cframe)) == 27);
    CHECK(got.tag.at(uint16_t(text)) == 2);
    CHECK(got.tag.at(uint16_t(owner)) == 34);
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
    CHECK(w.items[0].prop03.tag == 61);
    CHECK(w.items[0].prop03.vfmt == 0x01);
    CHECK(w.items[0].prop03.value ==
          std::vector<uint8_t>(
              {0x01, 0x03, 0xFC, 0xF9, 0x87, 0xA4, 0xB2, 0xE7, 0x29}));
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
        CHECK(w.items[i].prop03.tag == 34);
        CHECK(w.items[i].prop03.vfmt == 0x01);
        CHECK(w.items[i].prop03.value == std::vector<uint8_t>({0x01}));
    }
}

TEST_CASE("prop03: Content touch-text + Parent triples chain to exact close") {
    // Fresh-session prop 5018 (421B): the schema tag is 2 (string), so the
    // value is [0x23 len]["Touch the beacon again to heal you!"]. It then
    // chains 01/1b items, two Parent triples (prop id == member-count bound,
    // tag 28 ref, 5 bytes) and five Part spawns, closing exactly at 421.
    // This used to stop at a phantom 0x7d because the sentinel was read as
    // a 6-byte tag-25 value, which ate the spawn's type byte.
    const rbxclient::JoinSchema s = load_schema();
    const auto m = slurp(vec("prop03_5018.bin"));
    const rbxclient::PropTagTable pt = rbxclient::defaultPropTags();
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83ItemsEx(m.data(), m.size(), s, &pt, w, &err));
    CHECK(w.complete);
    CHECK(w.end_off == m.size());
    REQUIRE(w.items.size() == 14);
    CHECK(w.items[0].type == 0x03);
    CHECK(w.items[0].prop03.prop == 5018);
    CHECK(w.items[0].prop03.tag == 2);
    CHECK(w.items[0].prop03.vfmt == 0x01);
    REQUIRE(w.items[0].prop03.value.size() == 36);
    CHECK(w.items[0].prop03.value[0] == 0x23);
    CHECK(std::string(w.items[0].prop03.value.begin() + 1,
                      w.items[0].prop03.value.end()) ==
          "Touch the beacon again to heal you!");
    CHECK(w.items[3].type == 0x03);
    CHECK(w.items[3].prop03.prop == pt.bound);
    CHECK(w.items[3].prop03.tag == 28);
    CHECK(w.items[3].prop03.value.size() == 5);
    CHECK(w.items[4].type == 0x03);
    CHECK(w.items[4].prop03.tag == 28);
    CHECK(w.items[5].type == 0x02); // the child that follows its Parent triple
}

TEST_CASE("prop03: guid triple chains past the former 0x70 stop") {
    // @603831 (570B): 0x20 blob + spawns + prop-5918 triple (tag 46: 16B
    // guid). The pre-vfmt reading consumed the format byte as part of the
    // guid and stopped at 0x70; the correct split (vfmt + 16B) continues
    // through the 0x70 item and closes exactly at 570.
    const rbxclient::JoinSchema s = load_schema();
    const auto m = slurp(vec("prop03_5918.bin"));
    REQUIRE(m.size() == 570);
    const rbxclient::PropTagTable pt = rbxclient::defaultPropTags();
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83ItemsEx(m.data(), m.size(), s, &pt, w, &err));
    CHECK(w.complete);
    CHECK(w.end_off == m.size());
    REQUIRE(w.items.size() == 13);
    CHECK(w.items[3].type == 0x03);
    CHECK(w.items[3].prop03.prop == 5918);
    CHECK(w.items[3].prop03.tag == 46);
    CHECK(w.items[3].prop03.vfmt == 0x01);
    CHECK(w.items[3].prop03.value ==
          std::vector<uint8_t>({0xCA, 0xE6, 0x69, 0x41, 0xD9, 0xEF, 0xBD,
                                 0x40, 0x4E, 0x4D, 0x88, 0x75, 0x8E, 0xA6,
                                 0x76, 0x70})); // 16B GUID; 0x70 is data
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
                            0x01, 0x02, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04,
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
                            0x01, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x03,
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
    // Custom table maps prop 6002 -> tag 60. Value: [varint schema=1]
    // [8B id][varint v=1][7B tail] = 17B. Live UnionOperation spawns pin the
    // tail: both unions in the 2026-11-06 MM2 batch carry the same 17 bytes,
    // and 17 is the only length that keeps that record's member deltas
    // strictly increasing (all 961 records in the batch tile EOF-exact).
    // Null schema (0xFFFF) carries no id and no tail.
    rbxclient::PropTagTable pt;
    pt.tag[6002] = 60;
    const uint8_t body[] = {0x01, 0x05, 0x00, 0x00, 0x00, 0x72, 0x17,
                            0x01, 0x01, 0x07, 0x01, 0x02, 0x03, 0x04, 0x05,
                            0x06, 0x07, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00};
    rbxclient::PropTriple t;
    size_t used = 0;
    std::string err;
    INFO("triple err: " << err);
    REQUIRE(rbxclient::parsePropTriple(body, sizeof body, pt, t, &used,
                                       &err));
    CHECK(t.prop == 6002);
    CHECK(t.tag == 60);
    CHECK(t.value.size() == 17);
    CHECK(used == sizeof body);
    // Null form: schema 0xFFFF (LEB128 FF FF 03), no id follows.
    const uint8_t nullbody[] = {0x01, 0x05, 0x00, 0x00, 0x00, 0x72, 0x17,
                                    0x01, 0xFF, 0xFF, 0x03};
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

TEST_CASE("item09: checked u8 walks iff v == 4") {
    // sub_361440A: [09][u8]; v != 4 throws natively -> stop pinned.
    const rbxclient::JoinSchema s = load_schema();
    const uint8_t m[] = {0x83, 0x09, 0x04, 0x00};
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m, sizeof m, s, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == sizeof m);
    REQUIRE(w.items.size() == 1);
    CHECK(w.items[0].type == 0x09);
    CHECK(w.items[0].v09.v == 4);
    CHECK(!w.items[0].v09.rejected);
    // v = 5: structurally fine, natively throwing -> stop pinned here.
    const uint8_t bad[] = {0x83, 0x09, 0x05, 0x00};
    rbxclient::Walk83 w2;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(bad, sizeof bad, s, w2, &err));
    CHECK(!w2.complete);
    CHECK(w2.stop_type == 0x09);
    CHECK(w2.stop_off == 1);
    CHECK(w2.items.empty());
}

TEST_CASE("item0b: N == 0 null-drops, N != 0 zstd batch parses") {
    // sub_65C578A: [0b][u32 N]; N == 0 is the LABEL_112 null-drop (no
    // throw, chain continues). N != 0 is [u32 comp][u32 decomp][zstd];
    // the payload holds N [guid ref1][u16 class][props][guid ref2]
    // entries (sub_64D8394 prop loop).
    const rbxclient::JoinSchema s = load_schema();
    const uint8_t m[] = {0x83, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00};
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m, sizeof m, s, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == sizeof m);
    REQUIRE(w.items.size() == 1);
    CHECK(w.items[0].type == 0x0b);
    CHECK(w.items[0].head0b.n == 0);
    CHECK(w.items[0].head0b.dropped);

    // One entry: null ref1, class 0, empty prop loop, null ref2.
    const std::vector<uint8_t> raw = {0x00, 0x00, 0x00, 0xFF, 0x00};
    const auto comp = zstd_compress(raw);
    std::vector<uint8_t> nz = {0x83, 0x0b};
    put32le(nz, 1);
    put32le(nz, uint32_t(comp.size()));
    put32le(nz, uint32_t(raw.size()));
    nz.insert(nz.end(), comp.begin(), comp.end());
    nz.push_back(0x00);
    rbxclient::Walk83 w2;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(nz.data(), nz.size(), s, w2, &err));
    REQUIRE(w2.complete);
    CHECK(w2.end_off == nz.size());
    REQUIRE(w2.items.size() == 1);
    const auto& h = w2.items[0].head0b;
    CHECK(h.n == 1);
    CHECK(!h.dropped);
    REQUIRE(h.entries.size() == 1);
    CHECK(h.entries[0].cls == 0);
    CHECK(h.entries[0].ref1.lo == 0);
    CHECK(h.entries[0].ref2.lo == 0);
    CHECK(h.entries[0].props.empty());
    CHECK(h.batch_tail == 0);

    // A truncated comp length is a hard structural fail, never a gap.
    const uint8_t bad[] = {0x83, 0x0b, 0x01, 0x00, 0x00, 0x00, 0x00};
    rbxclient::Walk83 w3;
    CHECK(!rbxclient::walk83Items(bad, sizeof bad, s, w3, &err));
}

TEST_CASE("item14: triplet + two u32s, live two-item chain closes") {
    // Live 33B message (1818): two narrow 0x14 items, each followed by a
    // clock and the 00 end. The former greedy third tail ate the clock's
    // first four bytes and stopped the walk at 0xfe.
    const rbxclient::JoinSchema s = load_schema();
    const uint8_t m[] = {
        0x83, 0x14, 0x00, 0x00, 0x00, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0xFE, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x23, 0xC0, 0x4B, 0x0C, 0x00, 0x00};
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m, sizeof m, s, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == sizeof m);
    REQUIRE(w.items.size() == 3);
    CHECK(w.items[0].type == 0x14);
    const auto& t = w.items[0].v14;
    CHECK(!t.t.wide);
    CHECK(t.t.v[0] == 0x00u);
    CHECK(t.t.v[2] == 0xFEu);
    CHECK(t.a == 0u);
    CHECK(t.has_b);
    CHECK(t.b == 0u);
    const auto& t2 = w.items[1].v14;
    CHECK(t2.a == 1u);
    CHECK(t2.b == 0u);
    CHECK(w.items[2].type == 0x23);
    CHECK(w.items[2].clock == 0x000C4BC0u);
}

TEST_CASE("item18: triplet + two u32s walks") {
    // sub_35797E8: [18][flag=1][3x u32][a][b][00]. Fixed.
    const rbxclient::JoinSchema s = load_schema();
    const uint8_t m[] = {0x83, 0x18, 0x01, 0x01, 0x00, 0x00, 0x00,
                         0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
                         0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22,
                         0x00};
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m, sizeof m, s, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == sizeof m);
    REQUIRE(w.items.size() == 1);
    CHECK(w.items[0].type == 0x18);
    CHECK(w.items[0].v18.t.v[2] == 3u);
    CHECK(w.items[0].v18.a == 0x11111111u);
    CHECK(w.items[0].v18.b == 0x22222222u);
}

TEST_CASE("item19: 3xf32 + f64 + u32 walks") {
    // sub_3578E52: [19][12B][8B][4B][00] = 24B fixed. Bits preserved.
    const rbxclient::JoinSchema s = load_schema();
    const uint8_t m[] = {
        0x83, 0x19, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x40,
        0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xF0, 0x3F, 0xEF, 0xBE, 0xAD, 0xDE, 0x00};
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m, sizeof m, s, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == sizeof m);
    REQUIRE(w.items.size() == 1);
    CHECK(w.items[0].type == 0x19);
    CHECK(w.items[0].v19.trip[0] == 0x3F800000u); // 1.0f
    CHECK(w.items[0].v19.trip[1] == 0x40000000u); // 2.0f
    CHECK(w.items[0].v19.trip[2] == 0x40400000u); // 3.0f
    CHECK(w.items[0].v19.dbl_bits == 0x3FF0000000000000ull); // 1.0
    CHECK(w.items[0].v19.tail == 0xDEADBEEFu);
}

TEST_CASE("item1a: u32 + triplet + union tail walks") {
    // sub_3578F48: [1a][a][flag=0][3x u8][b][f=1.0f][b1=1][b2=0]
    // [u][00] (session-gated u32 present) and the short form without u.
    const rbxclient::JoinSchema s = load_schema();
    const uint8_t m[] = {
        0x83, 0x1a, 0x44, 0x33, 0x22, 0x11, 0x00, 0x07, 0x08, 0x09,
        0xDD, 0xCC, 0xBB, 0xAA, 0x00, 0x00, 0x80, 0x3F, 0x01, 0x00,
        0xEF, 0xBE, 0xAD, 0xDE, 0x00};
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m, sizeof m, s, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == sizeof m);
    REQUIRE(w.items.size() == 1);
    CHECK(w.items[0].type == 0x1a);
    const auto& t = w.items[0].v1a;
    CHECK(t.a == 0x11223344u);
    CHECK(!t.t.wide);
    CHECK(t.t.v[0] == 7u);
    CHECK(t.b == 0xAABBCCDDu);
    CHECK(t.f_bits == 0x3F800000u);
    CHECK(t.b1 == 1);
    CHECK(t.b2 == 0);
    CHECK(t.has_u);
    CHECK(t.u == 0xDEADBEEFu);
    // Short form: no trailing u32 (ends at b2 + 00).
    const uint8_t sh[] = {0x83, 0x1a, 0x01, 0x00, 0x00, 0x00, 0x01,
                          0x0A, 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00,
                          0x0C, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    rbxclient::Walk83 w2;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(sh, sizeof sh, s, w2, &err));
    REQUIRE(w2.complete);
    REQUIRE(w2.items.size() == 1);
    CHECK(!w2.items[0].v1a.has_u);
    CHECK(w2.items[0].v1a.t.wide);
}

TEST_CASE("item1d/1e: single entity refs walk") {
    // sub_3579956 / sub_3579AAA: one 336800E guid each.
    const rbxclient::JoinSchema s = load_schema();
    const uint8_t m[] = {0x83, 0x1d, 0x02, 0x07, 0x00, 0x00, 0x00, 0x1e,
                         0x00, 0x00};
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m, sizeof m, s, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == sizeof m);
    REQUIRE(w.items.size() == 2);
    CHECK(w.items[0].type == 0x1d);
    CHECK(w.items[0].ref1d.ref.lo == 2);
    CHECK(w.items[0].ref1d.ref.id == 7);
    CHECK(w.items[1].type == 0x1e);
    CHECK(w.items[1].ref1e.ref.lo == 0);
    CHECK(w.items[1].ref1e.ref.id == -1);
}

TEST_CASE("item1f: bounded blob, null-drop, overcap stop") {
    // sub_35C0CE6: [1f][a=3][b][u32 len][bytes][00]; a == 1 null-drops
    // (a+b only); len > 0x40000 throws natively -> stop pinned.
    const rbxclient::JoinSchema s = load_schema();
    const uint8_t m[] = {0x83, 0x1f, 0x03, 0x07, 0x03, 0x00,
                         0x00, 0x00, 'a',  'b',  'c',  0x00};
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m, sizeof m, s, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == sizeof m);
    REQUIRE(w.items.size() == 1);
    CHECK(w.items[0].type == 0x1f);
    CHECK(w.items[0].blob1f.a == 3);
    CHECK(w.items[0].blob1f.b == 7);
    CHECK(!w.items[0].blob1f.dropped);
    CHECK(w.items[0].blob1f.blob ==
          std::vector<uint8_t>({'a', 'b', 'c'}));
    // a == 1: null-drop, chain continues past a+b.
    const uint8_t nd[] = {0x83, 0x1f, 0x01, 0x09, 0x00};
    rbxclient::Walk83 w2;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(nd, sizeof nd, s, w2, &err));
    REQUIRE(w2.complete);
    REQUIRE(w2.items.size() == 1);
    CHECK(w2.items[0].blob1f.dropped);
    CHECK(w2.items[0].blob1f.blob.empty());
    // len 0x40001 > cap: native throw -> stop pinned, never a throw here.
    const uint8_t oc[] = {0x83, 0x1f, 0x03, 0x00, 0x01, 0x00,
                          0x04, 0x00, 0x00};
    rbxclient::Walk83 w3;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(oc, sizeof oc, s, w3, &err));
    CHECK(!w3.complete);
    CHECK(w3.stop_type == 0x1f);
    CHECK(w3.stop_off == 1);
    CHECK(w3.items.empty());
}

TEST_CASE("item24/28: single u32 items walk") {
    // sub_357945C / sub_357A18C: [type][u32][00]. Fixed.
    const rbxclient::JoinSchema s = load_schema();
    const uint8_t m[] = {0x83, 0x24, 0x78, 0x56, 0x34, 0x12, 0x28,
                         0xEF, 0xBE, 0xAD, 0xDE, 0x00};
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m, sizeof m, s, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == sizeof m);
    REQUIRE(w.items.size() == 2);
    CHECK(w.items[0].type == 0x24);
    CHECK(w.items[0].v24.v == 0x12345678u);
    CHECK(w.items[1].type == 0x28);
    CHECK(w.items[1].v28.v == 0xDEADBEEFu);
}

TEST_CASE("item25: N == 0 elements + ref + u32 + bool walk") {
    // sub_65C83BE -> sub_65C8292 with N == 0 (the 0x0B head null-drops
    // and reads nothing further): [25][0][elem flags=0x0a:
    // (f&0xF7)!=0 so no payload, continue][elem flags=0x02: end]
    // [guid lo=1 id][u32][bool][00].
    const rbxclient::JoinSchema s = load_schema();
    const uint8_t m[] = {0x83, 0x25, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x02,
                         0x01, 0x09, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33,
                         0x44, 0x01, 0x00};
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m, sizeof m, s, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == sizeof m);
    REQUIRE(w.items.size() == 1);
    CHECK(w.items[0].type == 0x25);
    const auto& t = w.items[0].v25;
    CHECK(t.batch.n == 0);
    CHECK(t.batch.dropped);
    REQUIRE(t.elems.size() == 2);
    CHECK(t.elems[0].flags == 0x0a);
    CHECK(t.elems[0].payload.empty());
    CHECK(t.elems[1].flags == 0x02);
    CHECK(t.ref.lo == 1);
    CHECK(t.ref.id == 9);
    CHECK(t.v == 0x44332211u);
    CHECK(t.flag == 1);
}

TEST_CASE("item25: N != 0 batch + elements + ref + u32 + bool walk") {
    // Same item with a non-empty 0x0B batch: [25][N=1][comp][decomp]
    // [zstd entry][elem flags=0x01: end][guid null][u32][bool][00].
    const rbxclient::JoinSchema s = load_schema();
    const std::vector<uint8_t> raw = {0x00, 0x00, 0x00, 0xFF, 0x00};
    const auto comp = zstd_compress(raw);
    std::vector<uint8_t> m = {0x83, 0x25};
    put32le(m, 1);
    put32le(m, uint32_t(comp.size()));
    put32le(m, uint32_t(raw.size()));
    m.insert(m.end(), comp.begin(), comp.end());
    m.push_back(0x01); // element flags: (f&0xF7)!=0 -> no payload, end
    m.push_back(0x00); // tail guid null
    put32le(m, 0x44332211u);
    m.push_back(0x01);
    m.push_back(0x00); // 0x83 end marker
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m.data(), m.size(), s, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == m.size());
    REQUIRE(w.items.size() == 1);
    const auto& t = w.items[0].v25;
    CHECK(t.batch.n == 1);
    REQUIRE(t.batch.entries.size() == 1);
    CHECK(t.batch.entries[0].cls == 0);
    CHECK(t.batch.entries[0].ref1.lo == 0);
    CHECK(t.batch.entries[0].ref2.lo == 0);
    REQUIRE(t.elems.size() == 1);
    CHECK(t.elems[0].flags == 0x01);
    CHECK(t.ref.lo == 0);
    CHECK(t.v == 0x44332211u);
    CHECK(t.flag == 1);

    // A truncated batch is a hard structural fail, never a gap.
    const uint8_t nz[] = {0x83, 0x25, 0x02, 0x00, 0x00, 0x00, 0x00};
    rbxclient::Walk83 w2;
    CHECK(!rbxclient::walk83Items(nz, sizeof nz, s, w2, &err));
}

TEST_CASE("item26: zigzag varint walks, server-null drops") {
    // sub_3498B2E: [26][varint V] -> s64 (V >> 1) ^ -(V & 1). V = 2 -> 1,
    // V = 1 -> -1. Server-gated null reads zero bytes (direct call).
    const rbxclient::JoinSchema s = load_schema();
    const uint8_t m[] = {0x83, 0x26, 0x02, 0x26, 0x01, 0x00};
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m, sizeof m, s, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == sizeof m);
    REQUIRE(w.items.size() == 2);
    CHECK(w.items[0].type == 0x26);
    CHECK(w.items[0].v26.v == 1);
    CHECK(!w.items[0].v26.dropped);
    CHECK(w.items[1].v26.v == -1);
    rbxclient::Item26 d;
    size_t used = 99;
    INFO("null err: " << err);
    REQUIRE(rbxclient::parseItem26(m + 2, 1, true, d, &used, &err));
    CHECK(d.dropped);
    CHECK(used == 0);
}

TEST_CASE("item27: u32 + 15x f32 walks") {
    // sub_357A288: [27][u32][60B][00]. Bits preserved (CFrame/scale
    // block hypothesis on semantics only).
    const rbxclient::JoinSchema s = load_schema();
    std::vector<uint8_t> m = {0x83, 0x27, 0x0D, 0x00, 0x00, 0x00};
    for (int i = 0; i < 15; ++i) {
        const uint32_t f = 0x3F800000u + uint32_t(i); // distinct bits
        m.push_back(uint8_t(f));
        m.push_back(uint8_t(f >> 8));
        m.push_back(uint8_t(f >> 16));
        m.push_back(uint8_t(f >> 24));
    }
    m.push_back(0x00);
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m.data(), m.size(), s, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == m.size());
    REQUIRE(w.items.size() == 1);
    CHECK(w.items[0].type == 0x27);
    CHECK(w.items[0].v27.id == 13u);
    CHECK(w.items[0].v27.f[0] == 0x3F800000u);
    CHECK(w.items[0].v27.f[14] == 0x3F80000Eu);
}

TEST_CASE("chain0914: new outer types chain with clock") {
    // Cross-type chain: 09 + 24 + 1d + 23 + end walks complete, exact.
    const rbxclient::JoinSchema s = load_schema();
    const uint8_t m[] = {0x83, 0x09, 0x04, 0x24, 0x78, 0x56, 0x34,
                         0x12, 0x1d, 0x01, 0x05, 0x00, 0x00, 0x00, 0x23,
                         0x11, 0x22, 0x33, 0x44, 0x00};
    rbxclient::Walk83 w;
    std::string err;
    INFO("walk err: " << err);
    REQUIRE(rbxclient::walk83Items(m, sizeof m, s, w, &err));
    REQUIRE(w.complete);
    CHECK(w.end_off == sizeof m);
    REQUIRE(w.items.size() == 4);
    CHECK(w.items[0].type == 0x09);
    CHECK(w.items[1].type == 0x24);
    CHECK(w.items[1].v24.v == 0x12345678u);
    CHECK(w.items[2].type == 0x1d);
    CHECK(w.items[2].ref1d.ref.id == 5);
    CHECK(w.items[3].type == 0x23);
    CHECK(w.items[3].clock == 0x44332211u);
}

TEST_CASE("item11: version-gated appearance body parses exact") {
    // sub_32EB3EA (ex-32BA84E): [u32 v](v>=9:[2x f32])(A:[u8](blob+vstr)?)
    // (B:[u32][blob][vstr])?(C:[f64][counts+arrays])?(v>=3:[u8](6x u32)?)
    // [u8](list1)?[u8](list2)?[5x f32]. All counts/sentinels wire-driven.
    // Minimal v=0: version + l1=1 + l2=1 + 20B tail = 26B exact.
    const rbxclient::JoinSchema s = load_schema();
    {
        const uint8_t m[] = {
            0x83, 0x11, 0x00, 0x00, 0x00, 0x00, // [11][u32 v=0]
            0x01, // l1: skip list1
            0x01, // l2: skip list2
            0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00,
            0x00, // 5x f32 tail
            0x00};
        rbxclient::Walk83 w;
        std::string err;
        INFO("walk err: " << err);
        REQUIRE(rbxclient::walk83Items(m, sizeof m, s, w, &err));
        CHECK(w.complete);
        CHECK(w.items.size() == 1);
        CHECK(w.items[0].type == 0x11);
        CHECK(w.items[0].appearance11.version == 0);
    }
    {
        // Truncated v=9 body (needs 2x f32 after version) hard-fails.
        const uint8_t m[] = {0x83, 0x11, 0x09, 0x00, 0x00, 0x00, 0x00};
        rbxclient::Walk83 w;
        std::string err;
        CHECK(!rbxclient::walk83Items(m, sizeof m, s, w, &err));
    }
}

TEST_CASE("0x03 shapes follow the schema tag after the vfmt byte") {
    // These Brookhaven fragments were once fitted as opaque 14/20B shapes.
    // The vfmt discovery (native sub_64CC280) shows the byte those fits
    // treated as a value prefix: [ref][u16 prop][u8 vfmt][tag-shaped value].
    // 5514 -> tag 22 (Vector3), 5033 -> tag 9 (u8), 5133 -> tag 11 (f32).
    const rbxclient::PropTagTable pt = rbxclient::defaultPropTags();
    CHECK(pt.tag.at(5514) == 22);
    CHECK(pt.tag.at(5033) == 9);
    CHECK(pt.tag.at(5133) == 11);
    CHECK(pt.fixed_len.count(5514) == 0);
    CHECK(pt.fixed_len.count(5033) == 0);
    CHECK(pt.fixed_len.count(5133) == 0);
    {
        const uint8_t m[] = {0x01, 0x7c, 0x07, 0x05, 0x00, 0x8a, 0x15,
                             0x00, 0x5b, 0x25, 0xc1, 0xc3, 0xdc, 0x34, 0x3d,
                             0x40, 0xeb, 0xe3, 0xb6, 0x42, 0x02};
        rbxclient::PropTriple t;
        size_t used = 0;
        std::string err;
        INFO("err: " << err);
        REQUIRE(rbxclient::parsePropTriple(m, sizeof m, pt, t, &used, &err));
        CHECK(t.prop == 5514);
        CHECK(t.vfmt == 0x00);
        CHECK(used == 20);
        CHECK(t.value.size() == 12);
        CHECK(m[used] == 0x02); // next chain item, not part of the value
    }
    {
        const uint8_t m[] = {0x01, 0x92, 0x19, 0x02, 0x00, 0xa9, 0x13,
                             0x01, 0x11, 0xeb, 0x9c, 0xc3, 0x20, 0x52, 0xc7,
                             0x40, 0x70, 0x9e, 0xb4, 0x41, 0x22, 0x03, 0x01,
                             0x0c, 0x06, 0x05, 0x00, 0x8a, 0x15};
        rbxclient::PropTriple t;
        size_t used = 0;
        std::string err;
        INFO("err: " << err);
        REQUIRE(rbxclient::parsePropTriple(m, sizeof m, pt, t, &used, &err));
        CHECK(t.prop == 5033);
        CHECK(t.vfmt == 0x01);
        CHECK(used == 9);
        CHECK(t.value == std::vector<uint8_t>({0x11}));
    }
    {
        const uint8_t m[] = {0x01, 0xc7, 0x9b, 0x0a, 0x00, 0xa9, 0x13,
                             0x00, 0x5e, 0x25, 0xc1, 0xc3, 0x20, 0x31, 0xb8,
                             0x40, 0xb2, 0xc1, 0xc7, 0x42, 0x00, 0x00, 0x00,
                             0x5e, 0x16, 0x00, 0x80};
        rbxclient::PropTriple t;
        size_t used = 0;
        std::string err;
        INFO("err: " << err);
        REQUIRE(rbxclient::parsePropTriple(m, sizeof m, pt, t, &used, &err));
        CHECK(t.prop == 5033);
        CHECK(t.vfmt == 0x00);
        CHECK(used == 9);
        CHECK(t.value == std::vector<uint8_t>({0x5e}));
    }
    {
        const uint8_t m[] = {0x01, 0xb9, 0x82, 0x03, 0x00, 0x0d, 0x14,
                             0x01, 0xa2, 0x5e, 0x2f, 0x9b, 0xc3, 0x20, 0x52,
                             0xc7, 0x40, 0x70, 0x9e, 0xb4, 0x41, 0x22};
        rbxclient::PropTriple t;
        size_t used = 0;
        std::string err;
        INFO("err: " << err);
        REQUIRE(rbxclient::parsePropTriple(m, sizeof m, pt, t, &used, &err));
        CHECK(t.prop == 5133);
        CHECK(t.vfmt == 0x01);
        CHECK(used == 12);
        CHECK(t.value.size() == 4);
    }
}

TEST_CASE("top8d: region chunk envelope exact, inner carried") {
    // [8d][01 ec 1a 09 00][u32 comp][u32 decomp][zstd] (frame exact).
    // Frame bytes below are real zstd output (12B payload, level 3).
    const uint8_t fr[] = {0x28, 0xb5, 0x2f, 0xfd, 0x20, 0x0c, 0x61, 0x00,
                          0x00, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
                          0x37, 0x38, 0x39, 0x61, 0x62};
    std::vector<uint8_t> m = {0x8d, 0x01, 0xec, 0x1a, 0x09, 0x00,
                              0x15, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00,
                              0x00};
    m.insert(m.end(), std::begin(fr), std::end(fr));
    rbxclient::RegionChunk rc;
    std::string err;
    INFO("err: " << err);
    REQUIRE(rbxclient::parseRegionChunk(m.data(), m.size(), rc, &err));
    const std::string want = "0123456789ab";
    CHECK(rc.inner.size() == want.size());
    CHECK(std::string(rc.inner.begin(), rc.inner.end()) == want);
    // Bad prefix fails loudly.
    m[1] ^= 0xff;
    CHECK(!rbxclient::parseRegionChunk(m.data(), m.size(), rc, &err));
}

TEST_CASE("a1: 17-byte client-state envelope") {
    auto put32 = [](uint8_t* p, uint32_t v) {
        for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
    };
    uint8_t m[24] = {0};
    m[0] = 0xA1;
    put32(m + 1, 0x114224);
    put32(m + 5, 1);
    put32(m + 9, 0x7fffffffu);
    put32(m + 13, 2);
    m[17] = 0x01; // ref lo
    put32(m + 18, 0x3f97);
    m[22] = 0x2e; // class 558 (Tool)
    m[23] = 0x02;
    rbxclient::AppStateHeader h;
    REQUIRE(rbxclient::parseAppStateHeader(m, sizeof m, &h));
    CHECK(h.tick == 0x114224u);
    CHECK(h.records == 1u);
    CHECK(h.ack == 0x7fffffffu);
    CHECK(h.flags == 2u);
    CHECK(h.body == 17u);
    m[5] = 2; // two records need 14 body bytes, only 7 are present
    CHECK(!rbxclient::parseAppStateHeader(m, sizeof m, &h));
    m[5] = 1;
    m[0] = 0x83;
    CHECK(!rbxclient::parseAppStateHeader(m, sizeof m, &h));
    CHECK(!rbxclient::parseAppStateHeader(m, 16, &h));
}

TEST_CASE("a1: multi-record frame splitter keeps masks and payloads") {
    std::vector<uint8_t> m = {0xA1};
    auto put32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) m.push_back(uint8_t(v >> (8 * i)));
    };
    put32(0x114224); // tick
    put32(2);        // records
    put32(0x7fffffffu);
    put32(1);
    // record 1: Tool, 19 member slots, mask bit 0, data + separator
    m.push_back(0x01);
    put32(0x111);
    m.insert(m.end(), {0x2e, 0x02, 0x13, 0x00});
    m.insert(m.end(), {0x01, 0, 0, 0, 0, 0, 0, 0});
    m.insert(m.end(), {0x05, 0x00, 0x21, 0xAA, 0xBB, 0xCC, 0xDD});
    m.insert(m.end(), {0x01, 0, 0x01, 0});
    // record 2: Tool, no present slots, last record ends at EOF
    m.push_back(0x01);
    put32(0x222);
    m.insert(m.end(), {0x2e, 0x02, 0x13, 0x00});
    m.insert(m.end(), {0, 0, 0, 0, 0, 0, 0, 0});
    m.insert(m.end(), {0x01, 0x00, 0x2E});

    rbxclient::JoinSchema s;
    s.classes.resize(559);
    s.classes[558].name = "Tool";
    rbxclient::AppStateFrame f;
    std::string err;
    INFO("a1 frame err: " << err);
    REQUIRE(rbxclient::parseAppStateFrame(m.data(), m.size(), s, &f, &err));
    REQUIRE(f.records.size() == 2);
    CHECK(f.header.records == 2u);
    CHECK(f.records[0].id == 0x111u);
    CHECK(f.records[1].id == 0x222u);
    CHECK(f.records[0].cls == 558);
    CHECK(f.records[0].member_count == 19);
    CHECK(f.records[0].mask == 1u);
    CHECK(f.records[0].data == std::vector<uint8_t>({0x05, 0x00, 0x21, 0xAA, 0xBB, 0xCC, 0xDD, 0x01, 0x00, 0x01, 0x00}));
    CHECK(f.records[1].mask == 0u);
    CHECK(f.records[1].data == std::vector<uint8_t>({0x01, 0x00, 0x2E}));

    // Removing the optional marker still finds the next record by its
    // declared body length (the live Humanoid/Animator case).
    m.erase(m.begin() + 17 + 5 + 2 + 2 + 8 + 7, m.begin() + 17 + 5 + 2 + 2 + 8 + 11);
    CHECK(rbxclient::parseAppStateFrame(m.data(), m.size(), s, &f, &err));
}

TEST_CASE("a1: member counts over 64 use multiple mask words") {
    std::vector<uint8_t> m = {0xA1};
    auto put32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) m.push_back(uint8_t(v >> (8 * i)));
    };
    put32(1); // tick
    put32(1); // records
    put32(0);
    put32(0);
    m.push_back(0x01);
    put32(0x333);
    m.insert(m.end(), {0x76, 0x02, 0x45, 0x00}); // Workspace, 69
    for (int i = 0; i < 8; ++i) m.push_back(0);
    for (int i = 0; i < 8; ++i) m.push_back(i == 0 ? 1 : 0);
    m.insert(m.end(), {0x01, 0x00, 0x2e}); // body len 1 + payload

    rbxclient::JoinSchema s;
    s.classes.resize(631);
    s.classes[630].name = "Workspace";
    rbxclient::AppStateFrame f;
    std::string err;
    INFO("multiword err: " << err);
    REQUIRE(rbxclient::parseAppStateFrame(m.data(), m.size(), s, &f, &err));
    REQUIRE(f.records.size() == 1);
    CHECK(f.records[0].member_count == 69);
    REQUIRE(f.records[0].mask_words.size() == 2);
    CHECK(f.records[0].mask_words[0] == 0u);
    CHECK(f.records[0].mask_words[1] == 1u);
    CHECK(f.records[0].data == std::vector<uint8_t>({0x01, 0x00, 0x2E}));
}

TEST_CASE("spawn: four-byte ref lo does not consume the parent u32") {
    // Live LocalScript spawn (/tmp/stopdump/stop_1_4037.bin): lo e2 92 ed e8
    // is a 4-byte varint whose last high bit is data. An unbounded LEB128
    // reader eats be e4 40 from the u32 and desyncs the following bulk.
    rbxclient::JoinSchema s;
    s.classes.resize(1);
    s.classes[0].name = "X";
    const uint8_t item[] = {
        0x01, 0x2f, 0x00, 0x00, 0x00, // ref lo=1, id=0x2f
        0x00, 0x00,                   // class 0
        0xff, 0xff,             // prop loops A/B empty
        0xe2, 0x92, 0xed, 0xe8, // lo = 0x0d1b4962 (4-byte form)
        0xbe, 0xe4, 0x40, 0xf9  // parent id = 0xf940e4be
    };
    rbxclient::SpawnRecord r;
    size_t used = 0;
    std::string err;
    INFO("spawn err: " << err);
    REQUIRE(rbxclient::parseSpawnItem(item, sizeof item, s, false, r, &used,
                                      &err));
    CHECK(used == sizeof item);
    CHECK(r.plo == 0x0d1b4962u);
    CHECK(r.parent == int64_t(0xf940e4beu));
}

TEST_CASE("0xA1 ReplicationPV: native bit-packed vectors decode exactly") {
    // Three live 1818 values captured by the tag-57 census. The first is the
    // 24-byte shape (three vectors + the 5-byte orientation selector), the
    // second is a 14-byte shape (position + linear velocity only), and the
    // third is the 9-byte shape (position only). Bytes and lengths are the
    // native wire; the expected integers are what sub_6427536 reads.
    struct V {
        std::vector<uint8_t> bytes;
        uint8_t flags;
        int pos_raw[3];
        int lin_raw[3];
        int ang_raw[3];
        uint8_t kind;
        uint64_t code;
        size_t used;
    };
    const std::vector<V> samples = {
        {{0x10, 0x17, 0x27, 0xf4, 0xc3, 0x5c, 0x10, 0x23, 0x4c, 0xc0,
          0xfd, 0xfe, 0x7f, 0xff, 0xe0, 0xff, 0xd2, 0x70, 0x00, 0x01,
          0xd6, 0x6c, 0x1f, 0x61},
         0x10,
         {-48527, 23747, 311857},
         {-36, -2, -9},
         {-2, 210, 7},
         2,
         0x611f6cd601ull,
         24},
        {{0x40, 0x74, 0x87, 0x60, 0x31, 0x00, 0xa7, 0xa6, 0xfd, 0x2f,
          0xc9, 0xeb, 0x0e, 0x3c},
         0x40,
         {-1929, 352, 3},
         {-9622, -276177, 245998},
         {0, 0, 0},
         0,
         0,
         14},
        {{0x60, 0xb7, 0xfe, 0xb0, 0xcd, 0x14, 0x71, 0xb5, 0x6a},
         0x60,
         {-323605, 70861, 437079},
         {0, 0, 0},
         {0, 0, 0},
         0,
         0,
         9},
    };
    for (size_t si = 0; si < samples.size(); ++si) {
        const V& v = samples[si];
        rbxclient::ReplicationPV pv;
        size_t used = 0;
        std::string err;
        INFO("decode err: " << err);
        REQUIRE(rbxclient::decodeReplicationPV(v.bytes.data(), v.bytes.size(),
                                               pv, &used, &err));
        CHECK(used == v.used);
        CHECK(pv.flags == v.flags);
        CHECK(pv.position.present == !(v.flags & 1));
        CHECK(pv.linear_velocity.present == !(v.flags & 0x20));
        CHECK(pv.angular_velocity.present == !(v.flags & 0x40));
        CHECK(pv.orientation_kind == v.kind);
        CHECK(pv.orientation_code == v.code);
        for (int i = 0; i < 3; ++i)
            CHECK(pv.position.raw[i] == v.pos_raw[i]);
        if (pv.linear_velocity.present)
            for (int i = 0; i < 3; ++i)
                CHECK(pv.linear_velocity.raw[i] == v.lin_raw[i]);
        if (pv.angular_velocity.present)
            for (int i = 0; i < 3; ++i)
                CHECK(pv.angular_velocity.raw[i] == v.ang_raw[i]);
        // Native dequantization: raw * 2^(exponent - bits + 1).
        if (si == 0)
            CHECK(pv.position.value[0] ==
                  doctest::Approx(-48527.0 / 4096.0).epsilon(1e-12));
    }
}

TEST_CASE("0xA1 ReplicationPV: values are followed by the next member exactly") {
    // A Part selects CFrame (slot 0, tag 27) and ReplicationPV (slot 1, tag
    // 57). The tag-57 payload is the 9-byte position-only native shape; the
    // record must consume it exactly and decode the preceding CFrame too.
    rbxclient::JoinSchema s;
    s.classes.resize(1);
    s.classes[0].name = "Part";
    s.classes[0].members.push_back({"CFrame", 27, 0, false});
    s.classes[0].members.push_back({"ReplicationPV", 57, 0, false});
    s.slots.resize(1);
    s.slots[0] = {0, 1};
    s.member_by_gid = {{0, 0}, {0, 1}};

    std::vector<uint8_t> body(13, 0); // CFrame pos, kind 1 = table identity
    body[12] = 1;
    const uint8_t pv[] = {0x60, 0xb7, 0xfe, 0xb0, 0xcd,
                          0x14, 0x71, 0xb5, 0x6a};
    body.insert(body.end(), std::begin(pv), std::end(pv));
    rbxclient::AppStateRecord rec;
    rec.id = 99;
    rec.cls = 0;
    rec.member_count = 2;
    rec.mask_words = {0x3};
    rec.data = {uint8_t(body.size()), 0x00};
    rec.data.insert(rec.data.end(), body.begin(), body.end());

    std::vector<rbxclient::JoinProp> props;
    std::vector<uint8_t> tail;
    std::string err;
    bool partial = false;
    INFO("body err: " << err);
    REQUIRE(rbxclient::parseAppStateBody(
        rec.data.data() + 2, body.size(), s, rec, &props, &err, &tail,
        &partial));
    CHECK(!partial);
    CHECK(tail.empty());
    REQUIRE(props.size() == 2);
    CHECK(props[0].name == "CFrame");
    CHECK(props[0].tag == 27);
    CHECK(props[1].name == "ReplicationPV");
    CHECK(props[1].tag == 57);
    CHECK(props[1].value == std::vector<uint8_t>(std::begin(pv), std::end(pv)));
}

TEST_CASE("0xA1 body: native end restores the stream after guards and padding") {
    // A 189707 Part body: the recorded u16 end is where native rewinds to
    // (sub_6431154 captures stream-pos + u16). A bit-packed value may consume
    // bytes past that end; those bytes are the next record's guard and are
    // discarded. A body may also end past the value in alignment padding,
    // which native skips when it restores the same end.
    rbxclient::JoinSchema s;
    s.classes.resize(1);
    s.classes[0].name = "Part";
    s.classes[0].members.push_back({"ReplicationPV", 57, 0, false});
    s.slots.resize(1);
    s.slots[0] = {0};
    s.member_by_gid = {{0, 0}};
    rbxclient::AppStateRecord rec;
    rec.id = 7;
    rec.member_count = 1;
    rec.mask_words = {1};

    // flags 0x20: position + angular velocity. The value consumes 20 bytes.
    const std::vector<uint8_t> value = {
        0x20, 0x7d, 0x98, 0xa3, 0xbe, 0x74, 0xb5, 0xbc, 0xed, 0x5f,
        0xa2, 0x06, 0x3c, 0x37, 0xb2, 0xf3, 0xae};
    std::vector<uint8_t> body = {uint8_t(value.size()), 0x00};
    body.insert(body.end(), value.begin(), value.end());
    rec.data = body;
    std::vector<rbxclient::JoinProp> props;
    std::string err;
    bool partial = false;
    size_t padding = 1;
    // End exactly on the recorded body: the native zero guard supplies the
    // three bytes that complete the angular block.
    REQUIRE(rbxclient::parseAppStateBody(
        rec.data.data() + 2, value.size(), s, rec, &props, &err, nullptr,
        &partial, nullptr, nullptr, value.size(), &padding));
    CHECK(!partial);
    CHECK(padding == 0);
    REQUIRE(props.size() == 1);
    CHECK(props[0].tag == 57);

    // Same value plus an eight-byte buffer suffix: the recorded body end is
    // captured after the value's three-byte lookahead, and the five remaining
    // buffer bytes are reported as skipped. The u16 body length is the only
    // bound; no observed slack constant is consulted.
    body.insert(body.end(), {0xc0, 0x0f, 0x00, 0, 0, 0, 0, 0});
    rec.data = body;
    props.clear();
    rbxclient::A1BodyAccounting acct;
    REQUIRE(rbxclient::parseAppStateBody(
        rec.data.data() + 2, value.size() + 8, s, rec, &props, &err, nullptr,
        &partial, nullptr, nullptr, value.size() + 8, &padding, nullptr, 0,
        &acct));
    CHECK(!partial);
    CHECK(padding == 5);
    CHECK(acct.body_bytes == value.size() + 8);
    // The angular block's three-bit lookahead lands in body padding, so those
    // body bytes are read by the decoder and count as decoded; the remaining
    // five are the skippable tail.
    CHECK(acct.decoded_bytes == value.size() + 3);
    CHECK(acct.end_seek_skipped == 5);
    CHECK(acct.unaccounted == 0);
    REQUIRE(props.size() == 1);

    // A second selected member that has no wire width leaves the tail
    // unattributed; only the single-tag-57 buffer suffix is allowed.
    rbxclient::JoinSchema two = s;
    two.classes[0].members.push_back({"Empty", 3, 0, false});
    two.slots[0] = {0, 1};
    two.member_by_gid = {{0, 0}, {0, 1}};
    rbxclient::AppStateRecord two_sel;
    two_sel.id = 9;
    two_sel.member_count = 2;
    two_sel.mask_words = {3};
    std::vector<uint8_t> two_body = {uint8_t(value.size() + 8), 0x00};
    two_body.insert(two_body.end(), value.begin(), value.end());
    two_body.insert(two_body.end(), 8, 0xa5);
    two_sel.data = two_body;
    props.clear();
    CHECK_FALSE(rbxclient::parseAppStateBody(
        two_sel.data.data() + 2, value.size() + 8, two, two_sel, &props,
        &err));
    CHECK(err.find("non-PV") != std::string::npos);

    // An empty mask with a non-empty body has no selected member to attribute
    // any byte to, so it fails instead of being absorbed as end padding.
    rbxclient::AppStateRecord empty_mask;
    empty_mask.id = 8;
    empty_mask.member_count = 0;
    empty_mask.mask_words = {0};
    std::vector<uint8_t> orphan = {12, 0x00, 0xde, 0xad, 0xbe, 0xef,
                                   0xde, 0xad, 0xbe, 0xef, 0xde, 0xad,
                                   0xbe, 0xef};
    empty_mask.data = orphan;
    props.clear();
    CHECK_FALSE(rbxclient::parseAppStateBody(
        empty_mask.data.data() + 2, 12, s, empty_mask, &props, &err));
}

TEST_CASE("0xA1 attributes: paired schema/value bits decode exactly") {
    // Measured mask layout (live 2,743): a two-slot class has attribute id 0
    // at bits 2 (schema) and 3 (value). The schema record and value record
    // deliberately arrive separately; the per-entity schema store must keep
    // the type/name between them.
    rbxclient::JoinSchema s;
    s.classes.resize(1);
    s.classes[0].name = "Part";
    s.slots.resize(1);
    s.slots[0] = {0, 1}; // two named writer slots
    rbxclient::AppStateRecord schema_rec;
    schema_rec.id = 77;
    schema_rec.cls = 0;
    schema_rec.member_count = 4;
    schema_rec.mask_words = {uint64_t(1) << 2};
    const std::vector<uint8_t> schema_payload = {
        0x00, 0x02, 0x05, 0x00, 0x00, 0x00, 'S', 't', 'a', 't', 'e'};
    std::vector<uint8_t> schema_body = {
        uint8_t(schema_payload.size()), 0x00};
    schema_body.insert(schema_body.end(), schema_payload.begin(),
                       schema_payload.end());
    schema_rec.data = schema_body;

    rbxclient::A1AttributeSchemas schemas;
    std::vector<rbxclient::A1AttributeUpdate> attrs;
    std::vector<rbxclient::JoinProp> props;
    std::string err;
    bool partial = false;
    REQUIRE(rbxclient::parseAppStateBody(
        schema_rec.data.data() + 2, schema_payload.size(), s, schema_rec,
        &props, &err, nullptr, &partial, &attrs, &schemas));
    CHECK(props.empty());
    CHECK(attrs.empty());
    CHECK(!partial);
    REQUIRE(schemas.count(77));
    REQUIRE(schemas[77].size() == 1);
    CHECK(schemas[77][0].tag == 0x02);
    CHECK(schemas[77][0].name == "State");

    rbxclient::AppStateRecord value_rec = schema_rec;
    value_rec.mask_words = {uint64_t(1) << 3};
    const std::vector<uint8_t> value_payload = {0x04, 'N', 'o', 'n', 'e'};
    std::vector<uint8_t> value_body = {
        uint8_t(value_payload.size()), 0x00};
    value_body.insert(value_body.end(), value_payload.begin(),
                      value_payload.end());
    value_rec.data = value_body;
    attrs.clear();
    REQUIRE(rbxclient::parseAppStateBody(
        value_rec.data.data() + 2, value_payload.size(), s, value_rec, &props,
        &err, nullptr, &partial, &attrs, &schemas));
    CHECK(props.empty());
    CHECK(!partial);
    REQUIRE(attrs.size() == 1);
    CHECK(attrs[0].id == 0);
    CHECK(attrs[0].tag == 0x02);
    CHECK(attrs[0].name == "State");
    CHECK(attrs[0].value == value_payload);
}

TEST_CASE("0xA1 attributes: enum schema carries its owning Enum name") {
    // Native sub_643456E reads an extra u32-length Enum name before the
    // attribute name when the schema tag is 7. The live Humanoid bodies use
    // `00 07 11 00 00 00 HumanoidStateType 15 00 00 00 previousHumanoidState`;
    // consuming only one name is what used to derail the rest of the record.
    rbxclient::JoinSchema s;
    s.classes.resize(1);
    s.classes[0].name = "Part";
    s.classes[0].members = {{"CanCollide", 9, 0, false},
                            {"numExpectedDirectChildren", 10, 0, false}};
    s.slots.resize(1);
    s.slots[0] = {0, 1};
    s.member_by_gid = {{0, 0}, {0, 1}};
    rbxclient::AppStateRecord rec;
    rec.id = 81;
    rec.cls = 0;
    rec.member_count = 4;
    rec.mask_words = {uint64_t(1) << 2};
    const std::vector<uint8_t> payload = {
        0x00, 0x07, 0x11, 0x00, 0x00, 0x00, 'H', 'u', 'm', 'a', 'n', 'o',
        'i', 'd', 'S', 't', 'a', 't', 'e', 'T', 'y', 'p', 'e', 0x15, 0x00,
        0x00, 0x00, 'p', 'r', 'e', 'v', 'i', 'o', 'u', 's', 'H', 'u', 'm',
        'a', 'n', 'o', 'i', 'd', 'S', 't', 'a', 't', 'e'};
    std::vector<uint8_t> body = {uint8_t(payload.size()), 0x00};
    body.insert(body.end(), payload.begin(), payload.end());
    rec.data = body;
    rbxclient::A1AttributeSchemas schemas;
    std::vector<rbxclient::A1AttributeUpdate> attrs;
    std::vector<rbxclient::JoinProp> props;
    std::string err;
    bool partial = false;
    REQUIRE(rbxclient::parseAppStateBody(
        rec.data.data() + 2, payload.size(), s, rec, &props, &err, nullptr,
        &partial, &attrs, &schemas));
    CHECK(!partial);
    REQUIRE(schemas.count(81));
    REQUIRE(schemas[81].size() == 1);
    CHECK(schemas[81][0].tag == 7);
    CHECK(schemas[81][0].enum_name == "HumanoidStateType");
    CHECK(schemas[81][0].name == "previousHumanoidState");

    rec.mask_words = {uint64_t(1) << 3};
    const std::vector<uint8_t> value = {0x08};
    body.assign({uint8_t(value.size()), 0x00});
    body.insert(body.end(), value.begin(), value.end());
    rec.data = body;
    attrs.clear();
    REQUIRE(rbxclient::parseAppStateBody(
        rec.data.data() + 2, value.size(), s, rec, &props, &err, nullptr,
        &partial, &attrs, &schemas));
    CHECK(!partial);
    REQUIRE(attrs.size() == 1);
    CHECK(attrs[0].tag == 7);
    CHECK(attrs[0].enum_name == "HumanoidStateType");
    CHECK(attrs[0].value == value);
}

TEST_CASE("0xA1 attributes: value before schema is dropped by the body end-seek") {
    rbxclient::JoinSchema s;
    s.classes.resize(1);
    s.classes[0].name = "Part";
    s.slots.resize(1);
    s.slots[0] = {0, 1};
    rbxclient::AppStateRecord rec;
    rec.id = 78;
    rec.cls = 0;
    rec.member_count = 4;
    rec.mask_words = {uint64_t(1) << 3};
    const std::vector<uint8_t> payload = {0x01, 'x'};
    std::vector<uint8_t> body = {uint8_t(payload.size()), 0x00};
    body.insert(body.end(), payload.begin(), payload.end());
    rec.data = body;
    rbxclient::A1AttributeSchemas schemas;
    std::vector<rbxclient::A1AttributeUpdate> attrs;
    std::vector<rbxclient::JoinProp> props;
    std::vector<uint8_t> tail;
    std::string err;
    bool partial = false;
    REQUIRE(rbxclient::parseAppStateBody(
        rec.data.data() + 2, payload.size(), s, rec, &props, &err, &tail,
        &partial, &attrs, &schemas));
    // sub_64356A4 returns an empty variant when its schema lookup misses;
    // sub_6431154's LABEL_99 branch then seeks to v142 (the body end) and does
    // not read the value. The record is applied with the values decoded before
    // that point, and the unread remainder is end-skip, not a partial record.
    CHECK(!partial);
    CHECK(attrs.empty());
    CHECK(tail.empty());
    rbxclient::A1BodyAccounting accounting;
    REQUIRE(rbxclient::parseAppStateBody(
        rec.data.data() + 2, payload.size(), s, rec, &props, &err, nullptr,
        &partial, &attrs, &schemas, payload.size(), nullptr, nullptr, 0,
        &accounting));
    CHECK(accounting.end_seek_skipped == payload.size());
    CHECK(accounting.unaccounted == 0);
}

TEST_CASE("0xA1 attributes: schema after the value does not revive it") {
    rbxclient::JoinSchema s;
    s.classes.resize(1);
    s.classes[0].name = "Part";
    s.slots.resize(1);
    s.slots[0] = {0, 1};
    rbxclient::A1AttributeSchemas schemas;
    std::vector<rbxclient::A1AttributeUpdate> attrs;
    std::vector<rbxclient::JoinProp> props;
    std::string err;
    bool partial = false;

    rbxclient::AppStateRecord rec;
    rec.id = 79;
    rec.cls = 0;
    rec.member_count = 4;
    rec.mask_words = {uint64_t(1) << 3}; // value, schema not seen yet
    const std::vector<uint8_t> value = {0x02};
    std::vector<uint8_t> body = {uint8_t(value.size()), 0x00};
    body.insert(body.end(), value.begin(), value.end());
    rec.data = body;
    REQUIRE(rbxclient::parseAppStateBody(
        rec.data.data() + 2, value.size(), s, rec, &props, &err, nullptr,
        &partial, &attrs, &schemas));
    CHECK(!partial);
    CHECK(attrs.empty());
    CHECK(!schemas.count(79));

    // The schema arrives in a later record. Native retained the schema, not the
    // already-dropped value, so this record stores the schema without an update.
    rec.mask_words = {uint64_t(1) << 2};
    const std::vector<uint8_t> schema = {0x00, 0x02, 0x01, 0x00, 0x00, 0x00,
                                         'A'};
    body.assign({uint8_t(schema.size()), 0x00});
    body.insert(body.end(), schema.begin(), schema.end());
    rec.data = body;
    attrs.clear();
    REQUIRE(rbxclient::parseAppStateBody(
        rec.data.data() + 2, schema.size(), s, rec, &props, &err, nullptr,
        &partial, &attrs, &schemas));
    CHECK(!partial);
    CHECK(attrs.empty());
    REQUIRE(schemas.count(79));
    REQUIRE(schemas[79].size() == 1);
    CHECK(schemas[79][0].name == "A");

    // A later value now resolves through that schema.
    rec.mask_words = {uint64_t(1) << 3};
    const std::vector<uint8_t> later_value = {0x01, 'x'};
    body.assign({uint8_t(later_value.size()), 0x00});
    body.insert(body.end(), later_value.begin(), later_value.end());
    rec.data = body;
    REQUIRE(rbxclient::parseAppStateBody(
        rec.data.data() + 2, later_value.size(), s, rec, &props, &err, nullptr,
        &partial,
        &attrs, &schemas));
    CHECK(!partial);
    REQUIRE(attrs.size() == 1);
    CHECK(attrs[0].name == "A");
    CHECK(attrs[0].value == later_value);
}

TEST_CASE("0xA1 attributes: schema and value interleaved in one record") {
    rbxclient::JoinSchema s;
    s.classes.resize(1);
    s.classes[0].name = "Part";
    s.slots.resize(1);
    s.slots[0] = {0, 1};
    rbxclient::AppStateRecord rec;
    rec.id = 80;
    rec.cls = 0;
    rec.member_count = 4;
    rec.mask_words = {(uint64_t(1) << 2) | (uint64_t(1) << 3)};
    const std::vector<uint8_t> payload = {
        0x00, 0x02, 0x05, 0x00, 0x00, 0x00, 'G', 'r', 'e', 'e', 'n',
        0x01, 'x'};
    std::vector<uint8_t> body = {uint8_t(payload.size()), 0x00};
    body.insert(body.end(), payload.begin(), payload.end());
    rec.data = body;
    rbxclient::A1AttributeSchemas schemas;
    std::vector<rbxclient::A1AttributeUpdate> attrs;
    std::vector<rbxclient::JoinProp> props;
    std::string err;
    bool partial = false;
    REQUIRE(rbxclient::parseAppStateBody(
        rec.data.data() + 2, payload.size(), s, rec, &props, &err, nullptr,
        &partial, &attrs, &schemas));
    CHECK(!partial);
    REQUIRE(attrs.size() == 1);
    CHECK(attrs[0].name == "Green");
    CHECK(attrs[0].value == std::vector<uint8_t>{0x01, 'x'});
    REQUIRE(schemas.count(80));
    REQUIRE(schemas[80].size() == 1);
    CHECK(schemas[80][0].name == "Green");
}

TEST_CASE("0xA1 attributes: an unknown attribute name decodes by schema") {
    rbxclient::JoinSchema s;
    s.classes.resize(1);
    s.classes[0].name = "Part";
    s.slots.resize(1);
    s.slots[0] = {0, 1};
    rbxclient::A1AttributeSchemas schemas;
    rbxclient::AppStateRecord rec;
    rec.id = 82;
    rec.cls = 0;
    rec.member_count = 4;
    rec.mask_words = {(uint64_t(1) << 2) | (uint64_t(1) << 3)};
    const std::vector<uint8_t> payload = {
        0x00, 0x02, 0x0F, 0x00, 0x00, 0x00, 'N', 'o', 'S', 'u', 'c', 'h',
        'A', 't', 't', 'r', 'i', 'b', 'u', 't', 'e', 0x01, 'x'};
    std::vector<uint8_t> body = {uint8_t(payload.size()), 0x00};
    body.insert(body.end(), payload.begin(), payload.end());
    rec.data = body;
    std::vector<rbxclient::A1AttributeUpdate> attrs;
    std::vector<rbxclient::JoinProp> props;
    std::string err;
    bool partial = false;
    REQUIRE(rbxclient::parseAppStateBody(
        rec.data.data() + 2, payload.size(), s, rec, &props, &err, nullptr,
        &partial, &attrs, &schemas));
    CHECK(!partial);
    REQUIRE(attrs.size() == 1);
    CHECK(attrs[0].name == "NoSuchAttribute");
    CHECK(attrs[0].value == std::vector<uint8_t>{0x01, 'x'});
}

TEST_CASE("0xA1 framing: null-ref records before and after a marker are accepted") {
    // A null-ref head is four bytes shorter. The boundary walker must try it
    // at the exact end of the previous body and require its body to close; a
    // shifted normal read can otherwise look plausible.
    rbxclient::JoinSchema s;
    s.classes.resize(1);
    s.classes[0].name = "Part";
    s.classes[0].members.push_back({"State", 2, 0, false});
    s.slots.resize(1);
    s.slots[0] = {0};
    s.member_by_gid = {{0, 0}};

    std::vector<uint8_t> frame(17, 0);
    frame[0] = 0xA1;
    frame[5] = 3; // records
    const uint8_t first[] = {
        0x01, 0x07, 0x00, 0x00, 0x00, // ref lo=1, id=7
        0x00, 0x00,                   // class 0
        0x01, 0x00,                   // member_count 1
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mask bit 0
        0x02, 0x00,                   // body length 2
        0x01, 'A'                     // State = "A" (value only; tag is schema)
    };
    const uint8_t second[] = {
        0x00,                         // null ref (one byte)
        0x00, 0x00,                   // class 0
        0x01, 0x00,                   // member_count 1
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mask bit 0
        0x02, 0x00,                   // body length 2
        0x01, 'B'                     // State = "B"
    };
    frame.insert(frame.end(), std::begin(first), std::end(first));
    frame.insert(frame.end(), {0x01, 0x00, 0x00, 0x00}); // marker
    frame.insert(frame.end(), std::begin(second), std::end(second));
    frame.insert(frame.end(), std::begin(second), std::end(second));
    rbxclient::AppStateFrame out;
    std::string err;
    INFO("frame err: " << err);
    REQUIRE(rbxclient::parseAppStateFrame(frame.data(), frame.size(), s, &out,
                                          &err));
    REQUIRE(out.records.size() == 3);
    CHECK(out.records[0].lo == 1);
    CHECK(out.records[0].id == 7);
    CHECK(out.records[1].lo == 0);
    CHECK(out.records[1].id == 0xFFFFFFFFu);
    CHECK(out.records[2].lo == 0);
    CHECK(out.records[2].id == 0xFFFFFFFFu);
    std::vector<rbxclient::JoinProp> props;
    bool partial = false;
    REQUIRE(rbxclient::parseAppStateBody(
        out.records[1].data.data() + 2, 2, s, out.records[1], &props, &err,
        nullptr, &partial));
    REQUIRE(props.size() == 1);
    CHECK(props[0].name == "State");
    CHECK(props[0].value == std::vector<uint8_t>({0x01, 'B'}));
}
