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

namespace rbx {
class Instance;
}

namespace rbxclient {

class LiveRuntime {
  public:
    using LogFn = std::function<void(const std::string&)>;

    LiveRuntime(std::string job_id, LogFn log);
    ~LiveRuntime();
    LiveRuntime(const LiveRuntime&) = delete;
    LiveRuntime& operator=(const LiveRuntime&) = delete;

    // Append transport bytes. Complete compact-varint frames are consumed as
    // they become available; an incomplete tail waits for the next feed.
    void feed(const uint8_t* data, size_t len);

    // Feed one complete session message (payload = id + body), i.e. a RakNet
    // application payload. Equivalent to writing compactVarint(len) + payload
    // into feed(): RakNet's reliability layer is framing, not a second
    // protocol, so both transports end up in the same parser.
    void feedMessage(const uint8_t* msg, size_t len);

    // Advance the Engine scheduler (task.wait/delay/events). Safe before the
    // snapshot exists; becomes a no-op until the Engine environment is live.
    void pump(double dt);

    bool has_world() const;
    // Applied DataModel root (null until the snapshot is finalized). Owned
    // by the Engine environment; valid until this LiveRuntime is destroyed.
    const rbx::Instance* root() const;
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
