// target.h — one reading of a joinScript, shared by every transport.
//
// RakNet and RbxTransport put different bytes on the wire, but they look at
// the same join script for the same facts: which Udmux edge to talk to, the
// RCC address/port, the RUPP token, the client ticket, and (for session
// crypto) the per-session server key plus the two early-auth blobs. That
// interpretation used to be copy-pasted into raknet_main.cpp and twice into
// play_main/hs_main behind two different error styles; it lives here once.
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rbx {

struct JoinTarget {
  // Network endpoints. udmux_port falls back to NetStackPort (the field both
  // transports use for the game app); netstack_port is kept separately
  // because RbxTransport targets NetStackPort for both app 4 and RUPP.
  std::string udmux_host;   // UdmuxEndpoints[0].Address
  uint16_t udmux_port = 0;  // UdmuxEndpoints[0].Port, else NetStackPort
  std::string rcc_host;     // ServerConnections[0].Address
  uint16_t rcc_port = 0;    // NetStackPort
  uint16_t netstack_port = 0;

  // RUPP TLV1 token (NetStackTokenValue, base64 of 16 bytes).
  bool has_token = false;
  uint8_t token[16] = {};

  std::string client_ticket;  // raw ClientTicket ("" when absent)

  // Session-crypto inputs (RakNet only; RbxTransport does not use them).
  bool has_crypto = false;
  uint8_t peer_pub[32] = {};
  std::string peer_key_from;  // keyring application name, or "raw"
  std::vector<uint8_t> ticket_pre;
  std::vector<uint8_t> ticket_auth;
  size_t ticket_parts = 0;  // ClientTicket semicolon-separated parts seen
};

// Reads the endpoint fields + RUPP token. False with *err naming the first
// missing or malformed field (e.g. "joinScript lacks NetStackPort").
bool readJoinTarget(const nlohmann::json& js, JoinTarget* out, std::string* err);

// Reads EphemeralEarlyPubKey (a bare base64 key or a keyring document — what
// ClientPublicKeyData always is) and the two early-auth blobs out of
// ClientTicket ("<iso>;<b64 258B>;<b64 pre>;<b64 auth>;<2>").
// False with *err when the key cannot be read; a short ticket blob only
// clears has_crypto, so a caller can still run with crypto disabled.
bool readSessionCrypto(const nlohmann::json& js, JoinTarget* out, std::string* err);

// "1.2.3.4" -> 4 bytes (false on anything else).
bool readIpv4(const std::string& s, uint8_t out[4]);

} // namespace rbx
