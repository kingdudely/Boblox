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
// After Reply2 the client keeps stamping the *join script's* RUPP token
// (TLV1 subtype 1) on every datagram. Reply2's own `01 11 02 <16B>` TLV is
// not a token the server accepts from the client: echoing it (the obvious
// reading, and what this client used to do) gets every post-handshake
// datagram silently dropped — the server keeps retransmitting and never ACKs.
// Live-verified 2026-10-27: the join token is what opens the session.
//
// Everything after Reply1 is encrypted by the native SessionCrypto (X25519 +
// BLAKE2b-512 KDF + ChaCha20-Poly1305 — see session_crypto.h and FINDINGS.md
// "RakNet session crypto"): the tail of Req2/Reply2 carries the join ticket
// (ClientTicket parts 2/3) plus the client's ephemeral key, and the server's
// Reply2 tag only verifies once it accepted that ticket and shares the keys.
// With those join fields in RaknetConfig, connect() runs the whole handshake.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rbxclient {

struct RaknetConfig {
  std::string host;         // UdmuxEndpoints[0].Address (RakNet endpoint)
  uint16_t port = 0;        // UdmuxEndpoints[0].Port (NOT NetStackPort)
  uint8_t join_token[16]{}; // RUPP TLV1 <NetStackTokenValue>, subtype 1
  uint8_t rcc_ip[4]{};      // RUPP TLV2 RCC IPv4 (network order)
  uint16_t rcc_port = 0;    // RUPP TLV2 port = NetStackPort (BE on the wire)
  uint8_t protocol_version = 0x05; // Req2 protocol field (native: 5)
  size_t probe_size = 1129; // Req2 payload size (native: 1160B datagram)
  bool send_req2 = false;   // send the plain (unsafe) experimental Req2

  // --- encrypted Req2 inputs, all straight from the join script -----------
  // have_crypto enables the real handshake: peer_pub is
  // joinScript.EphemeralEarlyPubKey (32B, per-session server key) and the two
  // blobs are ClientTicket parts 2/3 base64-decoded — the native client's
  // early-auth material (33B and 66B for the 5-part ticket the server sends).
  bool have_crypto = false;
  uint8_t peer_pub[32]{};
  std::vector<uint8_t> ticket_pre;   // ClientTicket part 2 (native: 33B)
  std::vector<uint8_t> ticket_auth;  // ClientTicket part 3 (native: 66B)
  // Capability word the native puts first in the Req2 plaintext: a bitmask
  // built from client FFlags (IDA sub_44869F6). Constant for a given client
  // build — captured value from the live 2026-10-27 session (byte order as
  // sent). The server answers with the negotiated subset in Reply2.
  uint8_t capability_word[8] = {0x00, 0x2d, 0xd3, 0xc7, 0x50, 0xb7, 0x08, 0x92};
  // Second plaintext field: per-session connection id (native: an 8-byte
  // NetworkClient method result, different every session). Zero = random.
  uint8_t session_id[8]{};
  uint8_t encryption_mode = 2; // Req2 [18]; native value for this build
  uint8_t application_id = 4;  // Req2 [19]; the RUPP app id (native: 4)
  // RUPP token subtype on post-handshake datagrams: 0 = native behaviour
  // (join token on the first datagram, then the session token from Reply2),
  // 1/2 = force that subtype (CLI --token-sub; the experiments in FINDINGS).
  uint8_t force_token_sub = 0;

  // The join script's raw ClientTicket string. The native splits it on ';'
  // (IDA sub_63DE3A4, called on the ACCEPTED path right after
  // "Successfully connected to server") and hands the parts to the RakPeer,
  // which is what produces the 0x8a message every live session sends once.
  std::string client_ticket;
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

    // Send one raw payload on the game connection (RUPP-wrapped with the
    // current TLV1 token, *not* datagram-encrypted). False when not connected.
    bool send(const uint8_t* payload, size_t len);

    // --- post-handshake data layer (cracked 2026-10-27) --------------------
    // True once Reply2's tag verified and the datagram keys are installed:
    //   peer  = reply2.raw[21:44] + reply2.plain[0:9]   (23 + 9 = 32 bytes)
    //   kdf   = BLAKE2b-512(X25519(eph_secret, peer) || our_pub || peer)
    //   tx    = digest[32:64], rx = digest[0:32]        (AES-256-GCM)
    // Frames are [ciphertext][counter low16 LE][tag16], no AAD, nonce =
    // LE64(counter) || "mbeR", counters from LE64("UniqueNu") +1 per frame.
    bool data_ready() const;

