// live_runtime.cpp — see live_runtime.h.
#include "engine/live_runtime.h"

#include "raknet/protocol.h"

#include "api/api.h"
#include "engine.h"
#include "instance/class_registry.h"
#include "instance/instance.h"
#include "instance/signal.h"
#include "solve/blob.h"
#include "solve/joindata.h"
#include "solve/replicate.h"
#include "solve/standardize.h"
#include "common/util.h"

#include "lua.h"
#include "luacode.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <utility>
#include <variant>
#include <vector>

namespace rbxclient {

namespace {

constexpr size_t kMaxReplFrame = 16u << 20;

// Collapse an A1 body error to a stable key: drop the byte offset so the same
// grammar gap counts as one entry across a whole session.
std::string a1ReasonKey(const std::string& err) {
    const size_t at = err.find(" at offset ");
    std::string k = at == std::string::npos ? err : err.substr(0, at);
    if (k.size() > 96)
        k.resize(96);
    return k;
}

struct PendingScript {
    uint32_t script_id = 0;
    int64_t anchor = -1;
    std::vector<uint8_t> payload;
    // Key bytes carried by the 0x78 bulk: the owning script's Source hash
    // (16 bytes; older servers sent its trailing 8-byte half). Used to
    // attribute a bulk to its script when the spawn item is not the
    // immediately preceding chain item.
    uint8_t key[16] = {0};
    uint8_t key_len = 0;
    bool have_key = false;
};

} // namespace

struct LiveRuntime::Impl {
    std::string job;
    LogFn log;

    std::vector<uint8_t> pending;
    std::vector<std::vector<uint8_t>> pre_schema_batches;
    // 0x83 chains that arrive before the snapshot world exists. The RCC sends
    // the local-player spawn chain (subtype 0x02) *before* the record batches
    // (RX ord 6 on 189707), so dropping it leaves the world without a Player.
    // Replay them once finalizeSnapshot has built the tree.
    std::vector<std::vector<uint8_t>> pre_apply_chains;

    JoinSchema schema;
    PropTagTable props;
    bool have_schema = false;

    std::vector<JoinRecord> snapshot_records;
    std::unique_ptr<ApplyReport> rep;
    bool snapshot_applied = false;
    // Some RCCs send the 0x84 marker *before* the snapshot batches, so the
    // marker itself cannot be the build trigger (place 142823291, 2026-10-29:
    // marker at ord 5, then 14 batches, no world). A quiet period after the
    // last batch is the signal that survives both orderings.
    std::chrono::steady_clock::time_point last_batch{};
    bool snapshot_batch_seen = false;

    std::FILE* chan1_dump = nullptr; // RBX_DUMP_CHAN1 (offline captures)
    rbxch::engine::Environment* env = nullptr;
    StreamStats sst;
    std::vector<PendingScript> scripts;
    // Authenticated account, from the join script. Players.LocalPlayer and
    // its PlayerGui exist from join in the native client; replication may
    // never send a Player record for us (live MM2: none in 40 s), so the
    // client creates the placeholder itself once this is known.
    long long local_user = 0;
    std::string local_user_name;
    std::string local_user_display;
    bool local_player_created = false;
    // --no-scripts: keep parsing + the instance world, skip Luau execution.
    bool run_scripts = true;
    // --gui: install PlayerGui/CurrentCamera for replicated scripts.
    bool gui = true;

    int messages = 0;
    int snapshot_batches = 0;
    int shared_batches = 0;
    int shared_strings_seen = 0;
    int netasset_batches = 0;
    int netasset_entries = 0;
    int unknown_batch_flags = 0;
    int unhandled_ids = 0;
    // Full inbound census: every app id and every 0x83 subtype the server sent,
    // so "did we parse everything" is a number instead of an impression.
    std::map<int, int> app_ids;
    std::map<int, int> sub_ids;
    std::map<int, int> unknown_ids;
    std::map<int, int> unknown_flags;
    std::map<int, int> stop_types; // chain item type where a walk pinned
    std::map<int, int> transport_ids; // RakNet-level ids also seen by the sink
    // Latest shared-string values by 16-byte content key (diagnostic; see the
    // flag=1 branch for why they cannot be substituted into schema.shared).
    std::map<std::string, std::vector<uint8_t>> dynamic_shared;
    int live_msgs = 0;
    int chain_probe = 0;
    int live_failures = 0;
    int live_stops = 0;
    int regions = 0;
    int a1_messages = 0;
    int a1_records = 0;
    int a1_applied_records = 0;
    int a1_props = 0;
    int a1_body_fail = 0;
    // Records applied from a decodable prefix: the members are exact, but the
    // record also carried bytes this grammar does not model (attribute list,
    // replication-only member slots).
    int a1_partial_records = 0;
    int a1_body_logged = 0;
    int a1_tail_logged = 0;
    std::set<int> a1_tail_seen;
    std::set<std::string> a1_body_seen;
    // Every A1 body-parse failure by normalized reason, so a long run gives a
    // complete census of the grammar gaps instead of the first 12 lines.
    std::map<std::string, int> a1_fail_reasons;
    int instance_pair_messages = 0;
    int instance_pair_records = 0;
    int chat_messages = 0;
    std::map<int, int> a1_first_class;
    int scripts_run = 0;
    int scripts_failed = 0;
    int scripts_skipped = 0;
    int modules_loaded = 0; // replicated ModuleScript chunks registered
    // RBX_EXEC_FILE: one diagnostic chunk after the world is live (used by
    // remote/movement parity runs; no effect when unset).
    bool debug_exec_done = false;
    std::set<const rbx::Instance*> known_players;
    std::map<const rbx::Instance*, const rbx::Instance*> known_characters;
    // Roster as replication last showed it (UserId -> Name). The RCC removes a
    // Player from everyone else's view without telling that client, so a diff
    // here is the only way the session can report a leave it actually saw.
    std::map<int64_t, std::string> roster;

    // Join watchdog. The RCC answers a placed join with 0x97 (schema + join
    // data) and then streams the snapshot batches. A session where 0x97 lands
    // but the record batches never follow is a failed join (measured on Adopt
    // Me: roughly one join in three), and it never heals on its own. The
    // caller uses these marks to drop the session and redo the handshake
    // instead of waiting for a world that will never arrive.
    bool join_data_seen = false;
    std::chrono::steady_clock::time_point join_data_at{};
    std::chrono::steady_clock::time_point last_progress{};
    bool world_progress_ = false;

    static long long ms_since(std::chrono::steady_clock::time_point t) {
        if (t.time_since_epoch().count() == 0) return -1;
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t)
            .count();
    }

    void noteProgress() {
        last_progress = std::chrono::steady_clock::now();
        world_progress_ = true;
    }

    bool failed = false;
    std::string error;
    std::map<int, int> types;

    Impl(std::string job_id, LogFn fn) : job(std::move(job_id)), log(std::move(fn)) {}

    ~Impl() {
        // The Engine environment owns every instance pushed into it. Drop our
        // own Refs first so engine::destroy's force-delete cannot be followed
        // by a second release from ApplyReport/root.
        scripts.clear();
        snapshot_records.clear();
        rep.reset();
        if (chan1_dump) {
            std::fclose(chan1_dump);
            chan1_dump = nullptr;
        }
        if (env) {
            rbxch::engine::destroy(env);
            env = nullptr;
        }
    }

    void fail(const std::string& why) {
        failed = true;
        error = why;
        log("ERROR " + why);
    }

    void feed(const uint8_t* data, size_t len) {
        if (failed || !len)
            return;
        pending.insert(pending.end(), data, data + len);
        size_t off = 0;
        while (off < pending.size()) {
            uint64_t ln = 0;
            const size_t k = rbx::compactVarintDecode(pending.data() + off,
                                                      pending.size() - off, &ln);
            if (k == 0)
                break; // incomplete compact varint: wait for more bytes
            if (ln == 0 || ln > kMaxReplFrame) {
                fail("frame length " + std::to_string(ln) +
                     " out of range at offset " + std::to_string(off));
                break;
            }
            if (off + k + ln > pending.size())
                break; // incomplete body: wait
            handle(pending.data() + off + k, size_t(ln));
            off += k + size_t(ln);
        }
        if (off)
            pending.erase(pending.begin(), pending.begin() + std::ptrdiff_t(off));
    }

