// input.cpp — see input.h.
#include "raknet/input.h"

#include "common/hex.h"

namespace rbxclient {
namespace input {

const uint8_t kCameraBlob[11] = {0x10, 0x03, 0x00, 0x64, 0xd0, 0xb7,
                                 0xe6, 0xd7, 0xff, 0x03, 0xe0};

namespace {

// Native count 1..3 trailer after the step pair: count-1 empty frames.
void putEmptyFrames(std::vector<uint8_t>* out, int count) {
  for (int i = 1; i < count; ++i) {
    out->push_back(0x04);
    out->push_back(0x04);
    out->push_back(0x00);
  }
}

void putStepPair(std::vector<uint8_t>* out, uint32_t step) {
  for (int k = 0; k < 4; ++k) out->push_back(uint8_t(step >> (8 * k)));
  for (int k = 0; k < 4; ++k) out->push_back(uint8_t(step >> (8 * k)));
}

void putU32le(std::vector<uint8_t>* out, uint32_t v) {
  for (int k = 0; k < 4; ++k) out->push_back(uint8_t(v >> (8 * k)));
}

} // namespace

std::vector<uint8_t> routeFrame(int idx, uint32_t step) {
  static const char* kFrames[4] = {
      "a6010100014c0100004c0100000000",
      "a6020100014c0100004c0100000000040400",
      "a6030100014c0100004c0100000000040400040400",
      "a604010010030064d0b7e6d7ff03e058010000580100000000040400040400040400",
  };
  if (idx < 0 || idx > 3) return {};
  std::vector<uint8_t> v = rbx::hexDecode(kFrames[idx]);
  if (v.size() < 13) return {};
  // The step pair sits after the camera/short blob: offset 5 for count 1-3
  // (blob is one byte) and offset 15 for the count=4 camera blob.
  const size_t o = (idx == 3) ? 15 : 5;
  if (v.size() >= o + 8) {
    for (int k = 0; k < 4; ++k) {
      v[o + k] = uint8_t(step >> (8 * k));
      v[o + 4 + k] = uint8_t(step >> (8 * k));
    }
  }
  return v;
}

std::vector<uint8_t> buildFrame(uint32_t step, int count,
                                const std::vector<uint8_t>& control,
                                bool camera) {
  if (count < 1) count = 1;
  if (count > 4) count = 4;
  std::vector<uint8_t> out;
  out.reserve(40);
  out.push_back(0xA6);
  out.push_back(uint8_t(count));
  out.push_back(0x01);
  out.push_back(0x00);
  if (camera) {
    out.insert(out.end(), kCameraBlob, kCameraBlob + sizeof kCameraBlob);
  } else {
    out.push_back(0x01);
  }
  putStepPair(&out, step);
  out.push_back(0x00);  // cond byte (native: 00)
  out.push_back(0x00);  // first sub-frame nActions
  putEmptyFrames(&out, count);
  if (!control.empty()) {
    out.pop_back();  // replace the final nActions byte
    out.insert(out.end(), control.begin(), control.end());
  }
  return out;
}

std::vector<uint8_t> controlBlock(uint8_t slots, uint8_t values, uint32_t a,
                                  uint32_t b) {
  if (slots == 0) return {};
  std::vector<uint8_t> out;
  out.push_back(0x01);  // N = one block
  putU32le(&out, a);
  putU32le(&out, b);
  std::vector<uint8_t> vals;
  for (int i = 0; i < 8; ++i) {
    if (slots & (1u << i)) vals.push_back(uint8_t((values >> i) & 1));
  }
  const uint16_t inner = uint16_t(1 + vals.size());  // presence + values
  out.push_back(uint8_t(inner & 0xff));
  out.push_back(uint8_t(inner >> 8));
  out.push_back(slots);
  out.insert(out.end(), vals.begin(), vals.end());
  return out;
}

std::vector<uint8_t> withControl(const std::vector<uint8_t>& frame,
                                 const std::vector<uint8_t>& block) {
  if (frame.empty() || block.empty()) return {};
  std::vector<uint8_t> v = frame;
  v.pop_back();
  v.insert(v.end(), block.begin(), block.end());
  return v;
}

std::vector<uint8_t> nextFrame(StreamState* s) {
  if (!s) return {};
  const int idx = s->frame_index < 3 ? s->frame_index : 3;
  std::vector<uint8_t> f = routeFrame(idx, s->step);
  s->step += 4;  // native advances both step slots by 4 per message
  if (s->frame_index < 3) ++s->frame_index;
  return f;
}

} // namespace input
} // namespace rbxclient
