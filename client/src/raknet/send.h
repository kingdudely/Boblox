// send.h — the upstream (client -> RCC) path, as one module.
//
// The transport has been able to send since the handshake was cracked
// (sendUnreliable / sendJoinMessage); what was missing is the *stream*: the
// native's 13.3 Hz 0xA6 route cadence, step continuity, and a way to put a
// ControlState block on a frame without touching the call site.
//
// This module owns that stream and nothing else:
//   * route frames at the native cadence, stepIds +4 per message
//   * an optional ControlState block, either sustained (hold) or one-shot
//   * a placement knob (last sub-frame, or every sub-frame — the open
//     hypothesis from the 2026-10-12 sweeps)
//   * raw upstream messages for probes and for the executor surface
//
// What it deliberately does NOT claim: the slot -> action mapping. The bool
// sweep in FINDINGS closed slots 0-7 without a spawn, so `hold(mask, values)`
// takes an explicit mask instead of inventing "forward". Recovering that
// mapping is a live-capture job, not a code guess.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rbxclient {

// The two RakNet reliability shapes the game connection uses.
class UpstreamSender {
  public:
    virtual ~UpstreamSender() = default;
    virtual bool send_unreliable(const uint8_t* data, size_t len) = 0;
    virtual bool send_ordered(const uint8_t* data, size_t len) = 0;
};

struct SendConfig {
    int input_ms = 75;      // native: 13.3 Hz (34.4 s capture, /tmp/nat_live)
    uint32_t step0 = 0x14c; // native's first route step
    bool route_stream = true;
};

class SendPath {
  public:
    enum class Placement {
        LastFrame,   // replace the final nActions byte (2026-10-12 cb0 shape)
        EveryFrame,  // every sub-frame carries the block (open hypothesis)
    };

    using LogFn = std::function<void(const std::string&)>;

    SendPath(UpstreamSender* tx, SendConfig cfg, LogFn log);

    // Emit at most one route frame if the cadence is due. Returns true when a
    // datagram was handed to the transport.
    bool tick(std::chrono::steady_clock::time_point now);

    // Sustained ControlState. mask==0 clears. Takes effect on the next frame
    // and stays until release() (the native holds keys for a sustained press).
    bool hold(uint8_t mask, uint8_t values);
    bool release();

    // One-shot block on the next route frame only.
    bool queue_control(uint8_t mask, uint8_t values);

    // Raw application message (id + body), no framing added.
    bool raw(const uint8_t* data, size_t len, bool ordered);

    // Raw ControlState bytes, replacing the generated block (probe escape
    // hatch: lets a live sweep try a shape this builder does not model).
    void set_raw_control(std::vector<uint8_t> block);
    void clear_raw_control();

    void set_placement(Placement p) { placement_ = p; }
    void set_route_stream(bool on) { cfg_.route_stream = on; }
    void set_cadence_ms(int ms) { cfg_.input_ms = ms < 10 ? 10 : ms; }

    uint64_t frames_sent() const { return frames_sent_; }
    uint64_t controls_sent() const { return controls_sent_; }
    uint64_t raw_sent() const { return raw_sent_; }
    uint64_t send_failures() const { return send_failures_; }
    uint32_t step() const { return step_; }
    bool held() const { return hold_mask_ != 0; }
    uint8_t hold_mask() const { return hold_mask_; }

    // Byte-exact preview of the control block the next frame would carry
    // (empty when nothing is queued). Used by the unit tests and the sweep
    // log, so the wire shape is visible without a packet capture.
    std::vector<uint8_t> next_control_block() const;

  private:
    std::vector<uint8_t> build_next_frame();
    std::vector<uint8_t> take_control_block();

    UpstreamSender* tx_;
    SendConfig cfg_;
    LogFn log_;

    std::chrono::steady_clock::time_point next_at_{};
    bool started_ = false;

    uint32_t step_;
    int frame_index_ = 0;
    uint64_t frames_sent_ = 0;
    uint64_t controls_sent_ = 0;
    uint64_t raw_sent_ = 0;
    uint64_t send_failures_ = 0;

    uint8_t hold_mask_ = 0;
    uint8_t hold_values_ = 0;
    uint8_t pending_mask_ = 0;
    uint8_t pending_values_ = 0;
    bool pending_ = false;
    std::vector<uint8_t> raw_control_;
    Placement placement_ = Placement::LastFrame;
};

} // namespace rbxclient