    void feedMessage(const uint8_t* msg, size_t len) {
        if (failed || !len)
            return;
        std::vector<uint8_t> framed;
        framed.reserve(len + 8);
        rbx::compactVarint(framed, len);
        framed.insert(framed.end(), msg, msg + len);
        // RBX_DUMP_CHAN1=<file>: the whole inbound app stream, exactly the
        // length-framed form tools/repl_coverage.cpp and repl_toolkit read.
        // Offline parity work needs the chain bytes that produced a stop.
        if (const char* path = std::getenv("RBX_DUMP_CHAN1")) {
            if (!chan1_dump) {
                chan1_dump = std::fopen(path, "wb");
            }
            if (chan1_dump) {
                std::fwrite(framed.data(), 1, framed.size(), chan1_dump);
                std::fflush(chan1_dump);
            }
        }
        feed(framed.data(), framed.size());
    }

    void handle(const uint8_t* body, size_t len) {
        if (!len)
            return;
        ++messages;
        const uint8_t id = body[0];
        ++types[int(id)];
        ++app_ids[int(id)];
        // RBX_DUMP_APP=<dir>: first sighting of every app id, raw body. The
        // replication/state streams already have dedicated dumps; this is the
        // capture surface for ids that otherwise exist only in the census.
        if (const char* dir = std::getenv("RBX_DUMP_APP")) {
            static std::map<int, bool> dumped;
            if (id != 0x83 && id != 0x97 && id != 0xA1 && !dumped[int(id)]) {
                dumped[int(id)] = true;
                char path[1024];
                std::snprintf(path, sizeof path, "%s/app_%02x_%zu.bin", dir,
                              unsigned(id), len);
                if (FILE* f = std::fopen(path, "wb")) {
                    std::fwrite(body, 1, len, f);
                    std::fclose(f);
                }
            }
        }
        switch (id) {
        case 0x97:
            onJoinData(body, len);
            break;
        case 0x84:
            finalizeSnapshot();
            break;
        case 0x83:
            onChain(body, len);
            break;
        case 0x86:
            onInstancePairs(body, len);
            break;
        case 0x87:
        case 0x88:
        case 0x8B:
        case 0x8C:
            onChat(body, len);
            break;
        case 0x81:
            onPeer(body, len);
            break;
        case 0x8D:
            onRegion(body, len);
            break;
        case 0x93:
            onNetworkSettings(body, len);
            break;
        case 0xA1:
            onState(body, len);
            break;
        default:
            // The sink forwards every app payload, including the RakNet-level
            // ones this parser has no business decoding: 0x00/0x03 keepalive
            // pings and their echoes, 0x10 handshake accept, 0x51..0x56 clock
            // sync, 0x93 network settings, 0x9B challenge. Those are decoded
            // and answered by the transport (raknet.cpp), so count them apart
            // from a genuinely unknown id.
            if (id == 0x00 || id == 0x03 || id == 0x10 ||
                (id >= 0x51 && id <= 0x56) || id == 0x93 || id == 0x9B) {
                ++transport_ids[int(id)];
                break;
            }
            // Anything else is counted and named on first sight instead of
            // vanishing: a silent default is exactly how "we parse
            // everything" turns out to be false.
            ++unhandled_ids;
            if (++unknown_ids[int(id)] == 1 && log && unhandled_ids <= 8) {
                char b[96];
                snprintf(b, sizeof b, "unhandled app id 0x%02x (len=%zu, %zuB)",
                         unsigned(id), len, size_t(len));
                log(b);
            }
            break;
        }
    }

    // 0x86 server->client instance-pair stream. The wire structure is pinned
    // (version 0x01, N*11B records); the pair's semantics are still unassigned.
    // RBX_PAIR_CLASSES=1 is a one-run diagnostic that prints the DataModel
    // classes behind both refs so a later capture can name the relationship.
    void onInstancePairs(const uint8_t* body, size_t len) {
        std::vector<raknet::AppInstancePair> pairs;
        std::string err;
        if (!raknet::parseAppInstancePairs(body, len, &pairs, &err)) {
            ++live_failures;
            if (log) log("0x86 parse failed: " + err);
            return;
        }
        ++instance_pair_messages;
        instance_pair_records += int(pairs.size());
        if (!std::getenv("RBX_PAIR_CLASSES") || !log || !rep)
            return;
        static int shown = 0;
        for (const auto& p : pairs) {
            if (shown >= 16) break;
            auto describe = [&](uint32_t id) {
                auto it = rep->by_id.find(id);
                if (it == rep->by_id.end() || it->second < 0 ||
                    size_t(it->second) >= rep->instances.size())
                    return std::to_string(id) + ":?";
                const auto& inst = rep->instances[size_t(it->second)];
                if (!inst) return std::to_string(id) + ":null";
                return std::to_string(id) + ":" + inst->class_name() + ":" +
                       inst->name();
            };
            if (log)
                log("0x86 pair " + describe(p.ref_a) + " -> " +
                    describe(p.ref_b) + " state=" + std::to_string(p.state) +
                    " flag=" + std::to_string(p.flag));
            ++shown;
        }
    }

    void onChat(const uint8_t* body, size_t len) {
        raknet::AppChat chat;
        std::string err;
        if (!raknet::parseAppChat(body, len, &chat, &err)) {
            ++live_failures;
            if (log) log("0x87/88/8b/8c chat parse failed: " + err);
            return;
        }
        ++chat_messages;
        if (!log)
            return;
        char kind[8];
        std::snprintf(kind, sizeof kind, "0x%02x", unsigned(chat.id));
        std::string s = "chat " + std::string(kind);
        if (chat.has_sender) {
            s += " sender=" + std::to_string(chat.sender_low) + ":" +
                 std::to_string(chat.sender_high);
        }
        if (chat.has_target) {
            s += " target=" + std::to_string(chat.target_low) + ":" +
                 std::to_string(chat.target_high);
        }
        if (chat.has_flag0)
            s += " flag0=" + std::to_string(chat.flag0 ? 1 : 0);
        if (chat.has_flag1)
            s += " flag1=" + std::to_string(chat.flag1 ? 1 : 0);
        s += " text=" + chat.message;
        log(s);
    }

