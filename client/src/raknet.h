// raknet.h — Roblox RakNet transport (the transport the native client
// actually uses for gameplay; RbxTransport/QUIC is only the dummy path).
//
// Wire facts (native spy capture of the official client's RakNet session,
// see FINDINGS.md "RakNet" entry): the game connection lives on the Udmux
// port, every datagram is RUPP-wrapped, and the offline handshake uses
// Roblox's own packet ids (not stock RakNet's 0x05/0x06/0x07/0x08):
//
//   C->S  RUPP(31) + [7b][magic][05][pad to 1129]   OpenConnectionRequest1
//   S->C  RUPP(4)  + [7e][magic][guid8][sec1][mtu2] OpenConnectionReply1
//   C->S  RUPP(31) + [78][magic]<encrypted tail>    OpenConnectionRequest2
//   S->C  RUPP(23) + [7d][magic]<encrypted tail>    OpenConnectionReply2
//
// After Reply2 both sides stamp RUPP TLV `01 11 02 <16B session token>`
// (the client keeps its TLV2 rcc addr:port); the server issues that token
// in Reply2 and the client echoes it from then on.
//
// Everything after Reply1 is high-entropy: the tail of Req2/Reply2 and all
// subsequent game payloads are encrypted at the application layer (QUIC got
// that from TLS; RakNet needs the client-ticket/pepper key exchange). The
// offline handshake below is therefore complete through Reply1, and the
// Req2/Reply2+gameplay path is gated on that key exchange.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace rbxclient {

struct RaknetConfig {
  std::string host;         // UdmuxEndpoints[0].Address (RakNet endpoint)
  uint16_t port = 0;        // UdmuxEndpoints[0].Port (NOT NetStackPort)
  uint8_t join_token[16]{}; // RUPP TLV1 <NetStackTokenValue>, subtype 1
  uint8_t rcc_ip[4]{};      // RUPP TLV2 RCC IPv4 (network order)
  uint16_t rcc_port = 0;    // RUPP TLV2 port = NetStackPort (BE on the wire)
  uint8_t protocol_version = 0x05; // Req1 protocol byte (native: 5)
  size_t probe_size = 1129; // Req1 payload size (native probe)
  bool send_req2 = false;   // send Req2 when the ticket crypto is in place
};

class RaknetClient {
  public:
    using LogFn = std::function<void(const std::string&)>;

    explicit RaknetClient(LogFn log);
    ~RaknetClient();
    RaknetClient(const RaknetClient&) = delete;
    RaknetClient& operator=(const RaknetClient&) = delete;

    // Offline handshake: Req1 -> Reply1 (the fully decodable part). Returns
    // true once Reply1 is parsed; *err otherwise. Sends Req2 only when
    // cfg.send_req2 is set (needs the ticket/pepper key exchange).
    bool connect(const RaknetConfig& cfg, std::string* err);

    // Send one raw application payload on the game connection (RUPP-wrapped
    // with the current TLV1 token). False when not connected.
    bool send(const uint8_t* payload, size_t len);

    // Receive and classify datagrams for up to timeout_ms. Returns datagrams
    // read; non-RUPP datagrams are counted and dropped.
    int pump(int timeout_ms);

    void close();

    bool connected() const;              // Reply1 parsed
    int stage() const;                   // 0 none, 1 reply1, 2 reply2
    uint16_t mtu() const;                // Reply1 MTU (native: 1200)
    uint64_t server_guid() const;
    bool has_session_token() const;      // server issued TLV 01 11 02
    const uint8_t* session_token() const;
    uint64_t pkts_rx() const;
    uint64_t payloads_rx() const;
    uint64_t bytes_rx() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rbxclient
