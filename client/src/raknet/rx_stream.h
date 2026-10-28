// rx_stream.h — RakNet datagram -> complete application messages.
//
// The server multiplexes every application message (0x83 replication, 0x93
// flag list, 0x10 accepted, 0x51..0x56 pings) into RakNet internal packets,
// and the join/replication bodies themselves are split at 1069 bytes. This
// class is the receive-side inverse of RaknetClient's send path: it walks one
// decrypted datagram, drops ACKs, reassembles split messages and delivers
// complete payloads in RakNet's reliability order.
//
// The bytes it hands back are the *application* payload (id + body) — the
// same bytes the RbxTransport/QUIC path passes to LiveRuntime one channel
// stream frame at a time. RakNet framing (flags/dgram id/reliability/split)
// is the only layer this transport adds; nothing below it is
// transport-specific, so both transports feed the identical replication
// parser (engine/live_runtime.cpp -> solve/replicate.h).
//
// Wire facts (see FINDINGS.md "RakNet" and "RakNet inner protocol"):
//   datagram: [flags][dgram id u24 LE][message]*
//   message : [reliability][u16 BE bit length]
//             [if RELIABLE: msg number u24 LE]
//             [if ORDERED/SEQUENCED: ordering index u24 LE][channel u8]
//             [if SPLIT: count u32 BE][split id u16 BE][index u32 BE]
//             [data]
//   flags   : bit7 valid, bit6 ACK, bit5 continuous send
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace rbxclient {

// One complete application message (payload[0] is the app message id).
struct RaknetAppMessage {
  uint8_t id = 0;
  uint8_t reliability = 0; // raw RakNet reliability byte
  bool ordered = false;    // RELIABLE_ORDERED (kind 3)
  uint8_t channel = 0;
  uint32_t ordering = 0;
  std::vector<uint8_t> payload; // includes the id byte
};

// Reassembles datagrams. No I/O: hand it the bytes the data AEAD produced and
// a sink; it calls the sink synchronously for every message that became
// complete, in order.
class RaknetRxStream {
  public:
    using Sink = std::function<void(const RaknetAppMessage&)>;

    // Datagram body as it comes out of the data AEAD (flags byte first).
    void onDatagram(const uint8_t* d, size_t n, const Sink& sink);

    uint64_t datagrams() const { return datagrams_; }
    uint64_t ack_datagrams() const { return ack_datagrams_; }
    uint64_t messages() const { return messages_; }
    uint64_t split_messages() const { return split_messages_; }
    uint64_t duplicate_splits() const { return duplicate_splits_; }
    uint64_t duplicate_ordering() const { return duplicate_ordering_; }
    uint64_t malformed() const { return malformed_; }
    size_t pending_splits() const { return splits_.size(); }
    size_t pending_ordered() const;

  private:
    struct SplitBuffer {
        uint32_t count = 0;
        uint32_t have = 0;
        std::vector<std::vector<uint8_t>> parts;
        uint8_t reliability = 0;
        uint8_t channel = 0;
        uint32_t ordering = 0;
        bool ordered = false;
    };

    void deliver(const uint8_t* payload, size_t len, uint8_t reliability,
                 bool ordered, uint8_t channel, uint32_t ordering,
                 const Sink& sink);
    void deliverOrdered(const RaknetAppMessage& msg, const Sink& sink);

    std::map<uint32_t, SplitBuffer> splits_; // split id -> buffer
    std::map<uint8_t, std::map<uint32_t, RaknetAppMessage>> ord_buf_;
    std::map<uint8_t, uint32_t> ord_next_;
    std::map<uint8_t, bool> ord_started_;

    uint64_t datagrams_ = 0;
    uint64_t ack_datagrams_ = 0;
    uint64_t messages_ = 0;
    uint64_t split_messages_ = 0;
    uint64_t duplicate_splits_ = 0;
    uint64_t duplicate_ordering_ = 0;
    uint64_t malformed_ = 0;
};

} // namespace rbxclient
