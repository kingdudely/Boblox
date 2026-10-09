// RUPP datagram framing — the Roblox UDP wrapper around QUIC packets.
//
// Wire format (native captures, tools/quicdump + py/rbx_client.py):
//   [01][00][u16 BE total] TLV1 TLV2 <QUIC payload>
//   total counts the 4-byte prefix itself, so the header is
//   4 + 19 + 8 = 31 bytes for the game path.
//   TLV1 = 01 11 01 + 16B NetStackTokenValue (type=1, len=17, subtype=1)
//   TLV2 = 02 06 + 4B RCC IPv4 (network order) + 2B RCC port (BE)
//          (native rupp_tx001: ip=10.37.8.91 port=61269 = rcc_ip:NetStackPort)
//
// The server wraps its replies the same way; datagrams that do not parse as
// RUPP are dropped (matches RuppTransport._strip semantics in py/rbx_client.py).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rbx {

constexpr size_t RUPP_HEADER_LEN = 31;

struct RuppHeader {
  uint8_t token[16]{};
  uint8_t rcc_ip[4]{};   // network byte order
  uint16_t rcc_port = 0; // host byte order (NetStackPort)
};

// Writes the RUPP header + payload into out; returns total wire length
// (0 if out is too small).
inline size_t rupp_wrap(uint8_t* out, size_t outcap, const RuppHeader& h,
                        const uint8_t* payload, size_t len) {
  if (outcap < RUPP_HEADER_LEN + len) return 0;
  uint8_t* p = out;
  *p++ = 0x01;
  *p++ = 0x00;
  *p++ = 0x00;
  *p++ = 0x1f; // total = 31 (prefix included)
  *p++ = 0x01;
  *p++ = 0x11;
  *p++ = 0x01; // type 1, len 17, subtype 1 (NetStackTokenValue)
  std::memcpy(p, h.token, 16);
  p += 16;
  *p++ = 0x02;
  *p++ = 0x06; // type 2, len 6
  std::memcpy(p, h.rcc_ip, 4);
  p += 4;
  *p++ = uint8_t(h.rcc_port >> 8);
  *p++ = uint8_t(h.rcc_port & 0xff);
  std::memcpy(p, payload, len);
  return RUPP_HEADER_LEN + len;
}

// Returns the QUIC payload pointer inside pkt, or nullptr if pkt is not a
// valid RUPP datagram (caller drops it, like the Python client does).
inline const uint8_t* rupp_strip(const uint8_t* pkt, size_t len, size_t* plen) {
  if (len < 4 || pkt[0] != 0x01) return nullptr;
  size_t total = (size_t(pkt[2]) << 8) | size_t(pkt[3]);
  if (total < 4 || total > len) return nullptr;
  *plen = len - total;
  return pkt + total;
}

} // namespace rbx