    // 0xA1 server->client client-state stream: see parseAppStateHeader.
    // The envelope and the first record's ref/class are decoded here; the
    // class-specific payload (Tool attributes, Humanoid/Animator state) is
    // still being mapped, so it is carried opaque rather than guessed.
    void onState(const uint8_t* body, size_t len) {
        AppStateFrame f;
        std::string err;
        if (!parseAppStateFrame(body, len, schema, &f, &err)) {
            ++live_failures;
            if (const char* dir = std::getenv("RBX_DUMP_A1FAIL")) {
                static int dumped = 0;
                char path[1024];
                std::snprintf(path, sizeof path, "%s/a1fail_%05d.bin", dir,
                              dumped++);
                if (std::FILE* fp = std::fopen(path, "wb")) {
                    std::fwrite(body, 1, len, fp);
                    std::fclose(fp);
                }
            }
            if (log) log("0xA1 state: " + err);
            return;
        }
        ++a1_messages;
        a1_records += int(f.records.size());
        if (!have_schema || f.records.empty())
            return;
        for (const AppStateRecord& r : f.records) {
            if (r.id && r.cls < schema.classes.size())
                ++a1_first_class[int(r.cls)];
            // RBX_NO_A1_APPLY=1 keeps the state stream parsed but unapplied; it
            // is the A/B switch for "does the state really move the world?".
            static const bool apply_a1 = !std::getenv("RBX_NO_A1_APPLY");
            if (!apply_a1 || !r.id || r.id == 0xFFFFFFFFu || !rep)
                continue;
            // Apply the record's member values exactly like a 0x03 property
            // update: the mask + schema member list is the wire's own state
            // grammar, so the world advances without a per-class decoder.
            const size_t body_len =
                r.data.size() >= 2
                    ? size_t(r.data[0]) | (size_t(r.data[1]) << 8)
                    : 0;
            if (r.data.size() < 2 || 2 + body_len > r.data.size()) {
                ++a1_body_fail;
                continue;
            }
            std::vector<JoinProp> props;
            std::string berr;
            std::vector<uint8_t> tail;
            bool partial = false;
            const bool tail_wanted = std::getenv("RBX_A1_TAIL") != nullptr;
            if (!parseAppStateBody(r.data.data() + 2, body_len, schema, r,
                                   &props, &berr,
                                   tail_wanted ? &tail : nullptr, &partial)) {
                ++a1_body_fail;
                ++a1_fail_reasons[a1ReasonKey(berr)];
                // RBX_DUMP_A1FAIL also keeps the first frame whose *record*
                // failed to decode: the framing parsed, so the frame is the
                // ground truth for a body grammar gap.
                if (const char* dir = std::getenv("RBX_DUMP_A1FAIL")) {
                    static int dumped = 0;
                    if (dumped++ == 0) {
                        char path[1024];
                        std::snprintf(path, sizeof path, "%s/a1body_%05zu.bin",
                                      dir, body_len);
                        if (std::FILE* fp = std::fopen(path, "wb")) {
                            std::fwrite(r.data.data(), 1, r.data.size(), fp);
                            std::fclose(fp);
                        }
                    }
                }
                if (tail_wanted && !tail.empty() && a1_tail_logged < 40) {
                    ++a1_tail_logged;
                    static const char* hx = "0123456789abcdef";
                    std::string h;
                    for (uint8_t b : tail) {
                        h.push_back(hx[b >> 4]);
                        h.push_back(hx[b & 15]);
                    }
                    const SchemaClass& tc = schema.classes[r.cls];
                    std::string sel;
                    for (size_t bi = 0; bi < r.member_count &&
                                       bi < tc.members.size(); ++bi) {
                        if (bi / 64 < r.mask_words.size() &&
                            (r.mask_words[bi / 64] >> (bi % 64)) & 1)
                            sel += (sel.empty() ? "" : " ") + tc.members[bi].name;
                    }
                    log("0xA1 tail " + tc.name + " " +
                        std::to_string(tail.size()) + "B body=" +
                        std::to_string(body_len) + " " + h + " sel=" + sel);
                }
                // One line per distinct shape: the census is what tells a
                // grammar gap from a class with a known tail (Tool).
                if (log && a1_body_logged < 12 &&
                    a1_body_seen.insert(berr).second) {
                    ++a1_body_logged;
                    log("0xA1 body: " + berr);
                }
                continue;
            }
            if (partial)
                ++a1_partial_records;
            ++a1_applied_records;
            for (const JoinProp& p : props) {
                if (applyStreamProp(*rep, schema, r.id, r.cls, p, sst))
                    ++a1_props;
            }
        }
        if (a1_messages <= 3 && log) {
            const AppStateRecord& r = f.records.front();
            char b[192];
            std::snprintf(b, sizeof b,
                          "0xA1 state tick=%u records=%u ack=%u flags=%u "
                          "first=%s mask=%016llx",
                          unsigned(f.header.tick),
                          unsigned(f.header.records), unsigned(f.header.ack),
                          unsigned(f.header.flags),
                          r.cls < schema.classes.size()
                              ? schema.classes[r.cls].name.c_str()
                              : "?",
                          static_cast<unsigned long long>(r.mask));
            log(b);
        }
    }

    void onNetworkSettings(const uint8_t* body, size_t len) {
        raknet::NetworkSettings s;
        std::string err;
        if (!raknet::parseNetworkSettings(body, len, &s, &err)) {
            ++live_failures;
            if (log) log("0x93 parse failed: " + err);
            return;
        }
        if (log)
            log("0x93 network settings: flags=" + std::to_string(s.flags) +
                " entries=" + std::to_string(s.settings.size()));
    }

    void onJoinData(const uint8_t* body, size_t len) {
        // The schema is the only join-data section this client builds from; a
        // 0x97 that fails to parse still means the RCC answered the join, so
        // the watchdog must see it.
        join_data_seen = true;
        join_data_at = std::chrono::steady_clock::now();
        std::vector<uint8_t> inner;
        std::string err;
        if (!parseJoinOuter(body, len, inner, &err)) {
            ++live_failures;
            log("0x97 outer failed: " + err);
            return;
        }
        JoinSchema fresh;
        if (!parseJoinSchema(inner.data(), inner.size(), fresh, &err)) {
            ++live_failures;
            log("0x97 schema failed: " + err);
            return;
        }
        schema = std::move(fresh);
        props = propTagsForSchema(schema);
        // RBX_DUMP_SCHEMA=<file>: write the live class/member table and the
        // prop-id -> (class, member, tag) map. Offline decoders need this to
        // resolve ids seen in streams that carry only the numeric ids (0xA1),
        // and it is the only copy of a session's schema otherwise.
        if (const char* path = std::getenv("RBX_DUMP_SCHEMA")) {
            if (FILE* f = std::fopen(path, "w")) {
                std::fprintf(f, "classes %zu props %zu\n",
                             schema.classes.size(), props.tag.size());
                for (size_t ci = 0; ci < schema.classes.size(); ++ci) {
                    const auto& c = schema.classes[ci];
                    std::fprintf(f, "class %zu %s members %zu\n", ci,
                                 c.name.c_str(), c.members.size());
                    for (size_t mi = 0; mi < c.members.size(); ++mi)
                        std::fprintf(f, "  m %zu %s tag %u\n", mi,
                                     c.members[mi].name.c_str(),
                                     unsigned(c.members[mi].tag));
                }
                for (const auto& kv : props.tag) {
                    const auto oc = props.owner_class.find(kv.first);
                    const auto mn = props.member_name.find(kv.first);
                    std::fprintf(f, "prop %u tag %u class %s member %s\n",
                                 unsigned(kv.first), unsigned(kv.second),
                                 oc != props.owner_class.end() &&
                                         oc->second < schema.classes.size()
                                     ? schema.classes[oc->second].name.c_str()
                                     : "?",
                                 mn != props.member_name.end()
                                     ? mn->second.c_str()
                                     : "?");
                }
                std::fclose(f);
                log("schema dumped to " + std::string(path));
            }
        }
        have_schema = true;
        log("join schema classes=" + std::to_string(schema.classes.size()) +
            " members=" + std::to_string(props.tag.size()) +
            " shared=" + std::to_string(schema.shared.size()));
        if (!pre_schema_batches.empty()) {
            std::vector<std::vector<uint8_t>> queued;
            queued.swap(pre_schema_batches);
            for (const auto& b : queued)
                onChain(b.data(), b.size());
        }
    }

    void onPeer(const uint8_t* body, size_t len) {
        PeerInit p;
        std::string err;
        if (!parsePeerInit(body, len, p, &err)) {
            log("0x81 parse failed: " + err);
            return;
        }
        log("peer assigned=" + std::to_string(p.peer));
    }

    void onRegion(const uint8_t* body, size_t len) {
        RegionChunk r;
        std::string err;
        if (!parseRegionChunk(body, len, r, &err)) {
            log("0x8D parse failed: " + err);
            return;
        }
        ++regions;
        if (regions <= 5 || regions % 100 == 0)
            log("terrain chunk #" + std::to_string(regions) + " " +
                std::to_string(r.inner.size()) + "B");
    }

    void onChain(const uint8_t* body, size_t len) {
        if (len < 2)
            return;
        ++sub_ids[int(body[1])];
        if (!have_schema) {
            if (pre_schema_batches.size() < 4096)
                pre_schema_batches.emplace_back(body, body + len);
            return;
        }
        if (body[1] == 0x16) {
            onBatch(body, len);
        } else {
            // Subtype census: a snapshot that is not 0x16 batches (place
            // 189707 sends large plain chains) currently has no path into the
            // world builder, so name what arrived instead of dropping it.
            if (chain_probe < 5 && log) {
                ++chain_probe;
                char b[64];
                snprintf(b, sizeof b, "0x83 chain sub=0x%02x len=%zu",
                         body[1], len);
                log(b);
            }
            onLiveChain(body, len);
        }
    }

