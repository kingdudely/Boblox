// ack83.cpp — see ack83.h.
#include "raknet/ack83.h"

#include <cstring>

namespace rbxclient {
namespace ack83 {

const float kDefaultV = 803.5f;

namespace {

void putU24(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(uint8_t(v & 0xff));
  out->push_back(uint8_t((v >> 8) & 0xff));
  out->push_back(uint8_t((v >> 16) & 0xff));
}

void putU32(std::vector<uint8_t>* out, uint32_t v) {
  for (int k = 0; k < 4; ++k) out->push_back(uint8_t(v >> (8 * k)));
}

void putF32(std::vector<uint8_t>* out, float f) {
  uint32_t bits = 0;
  static_assert(sizeof bits == sizeof f, "f32 is 4 bytes");
  std::memcpy(&bits, &f, sizeof bits);
  putU32(out, bits);
}

} // namespace

std::vector<uint8_t> ack05(uint32_t clock_ms, float x, float z, float v,
                           bool reliable) {
  const uint32_t a = clock_ms & 0xffffffu;   // the field is 24 bits wide
  const uint32_t b = (clock_ms - 1) & 0xffffffu;

  std::vector<uint8_t> out;
  out.reserve(50);
  out.push_back(0x83);
  out.push_back(0x05);
  out.push_back(0x16);
  putU24(&out, a);
  putU32(&out, 0);
  out.push_back(0x00);
  putU24(&out, a);
  putU32(&out, 0);
  out.push_back(0x00);
  putF32(&out, x);
  putF32(&out, z);
  putF32(&out, 0.0f);
  putU32(&out, 0);
  out.push_back(0x01);
  putU24(&out, b);
  out.push_back(0x00);
  putF32(&out, v);
  if (reliable) {
    // Same 0x23 record the native bundles into the reliable copy. In the
    // capture that record's clock was a different, smaller counter than the
    // 0x05 clock (524 against 4316), so its epoch is not recovered; the live
    // 0x05 clock is the only defensible value to put there.
    out.push_back(0x23);
    putU24(&out, a);
    out.push_back(0x00);
  }
  out.push_back(0x00);
  return out;
}

} // namespace ack83
} // namespace rbxclient
