// util.h — wire encoding helpers shared by the join client and the
// RbxTransport session (exact ports of py/probe5.py helpers).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace rbx {

// Roblox compact varint (top-2-bit size scheme): stream framing + ctrl/chan
// messages. <0x40: 1B, <0x4000: 2B (0x40 prefix), <0x40000000: 4B (0x80),
// else 8B (0xC0). (probe5.compact_varint)
inline void compactVarint(std::vector<uint8_t>& out, uint64_t v) {
  if (v < 0x40) {
    out.push_back(uint8_t(v));
  } else if (v < 0x4000) {
    out.push_back(uint8_t(0x40 | (v >> 8)));
    out.push_back(uint8_t(v & 0xff));
  } else if (v < 0x40000000ull) {
    out.push_back(uint8_t(0x80 | (v >> 24)));
    out.push_back(uint8_t((v >> 16) & 0xff));
    out.push_back(uint8_t((v >> 8) & 0xff));
    out.push_back(uint8_t(v & 0xff));
  } else {
    out.push_back(uint8_t(0xC0 | (v >> 56)));
    out.push_back(uint8_t((v >> 48) & 0xff));
    out.push_back(uint8_t((v >> 40) & 0xff));
    out.push_back(uint8_t((v >> 32) & 0xff));
    out.push_back(uint8_t((v >> 24) & 0xff));
    out.push_back(uint8_t((v >> 16) & 0xff));
    out.push_back(uint8_t((v >> 8) & 0xff));
    out.push_back(uint8_t(v & 0xff));
  }
}

// Decode a compact varint at p. Returns bytes consumed (1/2/4/8) or 0 if
// the buffer is too short. (probe10.try_challenge walk)
inline size_t compactVarintDecode(const uint8_t* p, size_t len, uint64_t* v) {
  if (len < 1) return 0;
  size_t n = size_t(1) << (p[0] >> 6);
  if (n > len) return 0;
  uint64_t val = p[0] & 0x3f;
  for (size_t i = 1; i < n; i++) val = (val << 8) | p[i];
  *v = val;
  return n;
}

// Unsigned LEB128 (inner-field varint). (probe5.leb128)
inline void leb128(std::vector<uint8_t>& out, uint64_t v) {
  for (;;) {
    uint8_t b = uint8_t(v & 0x7f);
    v >>= 7;
    if (v) out.push_back(uint8_t(b | 0x80));
    else {
      out.push_back(b);
      break;
    }
  }
}

// Standard base64 decode with Python-b64decode semantics: non-alphabet chars
// are discarded, '=' padding tolerated; on structural failure returns the raw
// string bytes (early_auth_payload fallback).
inline std::string b64decode(const std::string& in) {
  static const char* alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int vals[256];
  for (int i = 0; i < 256; i++) vals[i] = -1;
  for (int i = 0; alphabet[i]; i++) vals[(unsigned char)alphabet[i]] = i;
  std::vector<int> s;
  for (unsigned char c : in) if (vals[c] >= 0) s.push_back(vals[c]);
  std::string out;
  // group into 4-tuples; a leftover of 1 is invalid -> fallback
  size_t i = 0;
  while (i + 4 <= s.size()) {
    uint32_t quad = uint32_t((s[i] << 18) | (s[i + 1] << 12) | (s[i + 2] << 6) | s[i + 3]);
    out.push_back(char(quad >> 16));
    out.push_back(char((quad >> 8) & 0xff));
    out.push_back(char(quad & 0xff));
    i += 4;
  }
  size_t rem = s.size() - i;
  if (rem == 2) {
    uint32_t quad = uint32_t((s[i] << 18) | (s[i + 1] << 12));
    out.push_back(char(quad >> 16));
  } else if (rem == 3) {
    uint32_t quad = uint32_t((s[i] << 18) | (s[i + 1] << 12) | (s[i + 2] << 6));
    out.push_back(char(quad >> 16));
    out.push_back(char((quad >> 8) & 0xff));
  } else if (rem == 1) {
    // Python b64decode: data-char count %4 == 1 -> binascii.Error -> the
    // fallback (raw string bytes). Leftover bits of 2/3-char groups are
    // ignored by a2b_base64, so no padding-bit validation here.
    return in;
  }
  return out;
}

// Frame payload = compactVarint(len) + payload. (probe5.frame)
inline std::vector<uint8_t> frame(const uint8_t* p, size_t n) {
  std::vector<uint8_t> out;
  compactVarint(out, n);
  out.insert(out.end(), p, p + n);
  return out;
}
inline std::vector<uint8_t> frame(const std::vector<uint8_t>& payload) {
  return frame(payload.data(), payload.size());
}

// Stream header: [06 01 app][u32 BE chan]. (probe5.stream_header)
inline std::vector<uint8_t> streamHeader(uint32_t app, uint64_t chan) {
  return {0x06, 0x01, uint8_t(app), uint8_t(chan >> 24), uint8_t(chan >> 16),
          uint8_t(chan >> 8), uint8_t(chan)};
}

inline uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint32_t le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Random v4 UUID from the kernel (no external dep).
inline std::string uuid4() {
  std::string s;
  if (FILE* f = fopen("/proc/sys/kernel/random/uuid", "r")) {
    char buf[64];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    s = buf;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  }
  if (s.empty()) s = "00000000-0000-4000-8000-000000000000";
  return s;
}

inline std::string readFileBytes(const std::string& path, bool* ok) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    *ok = false;
    return "";
  }
  std::string out;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  fclose(f);
  *ok = true;
  return out;
}

} // namespace rbx