    void onBatch(const uint8_t* body, size_t len) {
        std::vector<uint8_t> inner;
        std::string err;
        if (!parseDeltaBatch(body, len, inner, &err)) {
            ++live_failures;
            log("0x16 batch failed: " + err);
            return;
        }
        const uint8_t flags = len > 2 ? body[2] : 0;
        if (flags == 1) {
            std::vector<SharedString> ss;
            if (!parseSharedStrings(inner.data(), inner.size(), ss, &err)) {
                ++live_failures;
                log("shared-string batch failed: " + err);
                return;
            }
            ++shared_batches;
            shared_strings_seen += int(ss.size());
            // Shared strings are the snapshot's first section; a stream that
            // stops here never reaches the record batches, so this counts as
            // join progress for the stall watchdog.
            noteProgress();
            // The 0x97 schema fixes the index order these entries belong to
            // (tag 49 reads schema.shared[idx]); entries here carry a 16-byte
            // content key, not an index, so they can be recorded but not
            // substituted into that table without the key order. Keep the
            // newest values keyed by their hash for diagnostics only.
            for (const SharedString& s : ss) {
                if (dynamic_shared.size() < 4096)
                    dynamic_shared.emplace(std::string(s.key, s.key + 16),
                                           s.value);
            }
            return;
        }
        if (flags == 5) {
            // NetAsset batch (sub_348C62C): id + payload per entry, no count
            // and no terminator. The factory creates by schema, so the payload
            // is opaque here; parse it anyway so the batch is accounted for
            // and a malformed one fails loudly instead of being assumed empty.
            std::vector<NetAssetEntry> na;
            if (!parseNetAssetBatch(inner.data(), inner.size(), na, &err)) {
                ++live_failures;
                log("netasset batch failed: " + err);
                return;
            }
            ++netasset_batches;
            netasset_entries += int(na.size());
            noteProgress();
            if (netasset_batches <= 3) {
                size_t bytes = 0;
                for (const NetAssetEntry& e : na) bytes += e.payload.size();
                log("netasset batch #" + std::to_string(netasset_batches) +
                    " entries=" + std::to_string(na.size()) + " payload=" +
                    std::to_string(bytes) + "B");
            }
            return;
        }
        // The kind byte is not a closed set: live sessions have been seen with
        // 0x02/0x03/0x04 as well, and those inners parse cleanly as ordinary
        // records (a records batch was never gated on it before). Try the
        // record grammar first for anything that is not shared-strings/NetAsset
        // and only report a genuinely unparseable kind.
        if (flags != 0 && flags != 2 && flags != 3 && flags != 4) {
            ++unknown_batch_flags;
            if (++unknown_flags[int(flags)] == 1 && log) {
                char b[96];
                snprintf(b, sizeof b,
                         "unknown 0x83/16 batch kind 0x%02x (%zuB inner)",
                         unsigned(flags), inner.size());
                log(b);
            }
        }
        if (inner.empty())
            return;
        std::vector<JoinRecord> recs;
        if (!parseJoinRecords(inner.data(), inner.size(), schema, recs, &err)) {
            ++live_failures;
            log("0x16 records failed: " + err);
            // Decompressed batch kept for offline replay: a record grammar gap
            // is only fixable from the exact bytes (RBX_DUMP_FAIL dir).
            if (const char* dir = std::getenv("RBX_DUMP_FAIL")) {
                char path[1024];
                std::snprintf(path, sizeof path, "%s/batch_%d_%zu.bin", dir,
                              live_failures, inner.size());
                if (FILE* f = std::fopen(path, "wb")) {
                    std::fwrite(inner.data(), 1, inner.size(), f);
                    std::fclose(f);
                }
            }
            return;
        }
        if (!snapshot_applied) {
            ++snapshot_batches;
            snapshot_batch_seen = true;
            last_batch = std::chrono::steady_clock::now();
            noteProgress();
            snapshot_records.insert(snapshot_records.end(),
                                    std::make_move_iterator(recs.begin()),
                                    std::make_move_iterator(recs.end()));
            if (snapshot_batches <= 3)
                log("snapshot batch #" + std::to_string(snapshot_batches) +
                    " records=" + std::to_string(recs.size()) +
                    " total=" + std::to_string(snapshot_records.size()));
            return;
        }
        for (const JoinRecord& r : recs) {
            applyStreamRecord(*rep, schema, r.id, r.cls, r.props, r.parent, sst);
            ++sst.entries_applied;
        }
    }

    void onLiveChain(const uint8_t* body, size_t len) {
        if (!snapshot_applied) {
            // A live item before the 0x84 marker is the practical end of the
            // snapshot (some servers omit the marker on rejoin). Build now and
            // process this chain against the resulting world. If there is no
            // snapshot yet, keep the chain: the Player/Character spawn chain
            // arrives before the record batches.
            finalizeSnapshot();
            if (!snapshot_applied) {
                if (pre_apply_chains.size() < 256)
                    pre_apply_chains.emplace_back(body, body + len);
                return;
            }
        }
        Walk83 w;
        std::string err;
        if (!walk83ItemsEx(body, len, schema, &props, w, &err)) {
            ++live_failures;
            if (live_failures <= 8)
                log("0x83 walk failed: " + err);
            // RBX_DUMP_FAIL=<dir>: keep the exact body that broke the walk so
            // the grammar gap can be replayed offline (same contract as
            // RBX_DUMP_STOP).
            if (const char* dir = std::getenv("RBX_DUMP_FAIL")) {
                char path[1024];
                std::snprintf(path, sizeof path, "%s/fail_%d_%zu.bin", dir,
                              live_failures, len);
                if (FILE* f = std::fopen(path, "wb")) {
                    std::fwrite(body, 1, len, f);
                    std::fclose(f);
                }
            }
            return;
        }
        if (!w.complete) {
            ++live_stops;
            ++stop_types[int(w.stop_type)];
            if (live_stops <= 4 && log) {
                char b[96];
                snprintf(b, sizeof b,
                         "0x83 walk stopped at type 0x%02x (off %zu of %zu)",
                         unsigned(w.stop_type), w.stop_off, len);
                // Bytes from the pin onward: the item type plus enough body to
                // reverse the missing grammar without a new live capture.
                std::string tail;
                static const char* kHex = "0123456789abcdef";
                const size_t from = w.stop_off < len ? w.stop_off : len;
                for (size_t k = from; k < len && k < from + 32; ++k) {
                    tail.push_back(kHex[body[k] >> 4]);
                    tail.push_back(kHex[body[k] & 15]);
                }
                std::string spans;
                const size_t first = w.spans.size() > 8 ? w.spans.size() - 8 : 0;
                for (size_t k = first; k < w.spans.size(); ++k) {
                    char sb[48];
                    snprintf(sb, sizeof sb, "%s%zu:0x%02x-%zu",
                             spans.empty() ? "" : ",", w.spans[k].off,
                             unsigned(w.spans[k].type), w.spans[k].end);
                    spans += sb;
                }
                log(std::string(b) + " bytes=" + tail + " items=" + spans);
            }
            if (const char* dir = std::getenv("RBX_DUMP_STOP")) {
                char path[1024];
                snprintf(path, sizeof path, "%s/stop_%d_%zu.bin", dir,
                         live_stops, len);
                if (FILE* f = std::fopen(path, "wb")) {
                    std::fwrite(body, 1, len, f);
                    std::fclose(f);
                }
            }
        }
        ++live_msgs;
        noteProgress();
        uint32_t last_spawn = 0;
        bool have_last_spawn = false;
        // Scripts wait for the whole chain to be applied. The native starts a
        // script after the replication step that delivered it, so siblings
        // spawned later in the same message exist by the first line (live MM2:
        // a streamed Model runs `script.Parent.EyeLeft` on spawn, which failed
        // when the script ran mid-chain before EyeLeft was applied).
        std::vector<PendingScript> chain_scripts;
        for (const ChainItem& it : w.items) {
            switch (it.type) {
            case 0x02:
                applyStreamRecord(*rep, schema, it.spawn.id, it.spawn.cls,
                                  mergeProps(it.spawn.props_a, it.spawn.props_b),
                                  it.spawn.parent, sst);
                last_spawn = it.spawn.id;
                have_last_spawn = it.spawn.lo != 0;
                break;
            case 0x03:
                applyTriple(it.prop03);
                break;
            case 0x07:
                logStreamEvent(schema, it.event, sst);
                if (env)
                    fireStreamEvent(*rep, schema, it.event, rbxch::engine::state(*env), sst);
                break;
            case 0x0B:
                applyBatch(it.head0b.entries);
                break;
            case 0x0D:
                for (const JoinRecord& rec : it.delta0d.entries.records) {
                    applyStreamRecord(*rep, schema, rec.id, rec.cls, rec.props,
                                      rec.parent, sst);
                    ++sst.entries_applied;
                    last_spawn = rec.id;
                    have_last_spawn = rec.lo != 0;
                }
                break;
            case 0x25:
                applyBatch(it.v25.batch.entries);
                break;
            case 0x78:
                ++sst.bulks;
                if (!it.bulk.payload.empty()) {
                    PendingScript ps;
                    ps.script_id = last_spawn;
                    ps.anchor = it.bulk.anchor;
                    ps.payload = it.bulk.payload;
                    ps.key_len = it.bulk.key_len;
                    for (int k = 0; k < ps.key_len; ++k)
                        ps.key[k] = it.bulk.key[k];
                    ps.have_key = true;
                    if (snapshot_applied)
                        chain_scripts.push_back(std::move(ps));
                    else
                        scripts.push_back(std::move(ps));
                }
                break;
            default:
                break;
            }
        }
        for (const PendingScript& ps : chain_scripts)
            runScript(ps);
        if (snapshot_applied)
            syncPlayers();
    }

