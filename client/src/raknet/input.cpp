// input.cpp — see input.h.
#include "raknet/input.h"

#include "common/hex.h"

#include <algorithm>
#include <cmath>

namespace rbxclient {
namespace input {

const uint8_t kCameraBlob[11] = {0x10, 0x03, 0x00, 0x64, 0xd0, 0xb7,
                                 0xe6, 0xd7, 0xff, 0x03, 0xe0};

const uint8_t kSteadyRoute[14] = {0x10, 0xd9, 0xa4, 0x7d, 0x9b, 0xbf, 0xd2,
                                  0xa4, 0x7d, 0x55, 0x27, 0x89, 0x6f, 0x21};

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

void putU24le(std::vector<uint8_t>* out, uint32_t v) {
  for (int k = 0; k < 3; ++k) out->push_back(uint8_t(v >> (8 * k)));
}

void putI16le(std::vector<uint8_t>* out, int16_t v) {
  const uint16_t u = uint16_t(v);
  out->push_back(uint8_t(u & 0xff));
  out->push_back(uint8_t(u >> 8));
}

int16_t quantize_unit(double v) {
  const long long q = std::llround(v * 32767.0);
  return int16_t(std::max(-32767LL, std::min(32767LL, q)));
}

// One 13-byte action: id, instance ref, UnitVector3 type, quantised
// direction, and the constant tail byte the native writes after it.
void putLongAction(std::vector<uint8_t>* out, uint8_t id, uint32_t ref,
                   int16_t x, int16_t y, int16_t z) {
  out->push_back(id);
  putU24le(out, ref);
  out->push_back(0x04);
  out->push_back(0x00);
  putI16le(out, x);
  putI16le(out, y);
  putI16le(out, z);
  out->push_back(0x43);
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

std::vector<uint8_t> steadyFrame(uint32_t step, uint32_t server) {
  std::vector<uint8_t> out;
  out.reserve(37);
  out.push_back(0xA6);
  out.push_back(0x04);
  out.push_back(0x01);
  out.push_back(0x00);
  out.insert(out.end(), kSteadyRoute, kSteadyRoute + sizeof kSteadyRoute);
  putU32le(&out, step);
  putU32le(&out, server);
  out.push_back(0x00);
  out.push_back(0x00);
  for (int i = 0; i < 3; ++i) {
    out.push_back(0x04);
    out.push_back(0x04);
    out.push_back(0x00);
  }
  return out;
}

std::vector<uint8_t> longFrameQuantized(uint32_t step, uint32_t server,
                                        uint32_t ref,
                                        const int16_t directions[4][3],
                                        const uint8_t* route) {
  if (!directions) return {};
  const uint8_t* r = route ? route : kSteadyRoute;
  std::vector<uint8_t> out;
  out.reserve(141);
  out.push_back(0xA6);
  out.push_back(0x04);
  out.push_back(0x01);
  out.push_back(0x00);
  out.insert(out.end(), r, r + 14);
  putU32le(&out, step);
  putU32le(&out, server);
  out.push_back(0x0C);  // long-form route flag, measured in every 141B frame
  for (int i = 0; i < 4; ++i) {
    out.push_back(0x02);  // two ControlState actions in this input frame
    putLongAction(&out, 0x7F, ref, directions[i][0], directions[i][1],
                  directions[i][2]);
    putLongAction(&out, 0x11, ref, directions[i][0], directions[i][1],
                  directions[i][2]);
    if (i < 3) {
      out.push_back(0x04);
      out.push_back(0x04);
    }
  }
  return out;
}

std::vector<uint8_t> longFrame(uint32_t step, uint32_t server, uint32_t ref,
                               const double directions[4][3],
                               const uint8_t* route) {
  if (!directions) return {};
  int16_t q[4][3];
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 3; ++k) q[i][k] = quantize_unit(directions[i][k]);
  return longFrameQuantized(step, server, ref, q, route);
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
