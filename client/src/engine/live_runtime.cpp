// live_runtime.cpp — see live_runtime.h.
#include "engine/live_runtime.h"

#include <functional>

#include "exec/exec.h"
#include "send/bridge.h"
#include "send/entity_batch.h"

#include "lua.h"

#include <cmath>

#include "raknet/protocol.h"

#include "engine.h"
#include "instance/class_registry.h"
#include "instance/instance.h"
#include "instance/signal.h"
#include "solve/joindata.h"
#include "solve/replicate.h"
#include "solve/standardize.h"
#include "common/util.h"

#include "lua.h"

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

struct PendingScript {
    uint32_t script_id = 0;
    int64_t anchor = -1;
    std::vector<uint8_t> payload;
};

} // namespace

struct LiveRuntime::Impl {
    std::string job;
    long long place_id = 0;
    long long universe_id = 0;
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

    rbxch::engine::Environment* env = nullptr;
    StreamStats sst;
    std::vector<PendingScript> scripts;
    // --no-scripts: keep parsing + the instance world, skip Luau execution.
    bool run_scripts = true;
    // --exec: user script run once through the executor surface.
    std::string exec_source;
    // Batch builders resolve refs / schema indices through this.
    rbx::exec::WorldView view;
    double interest_accum = 0.0;
    uint32_t interest_clock = 0x20000;
    // Client-side physics for our own character: the RCC accepts a CFrame
    // upload (0x83/0x05) instead of a walk request, so the position has to be
    // integrated here.
    double phys_accum = 0.0;
    // Upload cadence. Measured: the reference's 0x83/0x05 arrives every 0.5 s
    // (2 Hz) while physics itself integrates at 10 Hz.
    double send_accum = 0.0;
    // Last script-issued position; re-sent at 2 Hz until a character exists.
    float nudge_pos[3] = {0, 0, 0};
    float nudge_vel[3] = {0, 0, 0};
    bool have_nudge = false;
    double nudge_accum = 0.0;
    // The server's own millisecond clock, learned from its 0x83/05 heartbeat
    // (29 B: `83 05 10 <clock u32> ...`). 0x83/0a ownership claims carry this
    // domain, not the client session clock.
    uint32_t srv_clock_ms = 0;
    bool srv_clock_adopted = false;
    std::chrono::steady_clock::time_point srv_clock_at{};
    uint32_t phys_tick = 1;
    float move_dir[3] = {0, 0, 0};
    float phys_pos[3] = {0, 0, 0};
    float phys_vel[3] = {0, 0, 0};
    float claim_accum = 0.0f;
    bool interest_sent_once = false;
    float ground_y = 0.0f;
    bool grounded = true;
    bool phys_init = false;
    double phys_speed = 16.0;

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
    int instance_pair_messages = 0;
    int instance_pair_records = 0;
    int chat_messages = 0;
    std::map<int, int> a1_first_class;
    int scripts_run = 0;
    int scripts_failed = 0;
    int scripts_skipped = 0;
    std::set<const rbx::Instance*> known_players;
    std::map<const rbx::Instance*, const rbx::Instance*> known_characters;
    // Roster as replication last showed it (UserId -> Name). The RCC removes a
    // Player from everyone else's view without telling that client, so a diff
    // here is the only way the session can report a leave it actually saw.
    std::map<int64_t, std::string> roster;

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
        if (getenv("RBX_DIAG")) {
            static int a1_dumps = 0;
            if (a1_dumps < 3) {
                ++a1_dumps;
                static const char* kH = "0123456789abcdef";
                std::string hx;
                for (size_t k = 0; k < len && k < 96; ++k) {
                    hx.push_back(kH[body[k] >> 4]);
                    hx.push_back(kH[body[k] & 15]);
                }
                log("[diag] a1 len=" + std::to_string(len) + " " + hx);
            }
        }
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
        // Find our own body's ref by position: the A1 stream carries every
        // instance's CFrame, including the character Model the RCC never
        // replicates to us. Matching the spawn position inside a record's raw
        // data names the ref a CFrame write must address.
        if (getenv("RBX_ADOPT") && have_nudge) {
            // Our body is the instance the RCC owns for us and never sends as a
            // spawn: it appears in A1 by ref only. Adopt its translation so the
            // transform claim starts where the body actually is — a 65-stud
            // mismatch was measured between the picked SpawnLocation and the
            // real spawn point.
            float best_d2 = 150.0f * 150.0f;
            float bx = 0, by = 0, bz = 0;
            bool got = false;
            for (const AppStateRecord& r : f.records) {
                if (!r.id || (rep && rep->by_id.count(r.id))) continue;
                for (size_t k = 0; k + 12 <= r.data.size(); ++k) {
                    float x, y, z;
                    memcpy(&x, r.data.data() + k, 4);
                    memcpy(&y, r.data.data() + k + 4, 4);
                    memcpy(&z, r.data.data() + k + 8, 4);
                    if (!(fabs(x) < 5000 && fabs(y) < 5000 && fabs(z) < 5000))
                        continue;
                    const float dx = x - nudge_pos[0], dy = y - nudge_pos[1],
                                dz = z - nudge_pos[2];
                    const float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < best_d2) {
                        best_d2 = d2;
                        bx = x; by = y; bz = z;
                        got = true;
                    }
                }
            }
            if (got) {
                static int adopt_logs = 0;
                if (adopt_logs < 3) {
                    ++adopt_logs;
                    char b[128];
                    snprintf(b, sizeof b,
                             "[adopt] body pos %.2f %.2f %.2f (from %.2f %.2f "
                             "%.2f)", bx, by, bz, nudge_pos[0], nudge_pos[1],
                             nudge_pos[2]);
                    log(b);
                }
                // Move the local rig and the integrator onto the real body so
                // the claim starts where the server has us, not where the
                // picked SpawnLocation is.
                const float dx0 = phys_pos[0] - bx, dy0 = phys_pos[1] - by,
                            dz0 = phys_pos[2] - bz;
                if (!phys_init || (dx0 * dx0 + dy0 * dy0 + dz0 * dz0) > 4.0f) {
                    phys_pos[0] = bx;
                    phys_pos[1] = by;
                    phys_pos[2] = bz;
                    ground_y = by;
                    phys_init = true;
                }
                nudge_pos[0] = bx;
                nudge_pos[1] = by;
                nudge_pos[2] = bz;
            }
        }
        if (getenv("RBX_A1FIND")) {
            // RBX_A1FIND=x,y,z: look for that translation anywhere in the A1
            // state records. "Did the server apply our upload" becomes a local
            // question that needs no second client.
            float tx = 0, ty = 0, tz = 0;
            if (sscanf(getenv("RBX_A1FIND"), "%f,%f,%f", &tx, &ty, &tz) == 3) {
                for (const AppStateRecord& r : f.records) {
                    for (size_t k = 0; k + 12 <= r.data.size(); ++k) {
                        float x, y, z;
                        memcpy(&x, r.data.data() + k, 4);
                        memcpy(&y, r.data.data() + k + 4, 4);
                        memcpy(&z, r.data.data() + k + 8, 4);
                        if (fabs(x - tx) < 1.0f && fabs(y - ty) < 1.0f &&
                            fabs(z - tz) < 1.0f) {
                            static int hits = 0;
                            if (hits < 4) {
                                ++hits;
                                char b[128];
                                snprintf(b, sizeof b,
                                         "[diag] A1FIND hit ref=0x%x cls=%u "
                                         "(%.1f,%.1f,%.1f) known=%d",
                                         r.id, unsigned(r.cls), x, y, z,
                                         rep && rep->by_id.count(r.id) ? 1 : 0);
                                log(b);
                            }
                            break;
                        }
                    }
                }
            }
        }
        if (getenv("RBX_DIAG") && have_nudge) {
            static int body_ref_probe = 0;
            if (body_ref_probe < 6) {
                for (const AppStateRecord& r : f.records) {
                    if (!r.id || !rep || rep->by_id.count(r.id)) continue;
                    for (size_t k = 0; k + 12 <= r.data.size(); ++k) {
                        float x, y, z;
                        memcpy(&x, r.data.data() + k, 4);
                        memcpy(&y, r.data.data() + k + 4, 4);
                        memcpy(&z, r.data.data() + k + 8, 4);
                        if (fabs(x - nudge_pos[0]) < 0.6f &&
                            fabs(y - nudge_pos[1]) < 0.6f &&
                            fabs(z - nudge_pos[2]) < 0.6f) {
                            ++body_ref_probe;
                            char b[128];
                            snprintf(b, sizeof b,
                                     "[diag] A1 body candidate ref=0x%x cls=%u "
                                     "at (%.2f,%.2f,%.2f) off=%zu",
                                     r.id, unsigned(r.cls), x, y, z, k);
                            log(b);
                            break;
                        }
                    }
                }
            }
        }
        ++a1_messages;
        a1_records += int(f.records.size());
        if (!have_schema || f.records.empty())
            return;
        for (const AppStateRecord& r : f.records) {
            if (r.id && r.cls < schema.classes.size())
                ++a1_first_class[int(r.cls)];
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
            return;
        }
        if (!snapshot_applied) {
            ++snapshot_batches;
            snapshot_batch_seen = true;
            last_batch = std::chrono::steady_clock::now();
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
        // 0x83/06 inbound: candidate ownership/ack record. Dump it and compare
        // the refs against our own body refs (printed when the character links)
        // to see whether the server ever grants us the body.
        if (getenv("RBX_DIAG") && body[1] == 0x06 && len <= 80) {
            static int probe06 = 0;
            if (probe06 < 10) {
                ++probe06;
                static const char* kH = "0123456789abcdef";
                std::string hx;
                for (size_t k = 0; k < len; ++k) {
                    hx.push_back(kH[body[k] >> 4]);
                    hx.push_back(kH[body[k] & 15]);
                }
                log("[diag] rx06 len=" + std::to_string(len) + " " + hx);
            }
        }
        // 0x83/05 inbound: 29 B, unreliable, ~2 Hz. It carries the server's
        // millisecond clock, which is the domain 0x83/0a claims must use.
        if (body[1] == 0x05 && len >= 7 && body[2] == 0x10) {
            const uint32_t c = uint32_t(body[3]) | (uint32_t(body[4]) << 8) |
                               (uint32_t(body[5]) << 16) | (uint32_t(body[6]) << 24);
            if (c) {
                srv_clock_ms = c;
                srv_clock_at = std::chrono::steady_clock::now();
                // Adopt the server's epoch for our own uploads too, once.
                // Everything we send used the 2730-scale session clock taken
                // from a capture; the server's domain is ~29.5e6. If uploads
                // are validated against it, ours were 8 hours stale.
                if (!srv_clock_adopted) {
                    srv_clock_adopted = true;
                    rbxclient::batch::rebase_net_clock(c + 60);
                }
            }
        }
        // Ownership probe: does the server ever name our own body refs on the
        // wire? If nothing inbound references them, no network ownership was
        // granted and the 0x05 transform is being discarded, not rejected.
        // Does the server ever name our own Player ref on the wire? A Model
        // spawn parented to it is exactly what we never see locally.
        if (getenv("RBX_DIAG") && env && rep && rep->root && len >= 6) {
            static int player_ref_probe = 0;
            const uint32_t pref = self_player_ref();
            if (pref && player_ref_probe < 12) {
                for (size_t k = 0; k + 4 <= len; ++k) {
                    const uint32_t v = uint32_t(body[k]) |
                                       (uint32_t(body[k + 1]) << 8) |
                                       (uint32_t(body[k + 2]) << 16) |
                                       (uint32_t(body[k + 3]) << 24);
                    if (v != pref) continue;
                    ++player_ref_probe;
                    char b[240];
                    std::string tail;
                    static const char* kH = "0123456789abcdef";
                    const size_t from = k > 16 ? k - 16 : 0;
                    for (size_t j = from; j < len && j < from + 48; ++j) {
                        tail.push_back(kH[body[j] >> 4]);
                        tail.push_back(kH[body[j] & 15]);
                    }
                    snprintf(b, sizeof b,
                             "[diag] inbound names player ref 0x%x sub=0x%02x "
                             "off=%zu len=%zu ctx=%s",
                             pref, unsigned(body[1]), k, len, tail.c_str());
                    log(b);
                    break;
                }
            }
        }
        if (getenv("RBX_DIAG") && env && rep && rep->root && len >= 6) {
            static int ref_probe = 0;
            if (ref_probe < 24) {
                rbx::Instance* ch = local_character_inst();
                rbx::Instance* root_inst =
                    ch ? ch->find_first_child_of_class("HumanoidRootPart")
                       : nullptr;
                const uint32_t self_ref = root_inst ? ref_of(root_inst) : 0;
                if (self_ref) {
                    for (size_t k = 0; k + 4 <= len; ++k) {
                        const uint32_t v = uint32_t(body[k]) |
                                           (uint32_t(body[k + 1]) << 8) |
                                           (uint32_t(body[k + 2]) << 16) |
                                           (uint32_t(body[k + 3]) << 24);
                        if (v != self_ref) continue;
                        ++ref_probe;
                        char b[64];
                        snprintf(b, sizeof b,
                                 "[diag] inbound names self ref 0x%x sub=0x%02x "
                                 "off=%zu",
                                 self_ref, unsigned(body[1]), k);
                        log(b);
                        break;
                    }
                }
            }
        }
        // 0x83/05 inbound: 29 B, unreliable, ~2 Hz. Presumed the server's
        // authoritative transform for our own body; nothing parses it yet.
        if (getenv("RBX_DIAG") && body[1] == 0x05 && len <= 64) {
            static int probe05 = 0;
            if (probe05 < 6) {
                ++probe05;
                static const char* kH = "0123456789abcdef";
                std::string hx;
                for (size_t k = 0; k < len; ++k) {
                    hx.push_back(kH[body[k] >> 4]);
                    hx.push_back(kH[body[k] & 15]);
                }
                log("[diag] rx05 len=" + std::to_string(len) + " " + hx);
            }
        }
        Walk83 w;
        std::string err;
        if (!walk83ItemsEx(body, len, schema, &props, w, &err)) {
            ++live_failures;
            if (live_failures <= 8) {
                // The failure text names the offset but not the bytes, which
                // is exactly what we need to see when a spawn is dropped.
                std::string hx;
                static const char* kH = "0123456789abcdef";
                const size_t from = w.stop_off < len ? w.stop_off : 0;
                for (size_t k = from; k < len && k < from + 40; ++k) {
                    hx.push_back(kH[body[k] >> 4]);
                    hx.push_back(kH[body[k] & 15]);
                }
                log("0x83 walk failed: " + err + " type=0x" +
                    std::to_string(unsigned(w.stop_type)) + " off=" +
                    std::to_string(w.stop_off) + " bytes=" + hx);
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
        uint32_t last_spawn = 0;
        bool have_last_spawn = false;
        for (const ChainItem& it : w.items) {
            switch (it.type) {
            case 0x02:
                // Apply-once: the RCC resends spawns until acked, and applying
                // one twice created duplicate PlayerGui/Backpack children.
                if (rep && it.spawn.id && rep->by_id.count(it.spawn.id)) {
                    ++dup_spawns;
                    if (it.spawn.id && ack_queue.size() < 8192)
                        ack_queue.push_back(it.spawn.id);
                    break;
                }
                // A spawn parented to us is our own character. Log it: this is
                // the one record whose absence explains everything.
                if (getenv("RBX_DIAG")) {
                    const uint32_t self = self_player_ref();
                    if (self && it.spawn.parent == int64_t(self)) {
                        const std::string cname =
                            size_t(it.spawn.cls) < schema.classes.size()
                                ? schema.classes[it.spawn.cls].name
                                : std::string("?");
                        char b[128];
                        snprintf(b, sizeof b,
                                 "[diag] spawn under self parent ref=0x%x "
                                 "class=%u(%s) props=%zu",
                                 it.spawn.id, unsigned(it.spawn.cls),
                                 cname.c_str(),
                                 it.spawn.props_a.size() + it.spawn.props_b.size());
                        log(b);
                    }
                }
                applyStreamRecord(*rep, schema, it.spawn.id, it.spawn.cls,
                                  mergeProps(it.spawn.props_a, it.spawn.props_b),
                                  it.spawn.parent, sst);
                // Ack the spawn itself: the RCC only streams further records
                // once the ones we received are confirmed.
                if (it.spawn.id && ack_queue.size() < 8192)
                    ack_queue.push_back(it.spawn.id);
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
                if (have_last_spawn && !it.bulk.payload.empty()) {
                    PendingScript ps;
                    ps.script_id = last_spawn;
                    ps.anchor = it.bulk.anchor;
                    ps.payload = it.bulk.payload;
                    if (snapshot_applied)
                        runScript(ps);
                    else
                        scripts.push_back(std::move(ps));
                }
                break;
            default:
                break;
            }
        }
        if (snapshot_applied) {
            syncPlayers();
            // The signal above (or one that arrived before the snapshot) is
            // what tells the client to construct its own avatar.
            if (self_char_appearance) build_self_character();
        }
    }

    static std::vector<JoinProp> mergeProps(const std::vector<JoinProp>& a,
                                            const std::vector<JoinProp>& b) {
        std::vector<JoinProp> out = a;
        out.insert(out.end(), b.begin(), b.end());
        return out;
    }

    // Refs of records we actually applied, waiting to be acknowledged. The
    // RCC retransmits property chains until they are acked: a native session
    // receives ~9 chains in 60 s, ours received 2468 because our 0x83/06
    // carried a fabricated ref instead of these.
    std::vector<uint32_t> ack_queue;
    double ack_accum = 0.0;
    double ready_accum = 0.0;
    rbx::Instance* self_char_model = nullptr;
    rbx::Instance* self_char_root = nullptr;
    uint32_t self_char_ref = 0;
    uint32_t owner_unknown_ref = 0;
    std::vector<uint32_t> owner_refs;
    int dup_spawns = 0;
    // Client-authored character: the RCC sends InternalCharacterAppearanceLoaded
    // for our Player but never replicates the Model itself. Build it locally on
    // that signal, then announce it upstream (0x83/02) so the server has a ref
    // for the physics uploads.
    bool self_char_appearance = false;
    bool net_clock_rebased = false;

    void applyTriple(const PropTriple& p) {
        // Player.Character naming a ref we never spawned is the whole bug in
        // one line: print the ref and whether the instance exists locally.
        if (getenv("RBX_DIAG") && p.id) {
            const auto nm = props.member_name.find(p.prop);
            if (nm != props.member_name.end() && nm->second == "Character") {
                std::string hx;
                static const char* kH = "0123456789abcdef";
                for (uint8_t byte : p.value) {
                    hx.push_back(kH[byte >> 4]);
                    hx.push_back(kH[byte & 15]);
                }
                char b[200];
                snprintf(b, sizeof b,
                         "[diag] Character prop on ref=0x%x tag=0x%x value=%s",
                         p.id, unsigned(p.tag), hx.c_str());
                log(b);
            }
        }
        ++sst.prop_updates;
        if (p.id != 0 && p.id != 0xffffffffu && ack_queue.size() < 8192)
            ack_queue.push_back(p.id);
        if (!rep || !rep->root || p.lo == 0)
            return;
        const auto name = props.member_name.find(p.prop);
        const auto cls = props.owner_class.find(p.prop);
        if (name == props.member_name.end() || cls == props.owner_class.end()) {
            ++sst.prop_missing;
            return;
        }
        // The appearance-loaded signal is a hidden Player *property* in this
        // build (measured), not a separate event item: it marks the one point
        // where the owner is expected to construct its own avatar.
        if (name->second == "NetworkOwnerV3") {
            // tag 0x12 is a bool-ish value; the ref is the interesting part: an
            // owner assignment for a ref we do not have is OUR body part.
            const bool known = rep->by_id.count(p.id) != 0;
            static int no_hits = 0;
            if (no_hits < 40) {
                ++no_hits;
                char b[120];
                snprintf(b, sizeof b,
                         "[diag] NetworkOwnerV3 ref=0x%x known=%d v0=%d",
                         p.id, known ? 1 : 0,
                         p.value.empty() ? -1 : int(p.value[0]));
                log(b);
            }
            {
                const auto oc = props.owner_class.find(p.prop);
                static int cls_logged = 0;
                if (cls_logged < 8) {
                    ++cls_logged;
                    std::string cname = "?";
                    if (oc != props.owner_class.end() &&
                        size_t(oc->second) < schema.classes.size())
                        cname = schema.classes[size_t(oc->second)].name;
                    log("selfchar: owner prop class=" + cname + " known=" +
                        std::to_string(known ? 1 : 0));
                }
            }
            if (!known) {
                static int unknown_owner = 0;
                if (unknown_owner < 4) {
                    ++unknown_owner;
                    owner_unknown_ref = p.id;
                    owner_refs.push_back(p.id);
                    // Ack the assignment: the RCC's 0x83/06 record list is the
                    // only client-side way to confirm we took the instance.
                    if (ack_queue.size() < 8192) ack_queue.push_back(p.id);
                    log("selfchar: unknown-ref owner assignment 0x" +
                        std::to_string(p.id));
                }
            }
        }
        if (name->second == "InternalCharacterAppearanceLoaded") {
            const uint32_t self = self_player_ref();
            if (self && p.id == self) {
                if (!self_char_appearance)
                    log("selfchar: InternalCharacterAppearanceLoaded");
                self_char_appearance = true;
            }
        }
        JoinProp jp;
        jp.name = name->second;
        jp.tag = p.tag;
        jp.value = p.value;
        if (!applyStreamProp(*rep, schema, p.id, cls->second, jp, sst))
            ++sst.prop_missing;
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
        opt.place_id = place_id;
        opt.universe_id = universe_id;
        opt.profile = rbxch::Profile::Engine;
        env = rbxch::engine::create(opt);
        if (!env) {
            fail("engine::create failed");
            return;
        }
        rbxch::engine::attach(*env, rep->root.get());
        // Executor surface goes on after attach so it can extend `game`
        // (HttpGet) and sees the real DataModel, exactly like a script would.
        lua_State* exec_L = rbxch::engine::state(*env);
        rbx::exec::install(exec_L);
        view.ctx = this;
        view.ref_of = [](void* c, rbx::Instance* i) {
            return static_cast<Impl*>(c)->ref_of(i);
        };
        view.event_index = [](void* c, const char* cl, const char* ev) {
            return static_cast<Impl*>(c)->event_index(cl, ev);
        };
        view.member_index = [](void* c, const char* cl, const char* m) {
            return static_cast<Impl*>(c)->member_index(cl, m);
        };
        view.local_humanoid = [](void* c) {
            return static_cast<Impl*>(c)->local_humanoid();
        };
        view.nudge_pos = [](void* c, float x, float y, float z) {
            Impl* s = static_cast<Impl*>(c);
            s->nudge_pos[0] = x;
            s->nudge_pos[1] = y;
            s->nudge_pos[2] = z;
            s->have_nudge = true;
            s->nudge_accum = 0.0;
        };
        rbx::exec::set_world_view(&view);
        // RBX_FORCE_POS=x,y,z: claim an explicit spawn. Used to test whether
        // the server accepts a transform that starts exactly where the body is
        // (the observer reports the body's CFrame; the SpawnLocation we pick is
        // not always the one the RCC used, measured 57 studs apart).
        if (const char* fp = getenv("RBX_FORCE_POS")) {
            float x, y, z;
            if (sscanf(fp, "%f,%f,%f", &x, &y, &z) == 3) {
                nudge_pos[0] = x;
                nudge_pos[1] = y;
                nudge_pos[2] = z;
                have_nudge = true;
                phys_pos[0] = x;
                phys_pos[1] = y;
                phys_pos[2] = z;
                ground_y = y;
                phys_init = true;
                char b[96];
                snprintf(b, sizeof b, "[phys] forced pos %.2f %.2f %.2f", x, y, z);
                log(b);
            }
        }
        // Seed the spawn claim without a script: find the map's SpawnLocation
        // and start the 2 Hz transform at spawn height. Measured: a session
        // that never sends 0x83/05 never gets an avatar, and every earlier
        // fixture that spawned was one sending position uploads while waiting.
        if (!have_nudge) {
            rbx::Instance* ws =
                rep->root->find_first_child_of_class("Workspace");
            std::function<rbx::Instance*(rbx::Instance*, int)> find =
                [&](rbx::Instance* n, int depth) -> rbx::Instance* {
                if (!n || depth > 6) return nullptr;
                if (n->is_a("SpawnLocation")) return n;
                for (const auto& c : n->children())
                    if (rbx::Instance* hit = find(c.get(), depth + 1)) return hit;
                return nullptr;
            };
            rbx::Instance* sp = find(ws, 0);
            rbx::Variant sv;
            if (sp && sp->get_prop("CFrame", sv)) {
                if (const auto* scf = std::get_if<rbx::CFrame>(&sv)) {
                    nudge_pos[0] = scf->x;
                    nudge_pos[1] = scf->y + 3.0f;
                    nudge_pos[2] = scf->z;
                    have_nudge = true;
                    char b[96];
                    snprintf(b, sizeof b,
                             "[phys] spawn claim seeded %.1f %.1f %.1f",
                             nudge_pos[0], nudge_pos[1],
                             nudge_pos[2]);
                    log(b);
                }
            }
        }
        build_self_character();
        // The reference sends this 12-byte readiness marker once, between the
        // snapshot and its first position upload. We never sent it.
        {
            // Carry the first transform in the ready message itself: the
            // reference does (04 <event> 05 <transform> 00, measured), and
            // ownership is decided when the body spawns, not retroactively.
            const uint32_t rc = rbxclient::batch::net_clock_ms();
            const std::vector<uint8_t> rb =
                have_nudge
                    ? rbxclient::batch::ready_with_transform(
                          rc, nudge_pos[0], nudge_pos[1], nudge_pos[2], 0u, 0.0f)
                    : rbxclient::batch::ready_batch(rc);
            if (const rbx::send::Api* api = rbx::send::installed())
                if (api->raw) api->raw(api->ctx, rb.data(), rb.size(), true);
        }
        if (!exec_source.empty()) {
            std::string eerr;
            if (rbx::exec::run_source(exec_L, exec_source.data(), exec_source.size(), &eerr))
                log("exec: user script started");
            else
                log("exec: user script failed: " + eerr);
            exec_source.clear(); // run once
        }
        snapshot_applied = true;
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

    // Fire Players.PlayerAdded / Player.CharacterAdded for Players that
    // replication created or updated since the last chain. The Engine's
    // LocalPlayer/NumPlayers properties are live views (see instance.cpp),
    // so no cached identity needs refreshing here.
    void syncPlayers() {
        if (!env || !rep || !rep->root)
            return;
        rbx::Instance* players = rep->root->find_first_child_of_class("Players");
        if (!players)
            return;
        // RBX_DIAG=1 traces why the local-Player synthesis below does or does
        // not run. Off by default; every line is prefixed so it greps clean.
        const bool diag = std::getenv("RBX_DIAG") != nullptr;
        if (diag)
            std::fprintf(stderr, "[diag] syncPlayers children=%zu\n",
                         players->children().size());
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
        // Our own Player IS replicated to us, just after the initial snapshot, so
        // fabricating one here produced a second Player with the same name and a
        // different identity (measured 2026-11-05: Players listed
        // poopnestomegahater twice within one run). Players.LocalPlayer now
        // returns nil until the real row streams in, so a fabricated one is
        // redundant as well as wrong. Kept as a probe only.
        const std::string& me_name = rbx::local_user_name();
        const long long me_id = rbx::local_user_id();
        if (!diag)
            return;
        int rows = 0;
        for (const auto& child : players->children())
            if (child->is_a("Player") && child->name() == me_name)
                ++rows;
        std::fprintf(stderr, "[diag] local name='%s' id=%lld rows=%d\n",
                     me_name.c_str(), me_id, rows);
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
        if (player->set_prop("Character", rbx::Variant(best), &err)) {
            log("linked character '" + *pname + "' (" +
                std::to_string(best_parts) + " parts)");
            if (getenv("RBX_DIAG")) {
                // Refs a third-party client sees for another player: the Player
                // and its Character Model. A capture of the *owner* can then be
                // searched for this Model ref, which answers whether the RCC
                // sends a player its own Model.
                log("refs player=" + std::to_string(ref_of(player)) +
                    " model=" + std::to_string(ref_of(best)) +
                    " parent=" + std::to_string(ref_of(player)) + " name=" +
                    *pname);
            }
            if (getenv("RBX_DIAG")) {
                rbx::Instance* root_inst =
                    best->find_first_child_of_class("HumanoidRootPart");
                rbx::Instance* hum = best->find_first_child_of_class("Humanoid");
                char b[96];
                snprintf(b, sizeof b,
                         "[diag] body refs model=0x%x root=0x%x hum=0x%x",
                         ref_of(best), root_inst ? ref_of(root_inst) : 0,
                         hum ? ref_of(hum) : 0);
                log(b);
            }
        }
        else
            log("character link failed for '" + *pname + "': " + err);
    }

    // Build and replicate our own character. The RCC never assigns
    // Player.Character on the owner's own client and never sends the Model, so
    // the client is expected to construct it.
    void build_self_character() {
        // --- client-authored character (RBX_SELFCHAR=1) -------------------
        // The RCC never assigns Player.Character on the owner's own client and
        // never replicates the Model to us, so the client is expected to build
        // it locally and replicate it upstream. Without it the physics path has
        // no root and every transform is refused.
        if (getenv("RBX_SELFCHAR") && self_char_appearance &&
                !local_character_inst() && !self_char_ref && have_nudge) {
            auto class_id = [&](const char* name) -> int {
                for (size_t i = 0; i < schema.classes.size(); ++i)
                    if (schema.classes[i].name == name) return int(i);
                return -1;
            };
            const int model_cls = class_id("Model");
            const int part_cls = class_id("Part");
            const int hum_cls = class_id("Humanoid");
            rbx::Instance* players =
                rep->root ? rep->root->find_first_child_of_class("Players")
                          : nullptr;
            rbx::Variant lpv;
            rbx::Instance* me = nullptr;
            if (players && players->get_prop("LocalPlayer", lpv))
                if (const auto* p = std::get_if<rbx::Instance*>(&lpv)) me = *p;
            if (me && model_cls >= 0 && part_cls >= 0 && hum_cls >= 0) {
                // Refs the server has never issued. A fixed block can collide
                // with a stream id on a busy server; walk forward until all
                // three are free (ids are u32; the lo scope stays 1 like the
                // rest of this session's refs).
                uint32_t kBase = 0x00B00000u;
                while (rep->by_id.count(kBase) || rep->by_id.count(kBase + 1) ||
                       rep->by_id.count(kBase + 2))
                    kBase += 4;
                std::string err;
                const rbx::ClassInfo* mcls = rbx::find_class("Model");
                const rbx::ClassInfo* pcls = rbx::find_class("Part");
                const rbx::ClassInfo* hcls = rbx::find_class("Humanoid");
                rbx::Ref<rbx::Instance> model(
                    mcls ? rbx::instantiate(mcls) : nullptr);
                rbx::Ref<rbx::Instance> hrp(
                    pcls ? rbx::instantiate(pcls) : nullptr);
                rbx::Ref<rbx::Instance> hum(
                    hcls ? rbx::instantiate(hcls) : nullptr);
                if (model && hrp && hum) {
                    const uint32_t mref = kBase, rref = kBase + 1,
                                   href = kBase + 2;
                    rbx::CFrame cf;
                    cf.x = nudge_pos[0];
                    cf.y = nudge_pos[1];
                    cf.z = nudge_pos[2];
                    hrp->set_prop("Name", rbx::Variant(std::string(
                                              "HumanoidRootPart")), &err);
                    hrp->set_prop("CFrame", rbx::Variant(cf), &err);
                    hrp->reparent(model.get(), &err);
                    hum->reparent(model.get(), &err);
                    model->set_prop("Name", rbx::Variant(me->name()), &err);
                    model->reparent(me, &err);
                    me->set_prop("Character",
                                 rbx::Variant(static_cast<rbx::Instance*>(
                                     model.get())), &err);
                    auto reg = [&](uint32_t r, rbx::Instance* inst) {
                        rep->instances.push_back(rbx::Ref<rbx::Instance>(inst));
                        rep->by_id[r] = int(rep->instances.size() - 1);
                    };
                    reg(mref, model.get());
                    reg(rref, hrp.get());
                    reg(href, hum.get());
                    self_char_model = model.get();
                    self_char_root = hrp.get();
                    self_char_ref = rref;
                    const uint32_t pref = ref_of(me);
                    const rbx::send::Api* api = rbx::send::installed();
                    if (pref && api && api->raw) {
                        // Announce what we built: the server must have a ref
                        // for our root before it will accept 0x83/03 writes
                        // against it. Parent the Model under our Player.
                        const std::pair<uint32_t, uint16_t> spawns[] = {
                            {mref, uint16_t(model_cls)},
                            {rref, uint16_t(part_cls)},
                            {href, uint16_t(hum_cls)},
                        };
                        for (const auto& sp : spawns) {
                            const uint32_t parent = sp.first == mref ? pref : mref;
                            const std::vector<uint8_t> m =
                                rbxclient::batch::client_spawn(
                                    sp.first, sp.second, parent);
                            api->raw(api->ctx, m.data(), m.size(), true);
                        }
                    }
                    char b[128];
                    snprintf(b, sizeof b,
                             "selfchar: local character built parent=0x%x "
                             "model=0x%x root=0x%x hum=0x%x",
                             ref_of(me), mref, rref, href);
                    log(b);
                }
            }
        }
        // Session clock: the reference's first upload sits at ~2730 ms, so the
        // clock is rebased here rather than counting from process start (join +
        // asset load would put it tens of seconds ahead of the session).
        // One-shot: this function can be retried on later chains.
        if (!net_clock_rebased) {
            net_clock_rebased = true;
            rbxclient::batch::rebase_net_clock(2730);
        }

    }

    // Our own Player's replication id, 0 until the world has one.
    uint32_t self_player_ref() {
        if (!rep || !rep->root) return 0;
        rbx::Instance* players =
            rep->root->find_first_child_of_class("Players");
        if (!players) return 0;
        rbx::Variant lp;
        if (!players->get_prop("LocalPlayer", lp)) return 0;
        const auto* me = std::get_if<rbx::Instance*>(&lp);
        if (!me || !*me) return 0;
        return ref_of(*me);
    }

    // --- executor world view -------------------------------------------
    rbx::Instance* local_character_inst() {
        if (!rep || !rep->root) return nullptr;
        rbx::Instance* rt = rep->root.get();
        rbx::Instance* players = rt->find_first_child_of_class("Players");
        if (!players) return nullptr;
        // Prefer the same handle Lua sees: Players.LocalPlayer.
        rbx::Variant lp;
        if (players->get_prop("LocalPlayer", lp)) {
            if (const auto* p = std::get_if<rbx::Instance*>(&lp); p && *p) {
                rbx::Variant ch;
                if ((*p)->get_prop("Character", ch)) {
                    if (const auto* c = std::get_if<rbx::Instance*>(&ch); c && *c)
                        return *c;
                }
                // The server clears Character on its owner (measured: tag 3
                // nil) even after the appearance signal. The model we built
                // is still parented to the Player, so keep using it instead
                // of dropping the physics path.
                if (self_char_model && self_char_model->parent() == *p)
                    return self_char_model;
                if (rbx::Instance* ws = rt->find_first_child_of_class("Workspace")) {
                    const std::string want = (*p)->name();
                    for (const auto& c : ws->children())
                        if (c->is_a("Model") && c->name() == want) return c.get();
                }
            }
        }
        const long long self = rbx::local_user_id();
        if (self == 0) return nullptr;
        for (const auto& child : players->children()) {
            rbx::Instance* player = child.get();
            if (!player->is_a("Player")) continue;
            rbx::Variant uid;
            if (!player->get_prop("UserId", uid)) continue;
            const auto* id = std::get_if<int64_t>(&uid);
            if (!id || *id != self) continue;
            rbx::Variant ch;
            if (player->get_prop("Character", ch)) {
                const auto* character = std::get_if<rbx::Instance*>(&ch);
                if (character && *character) return *character;
            }
            // The Character property is not always replicated; the model lands
            // in Workspace under the player's name.
            if (rbx::Instance* ws = rt->find_first_child_of_class("Workspace")) {
                const std::string want = player->name();
                for (const auto& c : ws->children())
                    if (c->is_a("Model") && c->name() == want) return c.get();
            }
            return nullptr;
        }
        return nullptr;
    }

    rbx::Instance* local_humanoid() {
        rbx::Instance* ch = local_character_inst();
        return ch ? ch->find_first_child_of_class("Humanoid") : nullptr;
    }

    uint32_t ref_of(rbx::Instance* inst) {
        if (!rep || !inst) return 0;
        for (const auto& kv : rep->by_id) {
            if (kv.second < 0) continue;
            if (rep->instances[size_t(kv.second)].get() == inst)
                return kv.first;
        }
        return 0;
    }

    int event_index(const char* cls, const char* ev) {
        if (!have_schema || !cls || !ev) return -1;
        int idx = 0;
        for (const auto& c : schema.classes) {
            for (const auto& e : c.events) {
                if (c.name == cls && e.name == ev) return idx;
                ++idx;
            }
        }
        return -1;
    }

    // Wire property id (the schema's global class/member concatenation
    // index), not the per-class delta. parsePropTriple resolves the u16 in a
    // 0x83/03 item through exactly this table.
    int member_index(const char* cls, const char* member) {
        if (!have_schema || !cls || !member) return -1;
        auto lookup = [&](const std::string& want, bool substring) -> int {
            for (const auto& kv : props.member_name) {
                const bool hit = substring
                                     ? kv.second.find(want) != std::string::npos
                                     : kv.second == want;
                if (!hit) continue;
                const auto oc = props.owner_class.find(kv.first);
                if (oc == props.owner_class.end()) continue;
                if (size_t(oc->second) >= schema.classes.size()) continue;
                if (schema.classes[oc->second].name == cls)
                    return int(kv.first);
            }
            return -1;
        };
        if (int p = lookup(member, false); p >= 0) return p;
        // Name drift: the wire table may call it MoveDirectionInternal or
        // similar. Fall back to a substring match so a rename does not
        // silently disable movement.
        if (int p = lookup("MoveDirection", true); p >= 0) return p;
        return lookup("Move", true);
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
        const auto it = rep->by_id.find(ps.script_id);
        if (it == rep->by_id.end() || it->second < 0) {
            ++scripts_skipped;
            return;
        }
        rbx::Instance* script = rep->instances[size_t(it->second)].get();
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
        lua_State* L = rbxch::engine::state(*env);
        rbx::Instance::push(L, script);
        lua_setglobal(L, "script");

        std::vector<uint8_t> code = ps.payload;
        // Production script blobs use the wire opcode numbering. Rewrite
        // them before luau_load, exactly like the challenge path. Unknown
        // headers are passed through so the loader can report them honestly.
        if (code.size() > 1 && code[0] >= 3 && code[0] <= 14) {
            rbxclient::Stats st;
            std::string serr;
            std::vector<uint8_t> standardized = code;
            if (rbxclient::standardize(standardized, &st, &serr))
                code = std::move(standardized);
        }
        rbxch::Result r = rbxch::engine::execute(*env, code.data(), code.size());
        if (r.status == rbxch::Status::Ok) {
            ++scripts_run;
        } else {
            ++scripts_failed;
            if (scripts_failed <= 8)
                log("script id=" + std::to_string(ps.script_id) +
                    " failed: " + r.error);
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
        if (!(dt > 0.0))
            dt = 0.0;
        dt = std::min(dt, 0.25);
        rbxch::engine::step(*env, dt);
    }
};

LiveRuntime::LiveRuntime(std::string job_id, LogFn log, bool run_scripts,
                         long long place_id, long long universe_id)
    : impl_(std::make_unique<Impl>(std::move(job_id), std::move(log))) {
    impl_->run_scripts = run_scripts;
    impl_->place_id = place_id;
    impl_->universe_id = universe_id;
}

LiveRuntime::~LiveRuntime() = default;

void LiveRuntime::feed(const uint8_t* data, size_t len) {
    impl_->feed(data, len);
}

void LiveRuntime::feedMessage(const uint8_t* msg, size_t len) {
    impl_->feedMessage(msg, len);
}

void LiveRuntime::set_exec_source(std::string source) {
    impl_->exec_source = std::move(source);
}

void LiveRuntime::pump(double dt) {
    impl_->pump(dt);
    // --- own-character physics upload (0x83/0x05) ---------------------
    // --- periodic ready beacon (0x83/04) --------------------------------
    // The reference repeats this 12-byte message all session, interleaved with
    // 0x23 and 0x05 (measured rc8: 04 -> 23 -> 05 -> 05 -> 23 -> 04 ...,
    // ~1 per 12 s). We sent it once, after the world.
    if (impl_->env && impl_->rep && impl_->rep->root) {
        // Retry the self-character build: at world-ready the Players tree may
        // not have LocalPlayer yet, which was why it silently did nothing.
        impl_->build_self_character();
        impl_->ready_accum += dt;
        if (impl_->ready_accum >= 12.0) {
            impl_->ready_accum = 0.0;
            const std::vector<uint8_t> rb = rbxclient::batch::ready_batch(
                rbxclient::batch::net_clock_ms());
            if (const rbx::send::Api* api = rbx::send::installed())
                if (api->raw) api->raw(api->ctx, rb.data(), rb.size(), false);
        }
    }

    // --- record acknowledgements (0x83/06) ------------------------------
    // One ack per applied record, bounded rate. Native: ~2/s with count=1.
    if (impl_->env && impl_->rep && impl_->rep->root &&
        !impl_->ack_queue.empty()) {
        impl_->ack_accum += dt;
        // Native: 10 x 0x83/06 in 4.8 s = ~2/s, one record each. Sending more
        // than that floods the session (measured: 6192 outbound 0x83 in 50 s
        // vs the native's 226).
        const double period = getenv("RBX_ACK_MS")
                                  ? atof(getenv("RBX_ACK_MS")) / 1000.0
                                  : 0.5;
        if (impl_->ack_accum >= period) {
            impl_->ack_accum = 0.0;
            const rbx::send::Api* api = rbx::send::installed();
            if (api && api->raw) {
                size_t n = std::min<size_t>(impl_->ack_queue.size(), 1);
                for (size_t i = 0; i < n; ++i) {
                    const uint32_t r = impl_->ack_queue[i];
                    const std::vector<uint8_t> a =
                        rbxclient::batch::physics06_batch(r);
                    api->raw(api->ctx, a.data(), a.size(), false);
                }
                impl_->ack_queue.erase(impl_->ack_queue.begin(),
                                       impl_->ack_queue.begin() + n);
            }
        }
    }
    // RBX_TP_ONLY=1 keeps the CFrame write path alive while physics is off,
    // so the two ways of moving a body can be tested independently.
    const bool phys_on = getenv("BOBLOX_NO_PHYS") == nullptr;
    if (impl_->env && impl_->rep && impl_->rep->root &&
        (phys_on || getenv("RBX_TP_ONLY") != nullptr)) {
        float tp[3];
        while (rbx::exec::take_teleport(tp)) {
            rbx::Instance* ch = impl_->local_character_inst();
            rbx::Instance* root = ch ? ch->find_first_child_of_class("HumanoidRootPart") : nullptr;
            const uint32_t ref = root ? impl_->ref_of(root) : 0;
            const int prop = root ? impl_->member_index(root->class_name().c_str(), "CFrame") : -1;
            if (ref && prop >= 0) {
                const std::vector<uint8_t> b = rbxclient::batch::cframe_batch(
                    ref, uint16_t(prop), tp[0], tp[1], tp[2]);
                if (const rbx::send::Api* api = rbx::send::installed())
                    if (api->raw) api->raw(api->ctx, b.data(), b.size(), true);
                impl_->phys_pos[0] = tp[0]; impl_->phys_pos[1] = tp[1];
                impl_->phys_pos[2] = tp[2];
                impl_->phys_init = true;
            }
        }
        float d[3];
        while (rbx::exec::take_move(d)) {
            impl_->move_dir[0] = d[0];
            impl_->move_dir[1] = d[1];
            impl_->move_dir[2] = d[2];
        }
        // No replicated character yet: replay the last script position at 2 Hz
        // (the reference's upload cadence) so the RCC has a sustained claim to
        // adopt. A one-shot burst at join time is what made spawns flaky.
        if (impl_->have_nudge && !impl_->phys_init) {
            // The server never replicates our own Model, so there is no local
            // root to integrate. Run the same integrator on the claimed
            // position instead: the transform carries no instance refs, so a
            // moving claim is still expressible without a body.
            const float n = std::sqrt(impl_->move_dir[0] * impl_->move_dir[0] +
                                      impl_->move_dir[1] * impl_->move_dir[1] +
                                      impl_->move_dir[2] * impl_->move_dir[2]);
            const float dtf = float(dt);
            if (n > 1e-4f) {
                impl_->nudge_vel[0] =
                    impl_->move_dir[0] / n * float(impl_->phys_speed);
                impl_->nudge_vel[2] =
                    impl_->move_dir[2] / n * float(impl_->phys_speed);
            } else {
                impl_->nudge_vel[0] = 0.0f;
                impl_->nudge_vel[2] = 0.0f;
            }
            impl_->nudge_pos[0] += impl_->nudge_vel[0] * dtf;
            impl_->nudge_pos[2] += impl_->nudge_vel[2] * dtf;
            const float hspeed =
                std::sqrt(impl_->nudge_vel[0] * impl_->nudge_vel[0] +
                          impl_->nudge_vel[2] * impl_->nudge_vel[2]);
            impl_->nudge_accum += dt;
            if (impl_->nudge_accum >= 0.5) {
                impl_->nudge_accum = 0.0;
                // RBX_SPAWNTRACK=1: re-read the map's SpawnLocation at send
                // time. A round change replaces the map, so a position seeded
                // once at world-ready is stale and the server clamps it.
                if (getenv("RBX_SPAWNTRACK") && impl_->rep && impl_->rep->root) {
                    rbx::Instance* ws = impl_->rep->root
                                            ->find_first_child_of_class("Workspace");
                    std::function<rbx::Instance*(rbx::Instance*, int)> find =
                        [&](rbx::Instance* n, int depth) -> rbx::Instance* {
                        if (!n || depth > 6) return nullptr;
                        if (n->is_a("SpawnLocation")) return n;
                        for (const auto& c : n->children())
                            if (rbx::Instance* hit = find(c.get(), depth + 1))
                                return hit;
                        return nullptr;
                    };
                    if (rbx::Instance* sp = find(ws, 0)) {
                        rbx::Variant sv;
                        if (sp->get_prop("CFrame", sv))
                            if (const auto* scf = std::get_if<rbx::CFrame>(&sv)) {
                                impl_->nudge_pos[0] = scf->x;
                                impl_->nudge_pos[1] = scf->y + 3.0f;
                                impl_->nudge_pos[2] = scf->z;
                            }
                    }
                }
                const uint32_t clk = rbxclient::batch::net_clock_ms();
                const std::vector<uint8_t> b =
                    rbxclient::batch::physics_batch_clocked(
                        clk, clk, clk + 3, impl_->nudge_pos[0],
                        impl_->nudge_pos[1], impl_->nudge_pos[2], 0u,
                        hspeed * 64.0f);
                // The reference sends this transform both ways (measured
                // rc8: 25 x rel 0x60, 18 x rel 0x00). Only the reliable copy
                // can be retransmitted, and only the unreliable one is cheap.
                if (const rbx::send::Api* api = rbx::send::installed())
                    if (api->raw) {
                        api->raw(api->ctx, b.data(), b.size(), true);
                        api->raw(api->ctx, b.data(), b.size(), false);
                    }
            }
        }
        if (!phys_on) return;
        impl_->phys_accum += dt;
        if (impl_->phys_accum >= 0.1) {
            const double step = impl_->phys_accum;
            impl_->phys_accum = 0.0;
            rbx::Instance* ch = impl_->local_character_inst();
            rbx::Instance* root = ch ? ch->find_first_child_of_class("HumanoidRootPart") : nullptr;
            if (!root && ch) root = ch->find_first_child_of_class("Part");
            rbx::Instance* hum = ch ? ch->find_first_child_of_class("Humanoid") : nullptr;
            // The capture pairs the Humanoid (main ref) with the root part
            // (trailing ref); sending the same ref twice is not the same thing.
            const uint32_t ref = hum ? impl_->ref_of(hum) : 0;
            const uint32_t tail = root ? impl_->ref_of(root) : 0;
            if (ref && tail) {
                if (!impl_->phys_init) {
                    // The body is placed by the server at spawn; start from
                    // THERE. Defaulting to (0,0,0) put the character under the
                    // map, which is why observers saw nothing.
                    rbx::Variant v;
                    if (!root || !root->get_prop("CFrame", v)) return;
                    const auto* cf = std::get_if<rbx::CFrame>(&v);
                    if (!cf) return;
                    float sx = cf->x, sy = cf->y, sz = cf->z;
                    // Ownership exclusion: the RCC often does not replicate our
                    // own root part's CFrame, so it reads 0,0,0. Standing on
                    // that made the whole upload path unreachable (no [phys],
                    // no 0x05) and left the body to server physics — which is
                    // why an observer could tip it over with a push. Fall back
                    // to the map's SpawnLocation, ordinary replicated geometry.
                    if (std::fabs(sx) < 0.01f && std::fabs(sy) < 0.01f &&
                        std::fabs(sz) < 0.01f) {
                        rbx::Instance* ws = impl_->rep->root
                                                ? impl_->rep->root
                                                      ->find_first_child_of_class(
                                                          "Workspace")
                                                : nullptr;
                        std::function<rbx::Instance*(rbx::Instance*, int)> find =
                            [&](rbx::Instance* n, int depth) -> rbx::Instance* {
                            if (!n || depth > 6) return nullptr;
                            if (n->is_a("SpawnLocation")) return n;
                            for (const auto& c : n->children())
                                if (rbx::Instance* hit = find(c.get(), depth + 1))
                                    return hit;
                            return nullptr;
                        };
                        rbx::Instance* sp = find(ws, 0);
                        rbx::Variant sv;
                        if (sp && sp->get_prop("CFrame", sv)) {
                            if (const auto* scf = std::get_if<rbx::CFrame>(&sv)) {
                                sx = scf->x;
                                sy = scf->y + 3.0f;
                                sz = scf->z;
                                std::fprintf(stderr,
                                             "[phys] root cframe zero, seeded "
                                             "from SpawnLocation\n");
                            }
                        }
                        if (std::fabs(sx) < 0.01f && std::fabs(sy) < 0.01f &&
                            std::fabs(sz) < 0.01f)
                            return; // no usable spawn position yet
                    }
                    impl_->phys_pos[0] = sx;
                    impl_->phys_pos[1] = sy;
                    impl_->phys_pos[2] = sz;
                    impl_->phys_init = true;
                    impl_->ground_y = sy;
                    impl_->phys_vel[1] = 0.0f;
                    std::fprintf(stderr, "[phys] start pos %.1f %.1f %.1f\n",
                                 sx, sy, sz);
                }
                // Client-side physics: the owning client integrates its own
                // body. Direction sets horizontal velocity immediately, gravity
                // pulls down, and the ground clamp keeps us at spawn height.
                const float n = std::sqrt(impl_->move_dir[0] * impl_->move_dir[0] +
                                          impl_->move_dir[1] * impl_->move_dir[1] +
                                          impl_->move_dir[2] * impl_->move_dir[2]);
                const float dtf = float(step);
                if (n > 1e-4f) {
                    impl_->phys_vel[0] = impl_->move_dir[0] / n * float(impl_->phys_speed);
                    impl_->phys_vel[2] = impl_->move_dir[2] / n * float(impl_->phys_speed);
                } else {
                    impl_->phys_vel[0] = 0.0f;
                    impl_->phys_vel[2] = 0.0f;
                }
                impl_->phys_vel[1] -= 196.2f * dtf; // Roblox gravity
                impl_->phys_pos[0] += impl_->phys_vel[0] * dtf;
                impl_->phys_pos[1] += impl_->phys_vel[1] * dtf;
                impl_->phys_pos[2] += impl_->phys_vel[2] * dtf;
                if (impl_->phys_pos[1] <= impl_->ground_y) {
                    impl_->phys_pos[1] = impl_->ground_y;
                    impl_->phys_vel[1] = 0.0f;
                    impl_->grounded = true;
                }
                // Apply to our own tree so scripts (and our uploads) see it.
                rbx::Variant cv;
                if (root->get_prop("CFrame", cv)) {
                    if (auto* lc = std::get_if<rbx::CFrame>(&cv)) {
                        lc->x = impl_->phys_pos[0];
                        lc->y = impl_->phys_pos[1];
                        lc->z = impl_->phys_pos[2];
                        root->set_prop("CFrame", cv, nullptr);
                    }
                }
                // Clocked layout, not the reference layout: a, b and c are
                // clocks in the reference's own domain (see entity_batch.h).
                // Seeding this from a constant made it foreign to the server:
                // the reference's clock is milliseconds of client uptime.
                impl_->send_accum += step;
                const double send_period =
                    getenv("RBX_SEND_MS")
                        ? atof(getenv("RBX_SEND_MS")) / 1000.0
                        : 0.7;
                if (impl_->send_accum >= send_period) {
                    impl_->send_accum = 0.0;
                    // RBX_CLK_SRV=1: stamp the upload with the server's own
                    // clock (learned from its 0x83/05 heartbeat) instead of our
                    // session clock. The two domains differ by ~28M here.
                    if (const char* bf = getenv("RBX_BODY_FILE")) {
                        if (FILE* f = fopen(bf, "r")) {
                            float bx, by, bz;
                            if (fscanf(f, "%f %f %f", &bx, &by, &bz) == 3) {
                                static int adopted = 0;
                                if (adopted < 3) {
                                    ++adopted;
                                    impl_->log("[bodyfile] adopted " +
                                        std::to_string(bx) + " " +
                                        std::to_string(by) + " " +
                                        std::to_string(bz));
                                }
                                impl_->phys_pos[0] = bx;
                                impl_->phys_pos[1] = by;
                                impl_->phys_pos[2] = bz;
                                impl_->nudge_pos[0] = bx;
                                impl_->nudge_pos[1] = by;
                                impl_->nudge_pos[2] = bz;
                                impl_->phys_init = true;
                                impl_->ground_y = by;
                            }
                            fclose(f);
                        }
                    }
                    const bool clk_srv = getenv("RBX_CLK_SRV") != nullptr &&
                                         impl_->srv_clock_ms != 0;
                    uint32_t clk = rbxclient::batch::net_clock_ms();
                    if (clk_srv) {
                        const auto since = std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() -
                            impl_->srv_clock_at);
                        clk = impl_->srv_clock_ms + uint32_t(since.count());
                    }
                    const float ground_speed =
                        std::sqrt(impl_->phys_vel[0] * impl_->phys_vel[0] +
                                  impl_->phys_vel[2] * impl_->phys_vel[2]);
                    // The trailing float tracks horizontal speed at 64x: measured
                    // 692 -> 1030 while walking, i.e. 10.8 -> 16.1 studs/s.
                    const uint32_t srv_clock = [&] {
                        if (!impl_->srv_clock_ms) return 0u;
                        const auto since = std::chrono::duration_cast<
                            std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                       impl_->srv_clock_at);
                        return impl_->srv_clock_ms + uint32_t(since.count());
                    }();
                    // Grounded bit: the reference alternates this field
                    // between 0 and 0xffffffff as it jumps and lands; a
                    // permanent 0xffffffff read as airborne.
                    const uint32_t gflags =
                        impl_->grounded ? 0u : 0xffffffffu;
                    // RBX_PHYS_ITEMS selects how much rides with the
                    // transform, for bisecting server-side stream errors:
                    //   "05"    transform only
                    //   "0506"  transform + claim records (one chain)
                    //   default transform + claims + ownership (one chain)
                    const char* items = getenv("RBX_PHYS_ITEMS");
                    const bool only05 = items && std::strcmp(items, "05") == 0;
                    // Never claim refs the server did not issue: a 0x0a item
                    // naming a synthetic ref makes the RCC reject the WHOLE
                    // chain with 0x0104, which silently killed every transform
                    // we ever sent (measured: 71-byte upload = 45B transform +
                    // two claims naming 0x00b00001/2).
                    const bool refs_known = !impl_->self_char_ref;
                    const bool with_claims =
                        refs_known &&
                        !(items && (std::strcmp(items, "0506") == 0 ||
                                    std::strcmp(items, "05") == 0));
                    const std::vector<uint8_t> five =
                        rbxclient::batch::physics_batch_clocked(
                            clk, clk, clk + 3, impl_->phys_pos[0],
                            impl_->phys_pos[1], impl_->phys_pos[2], gflags,
                            ground_speed * 64.0f);
                    const std::vector<uint8_t> chain =
                        rbxclient::batch::physics_chain(
                            ref, tail, srv_clock, clk, impl_->phys_pos[0],
                            impl_->phys_pos[1], impl_->phys_pos[2], gflags,
                            ground_speed * 64.0f, with_claims);
                    // RBX_REPLAY05=<hex>: send a captured native upload
                    // verbatim. If the RCC applies it, our sender is fine and
                    // the difference is field values; if not, it is session
                    // authority and no byte shaping can help.
                    std::vector<uint8_t> replay;
                    if (const char* rh = getenv("RBX_REPLAY05")) {
                        static const char* kH = "0123456789abcdef";
                        for (const char* p2 = rh; p2[0] && p2[1]; p2 += 2) {
                            auto v = [](char c) -> int {
                                if (c >= '0' && c <= '9') return c - '0';
                                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                                return -1;
                            };
                            const int hi = v(p2[0]), lo = v(p2[1]);
                            if (hi < 0 || lo < 0) break;
                            replay.push_back(uint8_t(hi * 16 + lo));
                        }
                    }
                    const std::vector<uint8_t>& send =
                        !replay.empty() ? replay : (only05 ? five : chain);
                    // Match the reference: the transform goes out both
                    // unreliable and reliable-ordered (measured 25 x rel 0x60,
                    // 18 x rel 0x00). Only the unreliable copy has ever been
                    // sent by this client.
                    if (const rbx::send::Api* api = rbx::send::installed())
                        if (api->raw) {
                            api->raw(api->ctx, send.data(), send.size(), false);
                            api->raw(api->ctx, send.data(), send.size(), true);
                        }
                    // Property-write transform for the client-authored root:
                    // the one form that addresses a ref the server knows.
                    // CFrame write to the refs the RCC assigned to us. These
                    // are our own body parts: never replicated, but named in
                    // NetworkOwnerV3, so a property write can address them.
                    if (getenv("RBX_OWNERCF") && impl_->rep) {
                        const int pp = impl_->member_index("Part", "CFrame");
                        const int mp = impl_->member_index("MeshPart", "CFrame");
                        if (pp >= 0 || mp >= 0) {
                            const rbx::send::Api* api =
                                rbx::send::installed();
                            int n = 0;
                            for (const auto& kv : impl_->rep->by_id) {
                                (void)kv;
                            }
                            for (uint32_t orf : impl_->owner_refs) {
                                if (n++ >= 4) break;
                                // The owner ref's class is unknown; Part and
                                // MeshPart carry different CFrame prop ids, so
                                // write both forms.
                                for (int pid : {pp, mp}) {
                                    if (pid < 0) continue;
                                    const std::vector<uint8_t> cf =
                                        rbxclient::batch::cframe_batch(
                                            orf, uint16_t(pid),
                                            impl_->phys_pos[0],
                                            impl_->phys_pos[1],
                                            impl_->phys_pos[2]);
                                    if (api && api->raw)
                                        api->raw(api->ctx, cf.data(), cf.size(),
                                                 true);
                                }
                            }
                        }
                    }
                    if (impl_->self_char_ref &&
                        !getenv("RBX_NO_SELFCF")) {
                        const int prop = impl_->member_index("Part", "CFrame");
                        if (prop >= 0) {
                            const std::vector<uint8_t> cf =
                                rbxclient::batch::cframe_batch(
                                    impl_->self_char_ref, uint16_t(prop),
                                    impl_->phys_pos[0], impl_->phys_pos[1],
                                    impl_->phys_pos[2]);
                            if (const rbx::send::Api* api =
                                    rbx::send::installed())
                                if (api->raw)
                                    api->raw(api->ctx, cf.data(), cf.size(),
                                             true);
                        }
                    }
                }
            }
        }
    }

    // 0x83/0x0a control/relevance report. Off by default: on this build our
    // 16-byte item of the captured shape earns 0x15/0105 (packet stream error),
    // so it must not run unless explicitly asked for.
    //   BOBLOX_INTEREST=1        send continuously at ~30 Hz
    //   BOBLOX_INTEREST_ONCE=1   send exactly one item, once, after world ready
    const bool interest_on = getenv("BOBLOX_INTEREST") != nullptr;
    const bool interest_once = getenv("BOBLOX_INTEREST_ONCE") != nullptr;
    if (impl_->env && impl_->rep && impl_->rep->root &&
        (interest_on || interest_once) &&
        !(interest_once && impl_->interest_sent_once)) {
        impl_->interest_accum += dt;
        const double iv = interest_once ? 1.0
            : (getenv("BOBLOX_INTEREST_MS")
                   ? atof(getenv("BOBLOX_INTEREST_MS")) / 1000.0 : 0.033);
        if (impl_->interest_accum >= iv) {
            impl_->interest_accum = 0.0;
            rbx::Instance* rt = impl_->rep->root.get();
            rbx::Instance* players = rt->find_first_child_of_class("Players");
            rbx::Variant lp;
            rbx::Instance* me = nullptr;
            if (players && players->get_prop("LocalPlayer", lp))
                if (const auto* pp = std::get_if<rbx::Instance*>(&lp)) me = *pp;
            const uint32_t ref = me ? impl_->ref_of(me) : 0;
            if (ref) {
                const std::vector<uint8_t> b =
                    rbxclient::batch::interest_batch(ref, impl_->interest_clock);
                impl_->interest_clock += 0x38;
                if (const rbx::send::Api* api = rbx::send::installed())
                    if (api->raw) api->raw(api->ctx, b.data(), b.size(), true);
                if (interest_once) impl_->interest_sent_once = true;
            }
        }
    }
    // Scripts pushed through $BOBLOX_SOCKET run here, on the Lua thread.
    if (impl_->env) rbx::exec::pump_pending(rbxch::engine::state(*impl_->env));
}

uint32_t LiveRuntime::local_player_ref() const {
    if (!impl_->rep || !impl_->rep->root) return 0;
    rbx::Instance* players =
        impl_->rep->root->find_first_child_of_class("Players");
    if (!players) return 0;
    rbx::Variant lp;
    if (!players->get_prop("LocalPlayer", lp)) return 0;
    const auto* me = std::get_if<rbx::Instance*>(&lp);
    if (!me || !*me) return 0;
    return impl_->ref_of(*me);
}

bool LiveRuntime::has_world() const {
    return impl_->rep && impl_->rep->root;
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
                      " events=" + std::to_string(s.sst.events_logged) +
                      " fired=" + std::to_string(s.sst.events_fired) +
                      " bulks=" + std::to_string(s.sst.bulks) +
                      " regions=" + std::to_string(s.regions) +
                      " a1=" + std::to_string(s.a1_messages) + "/" +
                      std::to_string(s.a1_records) +
                      " a1_first=" + census(s.a1_first_class) +
                      " pairs=" + std::to_string(s.instance_pair_messages) +
                      "/" + std::to_string(s.instance_pair_records) +
                      " scripts_skip=" + std::to_string(s.scripts_skipped) +
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