    // Wrap `data` as one RELIABLE RakNet message (0x40 + bit length + message
    // number), frame it with the data AEAD and send it.
    bool sendReliable(const uint8_t* data, size_t len);

    // RakNet ID_CONNECTION_REQUEST (0x09) carrying our session GUID.
    bool sendConnectionRequest();

    // RakNet ID_NEW_INCOMING_CONNECTION (0x13) with the address list the
    // native client sends right after ACCEPTED (7-byte RakNet addresses
    // [ver][ip4][port BE], our socket address first, then the dummies).
    bool sendNewIncoming();

    // RakNet ID_SUBMIT_TICKET (0x8a in this build): the ticket-submit message
    // (rbxmsg::build8a + seal). Kept for the pre-join-message path.
    bool sendTicket(const std::string& ticket);

    // Send one already-built join message the way the native does: a message
    // that fits one datagram goes out as a single reliable-ordered 0x60 frame;
    // anything larger is cut into 1061-byte fragments (0x70 = reliable +
    // ordering index + split, each fragment its own reliable message with a
    // fresh datagram id, one ordering index shared by all the fragments, split
    // ids counted like the native: 0, 1, 2, ...). Returns datagrams sent.
    int sendJoinMessage(const uint8_t* msg, size_t len, bool ordered = true);

    // Datagrams sent / split messages sent (diagnostics).
    uint64_t datagrams_tx() const;
    uint64_t split_messages_tx() const;

    // ACK every reliable datagram received so far (0xc0 datagram:
    // count u16 BE, minEqualsMax u8, then min/max u24 LE pairs). The native
    // acks each range as soon as it sees it; without this the server keeps
    // retransmitting ACCEPTED.
    bool sendAcks();


    // ---- receive-side application messages --------------------------------
    // Every complete application message the server sends (0x83 replication,
    // 0x93 flags, 0x10 accepted, ...) after RakNet split reassembly and
    // ordering, delivered in order. The bytes are the app payload (id + body)
    // — the same shape RbxTransport hands to LiveRuntime::feed — so a RakNet
    // run parses replication through the identical engine API, not a copy.
    using MessageFn = std::function<void(const uint8_t* payload, size_t len)>;
    void setMessageSink(MessageFn fn);

    uint64_t app_messages() const;   // complete messages delivered
    uint64_t app_split_messages() const; // of those, reassembled from splits
    uint64_t app_bytes() const;
    uint64_t rx_malformed() const;

    // Last authenticated server datagram (still RakNet-encoded).
    const uint8_t* last_datagram() const;
    size_t last_datagram_len() const;
    uint64_t data_datagrams_rx() const; // decrypted, authenticated datagrams
    uint64_t data_frames_rx() const;    // encrypted frames seen (incl. failed)
    uint64_t acks_sent() const;         // ACK datagrams emitted
    uint64_t server_acks() const;       // ACK datagrams received

    // True once the server has pushed its network-settings override list
    // (0x93). The native sends its join only after that message arrives, so
    // the sequencer waits for it rather than racing the server's setup.
    bool netSettingsSeen() const;
    uint64_t dups_dropped() const;      // retransmitted datagrams ignored

    // Receive and classify datagrams for up to timeout_ms. Returns datagrams
    // read; non-RUPP datagrams are counted and dropped.
    int pump(int timeout_ms);

    void close();

    // Drop the socket and every piece of per-session state (keys, counters,
    // datagram/message numbering, split and ordering buffers, ACK bookkeeping)
    // so connect() starts a genuinely new RakNet session. Used when the RCC
    // never issues the 0x9B challenge: the ordered stream is blocked on a hole
    // and no amount of re-sending places it, but a new session can.
    void resetSession();

    bool connected() const;              // Reply1 parsed
    int stage() const;                   // 0 none, 1 reply1, 2 reply2
    // True only when the server's Reply2 AEAD tag verified against our derived
    // rx key: the proof the server accepted Req2 (ticket + ephemeral key) and
    // shares the session keys. reply2_plain() is the decrypted tail.
    bool reply2_ok() const;
    const uint8_t* reply2_plain() const;
    size_t reply2_plain_len() const;
    const uint8_t* client_pub() const;   // our ephemeral key sent in Req2
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