    static std::vector<JoinProp> mergeProps(const std::vector<JoinProp>& a,
                                            const std::vector<JoinProp>& b) {
        std::vector<JoinProp> out = a;
        out.insert(out.end(), b.begin(), b.end());
        return out;
    }

    void applyTriple(const PropTriple& p) {
        ++sst.prop_updates;
        if (!rep || !rep->root || p.lo == 0)
            return;
        // Prop id == the schema's member-count bound is the native's
        // out-of-band Parent property, ref-typed (5 bytes). It is how a
        // streamed child is re-parented; treating it as a member dropped it
        // (and over-read one byte into the child's spawn item).
        if (p.prop == props.bound) {
            const std::vector<uint8_t>& v = p.value;
            uint64_t ref_lo = 0;
            int shift = 0;
            size_t i = 0;
            bool done = false;
            for (; i < v.size() && shift < 64; ++i, shift += 7) {
                ref_lo |= uint64_t(v[i] & 0x7f) << shift;
                if (!(v[i] & 0x80)) {
                    ++i;
                    done = true;
                    break;
                }
            }
            int64_t parent = -1;
            if (!done || (ref_lo != 0 && v.size() - i < 4)) {
                ++sst.prop_missing;
                return;
            }
            if (ref_lo != 0)
                parent = int64_t(uint32_t(v[i]) |
                                 (uint32_t(v[i + 1]) << 8) |
                                 (uint32_t(v[i + 2]) << 16) |
                                 (uint32_t(v[i + 3]) << 24));
            if (!applyParentUpdate(*rep, p.id, parent, sst))
                ++sst.prop_missing;
            return;
        }
        const auto name = props.member_name.find(p.prop);
        const auto cls = props.owner_class.find(p.prop);
        if (name == props.member_name.end() || cls == props.owner_class.end()) {
            ++sst.prop_missing;
            if (log && std::getenv("RBX_LOG_PROPMISS"))
                log("prop miss: id=" + std::to_string(p.id) + " prop=" +
                    std::to_string(p.prop) + " (no schema member)");
            return;
        }
        JoinProp jp;
        jp.name = name->second;
        jp.tag = p.tag;
        jp.value = p.value;
        if (!applyStreamProp(*rep, schema, p.id, cls->second, jp, sst)) {
            ++sst.prop_missing;
            if (log && std::getenv("RBX_LOG_PROPMISS"))
                log("prop miss: id=" + std::to_string(p.id) + " prop=" +
                    std::to_string(p.prop) + " class=" +
                    std::to_string(cls->second) + " " + name->second +
                    " (apply rejected)");
        }
    }

    void applyBatch(const std::vector<JoinBatchEntry>& entries) {
        for (const JoinBatchEntry& e : entries) {
            if (e.ref1.id < 0)
                continue;
            const int64_t parent = e.ref2.lo == 0 ? -1 : e.ref2.id;
            applyStreamRecord(*rep, schema, uint32_t(e.ref1.id), e.cls, e.props,
                              parent, sst);
            ++sst.entries_applied;
        }
    }

    void finalizeSnapshot() {
        if (snapshot_applied || !have_schema || snapshot_records.empty())
            return;
        rbx::register_builtin_classes();
        std::string err;
        ApplyReport built = applyJoinSnapshot(schema, snapshot_records, &err);
        if (!built.root) {
            fail("snapshot apply failed: " + err);
            return;
        }
        snapshot_records.clear();
        rep = std::make_unique<ApplyReport>(std::move(built));
        // The snapshot can reference client-created services (Players,
        // Workspace, …) that are never records; until this ran, those records
        // stayed detached and the DataModel root had no children at all —
        // no Players service, no PlayerAdded, no avatar link (measured
        // 2026-10-30). Later 0x83 parents still re-parent their children.
        flushDeferredToRoot(*rep);

        rbxch::Options opt;
        opt.job = job;
        opt.profile = rbxch::Profile::Engine;
        opt.gui = gui;
        env = rbxch::engine::create(opt);
        if (!env) {
            fail("engine::create failed");
            return;
        }
        rbxch::engine::attach(*env, rep->root.get());
        snapshot_applied = true;
        ensureLocalPlayer();
        log("world spawned=" + std::to_string(rep->stats.spawned) +
            " grafted=" + std::to_string(rep->stats.grafted_to_root) +
            " deferred=" + std::to_string(rep->stats.deferred_to_parent) +
            " instances=" + std::to_string(rep->instances.size()) +
            " engine=on");
        if (!scripts.empty()) {
            std::vector<PendingScript> queued;
            queued.swap(scripts);
            for (const PendingScript& ps : queued)
                runScript(ps);
        }
        syncPlayers();
        if (!pre_apply_chains.empty()) {
            std::vector<std::vector<uint8_t>> queued;
            queued.swap(pre_apply_chains);
            for (const auto& c : queued) onLiveChain(c.data(), c.size());
        }
    }

    // The native client owns Players.LocalPlayer: it exists from join, with
    // its PlayerGui, whether or not the server ever replicates a Player
    // record for us. Create that placeholder (once) so LocalScripts that do
    // game:GetService("Players").LocalPlayer:WaitForChild("PlayerGui") at
    // line 1 do not index nil. A replicated Player for the same UserId fills
    // this instance in (applyRecordPass1 reuses it) instead of spawning a
    // second Player.
    void ensureLocalPlayer() {
        if (!env || !rep || !rep->root || local_user <= 0)
            return;
        rbx::Instance* players = rep->root->find_first_child_of_class("Players");
        if (!players)
            return;
        for (const auto& child : players->children()) {
            if (!child->is_a("Player"))
                continue;
            rbx::Variant uid;
            if (!child->get_prop("UserId", uid))
                continue;
            const auto* id = std::get_if<int64_t>(&uid);
            if (id && *id == local_user)
                return; // already there (created here or replicated)
        }
        const rbx::ClassInfo* cls = rbx::find_class("Player");
        if (!cls)
            return;
        rbx::Ref<rbx::Instance> player = rbx::instantiate(cls);
        const std::string name =
            local_user_name.empty() ? "Player" : local_user_name;
        player->set_name(name);
        std::string err;
        player->set_prop("UserId", rbx::Variant(local_user), &err);
        player->set_prop(
            "DisplayName",
            rbx::Variant(local_user_display.empty() ? name : local_user_display),
            &err);
        if (!player->reparent(players, &err)) {
            log("local player: " + err);
            return;
        }
        local_player_created = true;
        // attach_gui is idempotent: give the new Player its PlayerGui exactly
        // like the native client's GUI tree.
        if (gui)
            rbxch::api::attach_gui(rbxch::engine::state(*env), rep->root.get());
        log("local player created: " + name + ":" +
            std::to_string(local_user));
    }

    // Fire Players.PlayerAdded / Player.CharacterAdded for Players that
    // replication created or updated since the last chain. The Engine's
    // LocalPlayer/NumPlayers properties are live views (see instance.cpp),
    // so no cached identity needs refreshing here.
    void syncPlayers() {
        if (!env || !rep || !rep->root)
            return;
        ensureLocalPlayer();
        rbx::Instance* players = rep->root->find_first_child_of_class("Players");
        if (!players)
            return;
        lua_State* L = rbxch::engine::state(*env);
        std::map<int64_t, std::string> now;
        for (const auto& child : players->children()) {
            rbx::Instance* player = child.get();
            if (!player->is_a("Player"))
                continue;
            int64_t uid = 0;
            rbx::Variant idv;
            if (player->get_prop("UserId", idv))
                if (const auto* n = std::get_if<int64_t>(&idv)) uid = *n;
            now[uid] = player->name();
            linkCharacter(player);
            if (known_players.insert(player).second &&
                rbx::find_event(players->class_info(), "PlayerAdded"))
                players->event_signal("PlayerAdded")
                    ->fire(L, {rbx::Variant(player)});
            rbx::Variant ch;
            if (!player->get_prop("Character", ch))
                continue;
            const auto* character = std::get_if<rbx::Instance*>(&ch);
            if (!character || !*character)
                continue;
            auto it = known_characters.find(player);
            if (it != known_characters.end() && it->second == *character)
                continue;
            known_characters[player] = *character;
            log("player character: " + player->name() +
                " (" + (*character)->name() + ")");
            if (rbx::find_event(player->class_info(), "CharacterAdded"))
                player->event_signal("CharacterAdded")
                    ->fire(L, {rbx::Variant(*character)});
        }
        for (const auto& p : now)
            if (roster.find(p.first) == roster.end())
                log("player joined: " + p.second +
                    (p.first ? ":" + std::to_string(p.first) : ""));
        for (const auto& p : roster)
            if (now.find(p.first) == now.end())
                log("player left: " + p.second +
                    (p.first ? ":" + std::to_string(p.first) : "") +
                    " (Player removed from the replicated world)");
        roster.swap(now);
    }

