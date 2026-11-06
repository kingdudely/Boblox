// send.cpp — see send.h.
#include "raknet/send.h"

#include "raknet/input.h"

namespace rbxclient {

namespace {

void put_step_pair(std::vector<uint8_t>* out, uint32_t step) {
    for (int k = 0; k < 4; ++k) out->push_back(uint8_t(step >> (8 * k)));
    for (int k = 0; k < 4; ++k) out->push_back(uint8_t(step >> (8 * k)));
}

// The native frame, assembled from the same layout input.cpp pins:
//   a6 <count> 01 00 <blob=01> <stepA> <stepB> <cond=00>
//   <nActions>            frame 0
//   (04 04 <nActions>)*   frames 1..count-1
// `block` replaces the nActions byte of the frame(s) the placement selects.
// With an empty block this is byte-identical to input::buildFrame(step,count,
// {},false) — unit_send pins that.
std::vector<uint8_t> assemble(uint32_t step, int count,
                              const std::vector<uint8_t>& block,
                              bool every_frame, bool camera) {
    if (count < 1) count = 1;
    if (count > 4) count = 4;
    std::vector<uint8_t> out;
    out.reserve(48);
    out.push_back(0xA6);
    out.push_back(uint8_t(count));
    out.push_back(0x01);
    out.push_back(0x00);
    if (camera)
        out.insert(out.end(), input::kCameraBlob,
                   input::kCameraBlob + sizeof input::kCameraBlob);
    else
        out.push_back(0x01); // short blob
    put_step_pair(&out, step);
    out.push_back(0x00); // cond
    for (int i = 0; i < count; ++i) {
        if (i > 0) {
            out.push_back(0x04);
            out.push_back(0x04);
        }
        const bool place = !block.empty() && (every_frame || i == count - 1);
        if (place)
            out.insert(out.end(), block.begin(), block.end());
        else
            out.push_back(0x00);
    }
    return out;
}

} // namespace

SendPath::SendPath(UpstreamSender* tx, SendConfig cfg, LogFn log)
    : tx_(tx), cfg_(cfg), log_(std::move(log)), step_(cfg.step0) {}

bool SendPath::tick(std::chrono::steady_clock::time_point now) {
    if (!cfg_.route_stream || !tx_)
        return false;
    if (!started_) {
        started_ = true;
        next_at_ = now;
    }
    if (now < next_at_)
        return false;
    next_at_ += std::chrono::milliseconds(cfg_.input_ms);
    // A long stall (gdb, a blocked recv) must not fire a burst to catch up:
    // the native never does, and a burst is what looks like a flood.
    if (now > next_at_ + std::chrono::milliseconds(500))
        next_at_ = now + std::chrono::milliseconds(cfg_.input_ms);

    const std::vector<uint8_t> frame = build_next_frame();
    if (frame.empty() || !tx_->send_unreliable(frame.data(), frame.size())) {
        ++send_failures_;
        return false;
    }
    ++frames_sent_;
    step_ += 4;
    if (frame_index_ < 3)
        ++frame_index_;
    return true;
}

std::vector<uint8_t> SendPath::build_next_frame() {
    const int idx = frame_index_ < 3 ? frame_index_ : 3;
    const std::vector<uint8_t> block = take_control_block();
    if (!block.empty())
        ++controls_sent_;
    // count=4 is the native's camera route frame: the 11-byte blob rides in
    // it (input.cpp's captured form), so it must stay in the rebuild.
    return assemble(step_, idx + 1, block,
                    placement_ == Placement::EveryFrame, idx == 3);
}

std::vector<uint8_t> SendPath::take_control_block() {
    if (!raw_control_.empty())
        return raw_control_;

    uint8_t mask = 0;
    uint8_t values = 0;
    if (pending_) {
        mask = pending_mask_;
        values = pending_values_;
        pending_ = false;
    } else {
        mask = hold_mask_;
        values = hold_values_;
    }
    if (mask == 0)
        return {};
    return input::controlBlock(mask, values);
}

std::vector<uint8_t> SendPath::next_control_block() const {
    if (!raw_control_.empty())
        return raw_control_;
    if (pending_)
        return input::controlBlock(pending_mask_, pending_values_);
    if (hold_mask_ != 0)
        return input::controlBlock(hold_mask_, hold_values_);
    return {};
}

bool SendPath::hold(uint8_t mask, uint8_t values) {
    hold_mask_ = mask;
    hold_values_ = values;
    if (mask != 0 && log_)
        log_("send: hold mask=0x" + std::string(1, "0123456789abcdef"[mask & 0xF]) +
             std::string(1, "0123456789abcdef"[(mask >> 4) & 0xF]));
    return true;
}

bool SendPath::release() {
    hold_mask_ = 0;
    hold_values_ = 0;
    pending_ = false;
    pending_mask_ = 0;
    pending_values_ = 0;
    return true;
}

bool SendPath::queue_control(uint8_t mask, uint8_t values) {
    pending_ = true;
    pending_mask_ = mask;
    pending_values_ = values;
    return true;
}

bool SendPath::raw(const uint8_t* data, size_t len, bool ordered) {
    if (!tx_ || !data || len == 0)
        return false;
    const bool ok = ordered ? tx_->send_ordered(data, len)
                            : tx_->send_unreliable(data, len);
    if (ok)
        ++raw_sent_;
    else
        ++send_failures_;
    return ok;
}

void SendPath::set_raw_control(std::vector<uint8_t> block) {
    raw_control_ = std::move(block);
}

void SendPath::clear_raw_control() {
    raw_control_.clear();
}

} // namespace rbxclient
