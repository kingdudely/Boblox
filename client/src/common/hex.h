// hex.h — hex encode/decode used by every app and by the QUIC/RakNet
// message builders.
//
// Before this header the same pair-decoder existed four times (raknet_main's
// --token/--cap-word parsers, hs_main's hex_prefix, send.cpp's hx, session.cpp's
// hx) and three formatters (raknet_main's last-datagram dump, play_main's
// caps, hs_main's stream preview). One implementation now, so a fix lands
// everywhere.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace rbx {

namespace detail {
inline int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
} // namespace detail

// Decode hex pairs, ignoring ' ' and ':' separators. A dangling final nibble
// is dropped (callers that need an exact size use hexDecodeInto). Invalid
// characters decode as 0, matching the send/session builders this replaced.
inline std::vector<uint8_t> hexDecode(const std::string& in) {
  std::string s;
  s.reserve(in.size());
  for (char c : in)
    if (c != ' ' && c != ':') s.push_back(c);
  std::vector<uint8_t> out;
  out.reserve(s.size() / 2);
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    const int hi = detail::hexNibble(s[i]);
    const int lo = detail::hexNibble(s[i + 1]);
    out.push_back(uint8_t(((hi < 0 ? 0 : hi) << 4) | (lo < 0 ? 0 : lo)));
  }
  return out;
}

// Strict decode: exactly `want * 2` hex digits (separators allowed). False on
// any wrong length or non-hex character, so CLI flags can reject bad input.
inline bool hexDecodeInto(const std::string& in, uint8_t* out, size_t want) {
  std::string s;
  s.reserve(in.size());
  for (char c : in)
    if (c != ' ' && c != ':') s.push_back(c);
  if (s.size() != want * 2) return false;
  for (size_t i = 0; i < want; ++i) {
    const int hi = detail::hexNibble(s[i * 2]);
    const int lo = detail::hexNibble(s[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = uint8_t((hi << 4) | lo);
  }
  return true;
}

inline std::string hexEncode(const uint8_t* p, size_t n) {
  static const char* kDigits = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    s.push_back(kDigits[p[i] >> 4]);
    s.push_back(kDigits[p[i] & 0xf]);
  }
  return s;
}

// Hex preview capped at `max` bytes, with an ellipsis when truncated (log
// lines print the head of a datagram, not 1200 bytes of it).
inline std::string hexPrefix(const uint8_t* p, size_t n, size_t max = 32) {
  const size_t m = n < max ? n : max;
  std::string s = hexEncode(p, m);
  if (n > max) s += "...";
  return s;
}

// Zero-padded 8/16-digit forms (challenge fields, QUIC capability words).
inline std::string hexU32(uint32_t v) {
  char b[16];
  snprintf(b, sizeof b, "%08x", unsigned(v));
  return b;
}

inline std::string hexU64(uint64_t v) {
  char b[32];
  snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
  return b;
}

} // namespace rbx