    // The join snapshot replicates the avatar Model ("<PlayerName>") and the
    // Player, but the Player.Character link is not a replicated property on
    // this path (the official client sets it in native Player code). Without
    // the link, Players.LocalPlayer.Character is nil and no CharacterAdded
    // ever fires even though the avatar is sitting in Workspace. Resolve it
    // the way the native client does: a direct Workspace child named after
    // the player that is a Model containing a Humanoid.
    void linkCharacter(rbx::Instance* player) {
        if (!player || !rep || !rep->root)
            return;
        rbx::Variant name;
        if (!player->get_prop("Name", name))
            return;
        const auto* pname = std::get_if<std::string>(&name);
        if (!pname || pname->empty())
            return;
        rbx::Instance* workspace =
            rep->root->find_first_child_of_class("Workspace");
        if (!workspace)
            return;

        // A live avatar arrives as TWO Workspace models with the same name:
        // the Character wrapper (Humanoid, Animate, Accessories, ForceField)
        // and the body model that carries the actual BaseParts. The native
        // Player code links Character to the part-bearing model; picking the
        // first name match links the wrapper and leaves the avatar invisible
        // (Humanoid.Health exists, but there is nothing to draw or move).
        auto count_parts = [](rbx::Instance* model) {
            int n = 0;
            std::vector<rbx::Instance*> stack{model};
            while (!stack.empty()) {
                rbx::Instance* cur = stack.back();
                stack.pop_back();
                if (cur != model) {
                    const rbx::PropInfo* cf =
                        rbx::find_property(cur->class_info(), "CFrame");
                    const rbx::PropInfo* sz =
                        rbx::find_property(cur->class_info(), "Size");
                    if (cf && sz && cf->type == rbx::PropType::CFrame &&
                        sz->type == rbx::PropType::Vector3)
                        ++n;
                }
                for (const auto& c : cur->children())
                    stack.push_back(c.get());
            }
            return n;
        };

        rbx::Instance* best = nullptr;
        int best_parts = -1;
        bool best_humanoid = false;
        auto consider = [&](rbx::Instance* m) {
            if (!m || !m->is_a("Model") || m->name() != *pname)
                return;
            const int parts = count_parts(m);
            const bool humanoid = m->find_first_child_of_class("Humanoid") != nullptr;
            if (parts > best_parts ||
                (parts == best_parts && humanoid && !best_humanoid)) {
                best = m;
                best_parts = parts;
                best_humanoid = humanoid;
            }
        };
        // Body models can be direct Workspace children or nested in the
        // Character wrapper; consider both, deepest score wins.
        std::vector<rbx::Instance*> stack{workspace};
        while (!stack.empty()) {
            rbx::Instance* cur = stack.back();
            stack.pop_back();
            consider(cur);
            for (const auto& c : cur->children())
                stack.push_back(c.get());
        }
        if (!best || best_parts <= 0)
            return;

        rbx::Variant existing;
        rbx::Instance* current = nullptr;
        if (player->get_prop("Character", existing)) {
            const auto* p = std::get_if<rbx::Instance*>(&existing);
            if (p)
                current = *p;
        }
        // Keep an already-correct link; replace a wrapper-only link (0 parts)
        // with the body model the native client expects.
        if (current == best)
            return;
        if (current && count_parts(current) > 0)
            return;
        std::string err;
        if (player->set_prop("Character", rbx::Variant(best), &err))
            log("linked character '" + *pname + "' (" +
                std::to_string(best_parts) + " parts)");
        else
            log("character link failed for '" + *pname + "': " + err);
    }

    // A replicated chunk is only executable on the client when the native
    // client would run it: LocalScript (anywhere), or a Script explicitly set
    // to RunContext=Client. ModuleScript is require()d, never auto-run, and a
    // Legacy/Server Script belongs to the server. Running those locally is
    // both wrong and noisy: every server chunk would fail at its first
    // server-only API.
    static bool client_runs(rbx::Instance* inst) {
        if (!inst)
            return false;
        if (inst->is_a("ModuleScript"))
            return false;
        if (inst->is_a("LocalScript") || inst->is_a("CoreScript"))
            return true;
        if (inst->is_a("Script")) {
            rbx::Variant v;
            if (inst->get_prop("RunContext", v))
                if (const auto* e = std::get_if<rbx::EnumItem>(&v))
                    return e->value == 2; // Enum.RunContext.Client
        }
        return false;
    }

    // A chunk can run only on a script the native client actually has in the
    // DataModel: Roblox never executes a detached LocalScript, and a script
    // outside the tree has no `script` parent to resolve children from.
    // A snapshot without its parent record can leave a duplicate behind
    // (Adopt Me has two `Load` LocalScripts: a detached one and the parented
    // one that owns the AwaitPersistentModels modules). When exactly one
    // same-class, same-name instance is in the tree, that is the native's
    // owner, so attribute the chunk there instead of on the orphan.
    bool in_tree(rbx::Instance* inst) const {
        for (rbx::Instance* p = inst; p; p = p->parent())
            if (p == rep->root.get())
                return true;
        return false;
    }

    rbx::Instance* resolve_script_owner(rbx::Instance* script) {
        if (!rep || !rep->root || !script || in_tree(script))
            return script;
        rbx::Instance* match = nullptr;
        for (const auto& inst : rep->instances) {
            if (!inst || inst.get() == script)
                continue;
            if (inst->class_name() != script->class_name() ||
                inst->name() != script->name())
                continue;
            if (!in_tree(inst.get()))
                continue;
            if (match)
                return script; // ambiguous: keep the wire owner
            match = inst.get();
        }
        return match ? match : script;
    }

