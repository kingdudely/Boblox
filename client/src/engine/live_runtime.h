// live_runtime.h — live replication -> Engine DataModel, no window.
//
// The transport feeds raw app=4/chan=1 bytes here as they arrive. This class
// owns the varint framing, the 0x97 JoinData schema, snapshot accumulation,
// incremental 0x83 deltas (spawns, 0x03 property triples, 0x0B/0x25 entry
// batches, 0x0D entries, events, script blobs) and the persistent Engine
// environment that runs the resulting world.
//
// It is deliberately independent of QUIC/session/viewer: headless and headful
// runs share the same world; only window presentation is different.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rbx {
class Instance;
}

namespace rbxclient {

class LiveRuntime {
  public:
    using LogFn = std::function<void(const std::string&)>;

    // run_scripts=false keeps the replicated world and the Engine scheduler
    // but never loads a replicated script chunk (--no-scripts: the largest
    // single RAM saving for a client that only needs to render the world).
    // gui=true installs PlayerGui/CurrentCamera (--gui, default); Font is
    // always present. It is independent of the renderer.
    LiveRuntime(std::string job_id, LogFn log, bool run_scripts = true,
                bool gui = true);
    ~LiveRuntime();
    LiveRuntime(const LiveRuntime&) = delete;
    LiveRuntime& operator=(const LiveRuntime&) = delete;

    // Append transport bytes. Complete compact-varint frames are consumed as
    // they become available; an incomplete tail waits for the next feed.
    void feed(const uint8_t* data, size_t len);

    // The authenticated account (join script UserId/UserName/DisplayName).
    // The native client creates Players.LocalPlayer itself at join, so this
    // installs the same placeholder (and PlayerGui) before any script runs;
    // a replicated Player record for the same UserId fills that instance in
    // instead of spawning a twin. Call before the first feedMessage.
    void setLocalUser(long long user_id, std::string user_name,
                      std::string display_name);

    // Feed one complete session message (payload = id + body), i.e. a RakNet
    // application payload. Equivalent to writing compactVarint(len) + payload
    // into feed(): RakNet's reliability layer is framing, not a second
    // protocol, so both transports end up in the same parser.
    void feedMessage(const uint8_t* msg, size_t len);

    // Advance the Engine scheduler (task.wait/delay/events). Safe before the
    // snapshot exists; becomes a no-op until the Engine environment is live.
    void pump(double dt);

    // User script to run through the executor surface once the Engine
    // environment exists (after attach). Empty = no user script. The install
    // itself always happens, so scripts sent later see the UNC surface.
    void set_exec_source(std::string source);

    bool has_world() const;
    // Join progress for the session watchdog. 0x97 (join data) is the RCC's
    // answer to a placed join; the snapshot batches that follow are what
    // actually build the world. A session with join data but no progress for
    // long enough is a failed join and must be redone, not waited on.
    bool join_data_seen() const;
    long long ms_since_join_data() const;
    long long ms_since_progress() const;
    // Applied DataModel root (null until the snapshot is finalized). Owned
    // by the Engine environment; valid until this LiveRuntime is destroyed.
    const rbx::Instance* root() const;
    // True when the applied world contains an instance with this replication
    // id. Client->server frames that embed a capture's instance ids must ask
    // this first: replaying an id from another job hands the RCC a dangling
    // reference and it answers with 0x15 01 04 (receive packet error).
    bool hasInstance(uint32_t id) const;
    // Name of the local player's linked Workspace character, or "" until the
    // RCC replicates the avatar. Uses rbx::local_user_id(); with no account id
    // installed it returns "" rather than guessing another player's model.
    std::string local_character() const;
    // Replication id of our own Player instance, 0 until the world has one.
    // Needed by client-authored records that address the local player (the
    // 0x83/04 hydration event carries this ref; a capture's ref is another
    // session's and is rejected).
    uint32_t local_player_ref() const;

    // The local character's parts with the RCC's own replication refs and
    // their current CFrame. This is what the client-owned 0x83 records must
    // address: the native's frames carry live refs, and replaying a ref from
    // another session hands the RCC an instance that does not exist (its
    // 0x0105 receive error). Empty until the avatar is replicated.
    struct LocalPart {
        uint32_t ref = 0;   // network id (0 = not replicated)
        std::string name;
        std::string cls;
        bool has_cframe = false;
        double x = 0.0, y = 0.0, z = 0.0;
    };
    std::vector<LocalPart> local_character_parts() const;
    // Ref of an instance whose server state we most recently applied, in
    // arrival order, or false when the queue is empty. This is what the
    // client->server 0x83/0a ack names: it says "I applied your update for
    // this instance up to <clock>", so it has to name the instance the server
    // just sent, not one we picked ourselves.
    bool takeAppliedRef(uint32_t* out);
    bool failed() const;
    const std::string& error() const;

    int snapshot_records() const;
    int live_messages() const;
    int scripts_run() const;
    int scripts_failed() const;
    // One-line coverage counters (parsed items, stops, prop updates, bulks).
    std::string stats() const;
    // Top classes in the applied world (diagnostic: is a Player/Humanoid
    // actually present, or is the tree only static map instances?).
    std::string class_stats() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rbxclient
