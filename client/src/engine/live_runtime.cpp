// live_runtime.cpp — see live_runtime.h.
#include "engine/live_runtime.h"

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
        case 0x81:
            onPeer(body, len);
            break;
        case 0x8D:
            onRegion(body, len);
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
        } else if (flags != 0) {
            ++unknown_batch_flags;
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
        Walk83 w;
        std::string err;
        if (!walk83ItemsEx(body, len, schema, &props, w, &err)) {
            ++live_failures;
            if (live_failures <= 8)
                log("0x83 walk failed: " + err);
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
                log(std::string(b) + " bytes=" + tail);
            }
        }
        ++live_msgs;
        uint32_t last_spawn = 0;
        bool have_last_spawn = false;
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
        const auto name = props.member_name.find(p.prop);
        const auto cls = props.owner_class.find(p.prop);
        if (name == props.member_name.end() || cls == props.owner_class.end()) {
            ++sst.prop_missing;
            return;
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
        opt.profile = rbxch::Profile::Engine;
        env = rbxch::engine::create(opt);
        if (!env) {
            fail("engine::create failed");
            return;
        }
        rbxch::engine::attach(*env, rep->root.get());
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

LiveRuntime::LiveRuntime(std::string job_id, LogFn log, bool run_scripts)
    : impl_(std::make_unique<Impl>(std::move(job_id), std::move(log))) {
    impl_->run_scripts = run_scripts;
}

LiveRuntime::~LiveRuntime() = default;

void LiveRuntime::feed(const uint8_t* data, size_t len) {
    impl_->feed(data, len);
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