    void runScript(const PendingScript& ps) {
        if (!run_scripts) {
            ++scripts_skipped;
            return;
        }
        if (!env || !rep || !rep->root) {
            scripts.push_back(ps);
            return;
        }
        // Attribute the bulk through its content key first: the RCC sends
        // script bodies for scripts spawned much earlier in the stream (big
        // games), so "the last spawn item" is not the owner. Keep the
        // adjacency result when no key matches (small places still work).
        uint32_t owner = ps.script_id;
        if (ps.have_key && (!rep->source_hash.empty() || !rep->source_half.empty())) {
            bool saw_last = false;
            uint32_t first_client = 0;
            int matches = 0;
            auto consider = [&](uint32_t id) {
                ++matches;
                if (id == ps.script_id)
                    saw_last = true;
                const auto iid = rep->by_id.find(id);
                if (!first_client && iid != rep->by_id.end() &&
                    iid->second >= 0 &&
                    client_runs(rep->instances[size_t(iid->second)].get()))
                    first_client = id;
            };
            // The current wire key is the full 16-byte Source hash, so the
            // exact map is the strongest attribution. Only when it does not
            // resolve (older 8-byte-half capture, or a spawn whose Source
            // hash was not recorded) fall back to the half index.
            if (ps.key_len == 16) {
                const auto full = rep->source_hash.find(
                    std::string(reinterpret_cast<const char*>(ps.key), 16));
                if (full != rep->source_hash.end())
                    consider(full->second);
            }
            if (matches == 0) {
                const std::string key(reinterpret_cast<const char*>(ps.key), 8);
                const auto range = rep->source_half.equal_range(key);
                for (auto k = range.first; k != range.second; ++k)
                    consider(k->second);
            }
            if (saw_last)
                owner = ps.script_id;
            else if (first_client)
                owner = first_client;
            if (std::getenv("RBX_LOG_BULKS")) {
                std::string hex;
                static const char* kHex = "0123456789abcdef";
                for (int k = 0; k < ps.key_len; ++k) {
                    hex.push_back(kHex[ps.key[k] >> 4]);
                    hex.push_back(kHex[ps.key[k] & 15]);
                }
                log("bulk key=" + hex + " keylen=" +
                    std::to_string(ps.key_len) + " matches=" +
                    std::to_string(matches) + " last=" +
                    std::to_string(ps.script_id) + " owner=" +
                    std::to_string(owner));
            }
        }
        const auto it = rep->by_id.find(owner);
        if (it == rep->by_id.end() || it->second < 0) {
            ++scripts_skipped;
            return;
        }
        rbx::Instance* script = rep->instances[size_t(it->second)].get();
        if (rbx::Instance* owner_inst = resolve_script_owner(script)) {
            if (owner_inst != script && std::getenv("RBX_LOG_SCRIPTS"))
                log("script id=" + std::to_string(owner) + " '" + script->name() +
                    "' detached; attributing the chunk to the in-tree copy");
            script = owner_inst;
        }
        std::vector<uint8_t> code = ps.payload;
        // Production script bulks are RSB1 blobs, not raw chunks: they use
        // the same xor/xxhash/zstd container as the 0x9B challenge payload.
        // Decode first (raw standard/wire chunks fail the checksum and fall
        // through unchanged), then standardize the wire opcodes.
        std::vector<uint8_t> wire;
        std::string blob_err;
        if (rbxclient::decodeBlob(code.data(), code.size(), wire, &blob_err))
            code = std::move(wire);
        // Production wire programs use the wire opcode numbering. Rewrite
        // them before luau_load, exactly like the challenge path. Unknown
        // headers are passed through so the loader can report them honestly.
        if (code.size() > 1 && code[0] >= 3 && code[0] <= 14) {
            rbxclient::Stats st;
            std::string serr;
            std::vector<uint8_t> standardized = code;
            if (rbxclient::standardize(standardized, &st, &serr))
                code = std::move(standardized);
        }
        // After container decode + wire standardization a real chunk starts
        // with a Luau bytecode version. Anything else is not script data.
        // A 0x78 bulk can be a false-positive parse inside union physics data.
        if (code.empty() || code[0] < 3 || code[0] > 14) {
            ++scripts_skipped;
            if (std::getenv("RBX_LOG_SCRIPTS"))
                log("script id=" + std::to_string(owner) + " '" +
                    script->name() + "' skipped (payload is not a chunk)");
            return;
        }
        if (script->is_a("ModuleScript")) {
            // A ModuleScript is never auto-run: the native client keeps its
            // chunk and executes it the first time a script requires it. Hand
            // the ready chunk to the engine's require cache instead of
            // executing it here (running it eagerly would be observable:
            // module top-level code would run before its requirer and even
            // when nothing requires it).
            rbxch::engine::set_module_source(*env, script, std::move(code));
            ++modules_loaded;
            if (std::getenv("RBX_LOG_SCRIPTS"))
                log("module id=" + std::to_string(owner) + " '" +
                    script->name() + "' registered for require (parent " +
                    (script->parent() ? script->parent()->class_name() + " '" +
                                            script->parent()->name() + "'"
                                      : std::string("none")) + ")");
            return;
        }
        if (!client_runs(script)) {
            ++scripts_skipped;
            if (std::getenv("RBX_LOG_SCRIPTS"))
                log("script id=" + std::to_string(owner) + " '" +
                    script->name() + "' skipped (" + script->class_name() +
                    ", not client-run)");
            return;
        }
        if (ps.anchor >= 0) {
            const auto pit = rep->by_id.find(uint32_t(ps.anchor));
            if (pit != rep->by_id.end() && pit->second >= 0) {
                rbx::Instance* parent = rep->instances[size_t(pit->second)].get();
                if (script->parent() != parent) {
                    std::string rerr;
                    script->reparent(parent, &rerr);
                }
            }
        }
        if (std::getenv("RBX_LOG_SCRIPTS")) {
            std::string kids;
            for (const auto& child : script->children()) {
                if (!kids.empty()) kids += ",";
                kids += child->class_name() + " '" + child->name() + "'";
            }
            log("run id=" + std::to_string(owner) + " '" + script->name() +
                "' class=" + script->class_name() + " parent=" +
                (script->parent() ? script->parent()->name() : "none") +
                " children=[" + kids + "]");
        }
        // `script` is bound per chunk by the engine (per-thread environment),
        // not set as a global here: a script that yields must keep seeing its
        // own instance after another script has started.
        rbxch::Result r =
            rbxch::engine::execute(*env, code.data(), code.size(), script);
        if (r.status == rbxch::Status::Ok) {
            ++scripts_run;
            // Opt-in per-script trace: parity work needs to know which chunk
            // ran, not just that "some scripts" did.
            if (std::getenv("RBX_LOG_SCRIPTS"))
                log("script id=" + std::to_string(owner) + " '" +
                    script->name() + "' ok (" + std::to_string(code.size()) +
                    "B)");
        } else {
            ++scripts_failed;
            if (const char* dir = std::getenv("RBX_DUMP_SCRIPT_FAIL")) {
                char path[1024];
                snprintf(path, sizeof path, "%s/script_%u.bin", dir,
                         ps.script_id);
                if (FILE* f = std::fopen(path, "wb")) {
                    std::fwrite(code.data(), 1, code.size(), f);
                    std::fclose(f);
                }
            }
            if (scripts_failed <= 8) {
                std::string head;
                static const char* kHex = "0123456789abcdef";
                for (size_t k = 0; k < code.size() && k < 16; ++k) {
                    head.push_back(kHex[code[k] >> 4]);
                    head.push_back(kHex[code[k] & 15]);
                }
                log("script id=" + std::to_string(owner) +
                    " failed: " + r.error + " (" + std::to_string(code.size()) +
                    "B head=" + head + ")");
            }
        }
    }

    void pump(double dt) {
        // Build the world when the snapshot stream has gone quiet. This also
        // covers servers whose 0x84 landed before the batches (and servers
        // that never send the marker at all), without cutting a snapshot that
        // is still arriving.
        if (!snapshot_applied && snapshot_batch_seen &&
            std::chrono::steady_clock::now() - last_batch >=
                std::chrono::milliseconds(1000))
            finalizeSnapshot();
        if (!env)
            return;
        if (!debug_exec_done) {
            debug_exec_done = true;
            if (const char* path = std::getenv("RBX_EXEC_FILE")) {
                if (FILE* f = std::fopen(path, "rb")) {
                    std::string src;
                    char buf[4096];
                    size_t got = 0;
                    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0)
                        src.append(buf, got);
                    std::fclose(f);
                    lua_CompileOptions o;
                    std::memset(&o, 0, sizeof o);
                    size_t bc_size = 0;
                    char* bc = luau_compile(src.data(), src.size(), &o,
                                            &bc_size);
                    if (!bc) {
                        log("debug exec: compile failed");
                    } else {
                        rbxch::Result r =
                            rbxch::engine::execute(*env, bc, bc_size);
                        std::free(bc);
                        log(std::string("debug exec ") + path + ": " +
                            (r.status == rbxch::Status::Ok ? "ok" : r.error));
                    }
                } else {
                    log(std::string("debug exec: cannot open ") + path);
                }
            }
        }
        if (!(dt > 0.0))
            dt = 0.0;
        dt = std::min(dt, 0.25);
        rbxch::engine::step(*env, dt);
    }
};

LiveRuntime::LiveRuntime(std::string job_id, LogFn log, bool run_scripts,
                         bool gui)
    : impl_(std::make_unique<Impl>(std::move(job_id), std::move(log))) {
    impl_->run_scripts = run_scripts;
    impl_->gui = gui;
}

LiveRuntime::~LiveRuntime() = default;

void LiveRuntime::feed(const uint8_t* data, size_t len) {
    impl_->feed(data, len);
}

void LiveRuntime::setLocalUser(long long user_id, std::string user_name,
                               std::string display_name) {
    impl_->local_user = user_id;
    impl_->local_user_name = std::move(user_name);
    impl_->local_user_display = std::move(display_name);
    // The replication layer identifies our own Player by the same account
    // fields the native client does; install them before any record applies.
    rbx::set_local_user_id(user_id);
    rbx::set_local_user_name(impl_->local_user_name);
}

void LiveRuntime::feedMessage(const uint8_t* msg, size_t len) {
    impl_->feedMessage(msg, len);
}

void LiveRuntime::pump(double dt) {
    impl_->pump(dt);
}

bool LiveRuntime::has_world() const {
    return impl_->rep && impl_->rep->root;
}

bool LiveRuntime::join_data_seen() const {
    return impl_->join_data_seen;
}

long long LiveRuntime::ms_since_join_data() const {
    return Impl::ms_since(impl_->join_data_at);
}

long long LiveRuntime::ms_since_progress() const {
    // Before the first progress mark, the join data (0x97) is the reference:
    // a session that answered the join and then streamed nothing is stalled
    // from that moment, not from process start.
    if (!impl_->world_progress_)
        return Impl::ms_since(impl_->join_data_at);
    return Impl::ms_since(impl_->last_progress);
}

const rbx::Instance* LiveRuntime::root() const {
    return impl_->rep ? impl_->rep->root.get() : nullptr;
}

bool LiveRuntime::hasInstance(uint32_t id) const {
    return impl_->rep && impl_->rep->by_id.count(id) != 0;
}

std::string LiveRuntime::local_character() const {
    const rbx::Instance* rt = root();
    if (!rt)
        return {};
    const rbx::Instance* players =
        rt->find_first_child_of_class("Players");
    if (!players)
        return {};
    const long long self = rbx::local_user_id();
    if (self == 0)
        return {};
    for (const auto& child : players->children()) {
        const rbx::Instance* player = child.get();
        if (!player->is_a("Player"))
            continue;
        rbx::Variant uid;
        if (!player->get_prop("UserId", uid))
            continue;
        const auto* id = std::get_if<int64_t>(&uid);
        if (!id || *id != self)
            continue;
        rbx::Variant ch;
        if (!player->get_prop("Character", ch))
            return {};
        const auto* character = std::get_if<rbx::Instance*>(&ch);
        return (character && *character) ? (*character)->name()
                                         : std::string();
    }
    return {};
}

bool LiveRuntime::failed() const {
    return impl_->failed;
}

const std::string& LiveRuntime::error() const {
    return impl_->error;
}

int LiveRuntime::snapshot_records() const {
    return impl_->snapshot_records.empty() && impl_->rep
               ? impl_->rep->stats.spawned
               : int(impl_->snapshot_records.size());
}

int LiveRuntime::live_messages() const {
    return impl_->live_msgs;
}

int LiveRuntime::scripts_run() const {
    return impl_->scripts_run;
}

int LiveRuntime::scripts_failed() const {
    return impl_->scripts_failed;
}

std::string LiveRuntime::class_stats() const {
    if (!impl_->rep)
        return "";
    std::map<std::string, int> counts;
    for (const auto& inst : impl_->rep->instances)
        if (inst)
            ++counts[inst->class_name()];
    std::vector<std::pair<std::string, int>> top(counts.begin(), counts.end());
    std::sort(top.begin(), top.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string out;
    const size_t shown = getenv("RBX_CLASSES") ? top.size() : 20;
    for (size_t i = 0; i < top.size() && i < shown; ++i) {
        if (i)
            out += " ";
        out += top[i].first + ":" + std::to_string(top[i].second);
    }
    if (getenv("RBX_DUMP_PLAYERS") && impl_->rep && impl_->rep->root) {
        auto* root = impl_->rep->root.get();
        if (!root->find_first_child_of_class("Players")) {
            // A Player outside the Players service cannot fire PlayerAdded, so
            // name what the root actually holds instead of reporting nothing.
            out += " players-service=missing";
            size_t n = 0;
            for (const auto& c : root->children()) {
                if (n++ >= 12) break;
                out += " root{" + c->name() + ":" + c->class_name() + "}";
            }
        }
        if (auto* players = root->find_first_child_of_class("Players")) {
            for (const auto& child : players->children()) {
                rbx::Instance* p = child.get();
                if (!p->is_a("Player"))
                    continue;
                out += " player{" + p->name();
                rbx::Variant uid;
                if (p->get_prop("UserId", uid))
                    if (const auto* n = std::get_if<int64_t>(&uid))
                        out += ":" + std::to_string(*n);
                rbx::Variant ch;
                if (p->get_prop("Character", ch)) {
                    if (const auto* c = std::get_if<rbx::Instance*>(&ch); c && *c)
                        out += ":char=" + (*c)->name() + "(" + (*c)->class_name() + ");parent=" + ((*c)->parent() ? (*c)->parent()->name() : std::string("nil"));
                    else
                        out += ":char=nil";
                } else {
                    out += ":char=unset";
                }
                out += "}";
            }
        }
        if (auto* ws = root->find_first_child_of_class("Workspace")) {
            for (const auto& child : ws->children()) {
                if (!child->is_a("Model")) continue;
                bool humanoid = false;
                for (const auto& gc : child->children())
                    if (gc->is_a("Humanoid")) humanoid = true;
                if (humanoid)
                    out += " hummodel{" + child->name() + ":" +
                           std::to_string(child->children().size()) + "}";
            }
        }
    }
    return out;
}

namespace {
// "id:count,..." in ascending id order: the whole inbound census on one line.
std::string census(const std::map<int, int>& m) {
    std::string out;
    for (const auto& kv : m) {
        if (!out.empty())
            out.push_back(',');
        char b[24];
        snprintf(b, sizeof b, "%02x:%d", unsigned(kv.first), kv.second);
        out += b;
    }
    return out;
}

// Reason strings can contain commas, so quote them and escape the quote.
// Only the `limit` most frequent entries are emitted: a long run has hundreds
// of distinct (class, tail-length, member) shapes and printing them all turns
// the one-line stats into tens of kilobytes.
std::string census_str(const std::map<std::string, int>& m, size_t limit = 12) {
    std::vector<std::pair<int, std::string>> top;
    top.reserve(m.size());
    for (const auto& kv : m)
        top.push_back({kv.second, kv.first});
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first)
            return a.first > b.first;
        return a.second < b.second;
    });
    std::string out;
    const size_t shown = std::min(limit, top.size());
    for (size_t i = 0; i < shown; ++i) {
        if (!out.empty())
            out.push_back(';');
        out.push_back('"');
        for (char ch : top[i].second) {
            if (ch == '"' || ch == '\\')
                out.push_back('\\');
            out.push_back(ch);
        }
        out += "\":" + std::to_string(top[i].first);
    }
    if (top.size() > shown)
        out += ";+" + std::to_string(top.size() - shown) + " more";
    return out;
}
} // namespace

std::string LiveRuntime::stats() const {
    const Impl& s = *impl_;
    std::string out = "live_frames=" + std::to_string(s.live_msgs) +
                      " schema=" + (s.have_schema ? "yes" : "no") +
                      " pending=" + std::to_string(s.pending.size()) +
                      " snap_batches=" + std::to_string(s.snapshot_batches) +
                      " shared=" + std::to_string(s.shared_batches) +
                      " stops=" + std::to_string(s.live_stops) +
                      " walk_fail=" + std::to_string(s.live_failures) +
                      " spawn=" + std::to_string(s.sst.spawned) +
                      " update=" + std::to_string(s.sst.updated) +
                      " entries=" + std::to_string(s.sst.entries_applied) +
                      " prop3=" + std::to_string(s.sst.prop_updates) +
                      " prop_miss=" + std::to_string(s.sst.prop_missing) +
                      " prop_def=" + std::to_string(s.sst.prop_deferred) +
                      " prop_drop=" + std::to_string(s.sst.prop_dropped) +
                      " events=" + std::to_string(s.sst.events_logged) +
                      " fired=" + std::to_string(s.sst.events_fired) +
                      " bulks=" + std::to_string(s.sst.bulks) +
                      " regions=" + std::to_string(s.regions) +
                      " a1=" + std::to_string(s.a1_messages) + "/" +
                      std::to_string(s.a1_records) + " applied=" +
                      std::to_string(s.a1_applied_records) + "/" +
                      std::to_string(s.a1_props) + " a1fail=" +
                      std::to_string(s.a1_body_fail) + " a1part=" +
                      std::to_string(s.a1_partial_records) +
                      " a1why=" + census_str(s.a1_fail_reasons) +
                      " a1_first=" + census(s.a1_first_class) +
                      " pairs=" + std::to_string(s.instance_pair_messages) +
                      "/" + std::to_string(s.instance_pair_records) +
                      " scripts=" + std::to_string(s.scripts_run) + "/" +
                      std::to_string(s.scripts_failed) + "/" +
                      std::to_string(s.scripts_skipped) +
                      " modules=" + std::to_string(s.modules_loaded) +
                      " shared_str=" + std::to_string(s.shared_strings_seen) +
                      " netasset=" + std::to_string(s.netasset_batches) +
                      "/" + std::to_string(s.netasset_entries) +
                      " unk_flag=" + std::to_string(s.unknown_batch_flags) +
                      " unk_id=" + std::to_string(s.unhandled_ids) +
                      " stop=" + census(s.stop_types) +
                      " proto=" + census(s.transport_ids) +
                      " ids=" + census(s.app_ids) + " sub=" + census(s.sub_ids);
    return out;
}

} // namespace rbxclient
