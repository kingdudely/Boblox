// rx_stream.cpp — see rx_stream.h.
#include "raknet/rx_stream.h"

#include <algorithm>
#include <cstring>

namespace rbxclient {
namespace {

constexpr size_t kMaxPayload = 1u << 20;   // 1 MiB: no app message is larger
constexpr uint32_t kMaxSplitCount = 1024;  // fragments per split message
constexpr size_t kMaxPendingSplits = 64;   // concurrent split ids

inline uint32_t rdU24(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
}
inline uint32_t rdU32BE(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint16_t rdU16BE(const uint8_t* p) {
  return uint16_t((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}

} // namespace

size_t RaknetRxStream::pending_ordered() const {
  size_t n = 0;
  for (const auto& kv : ord_buf_) n += kv.second.size();
  return n;
}

void RaknetRxStream::onDatagram(const uint8_t* d, size_t n, const Sink& sink) {
  if (!d || n < 4) {
    ++malformed_;
    return;
  }
  ++datagrams_;
  // ACK datagrams (bit7 + bit6) carry only transport state.
  if ((d[0] & 0xc0) == 0xc0) {
    ++ack_datagrams_;
    return;
  }
  size_t off = 4; // flags + datagram id (u24 LE)
  while (off + 3 <= n) {
    const uint8_t rel = d[off];
    const uint16_t bits = uint16_t((uint16_t(d[off + 1]) << 8) | d[off + 2]);
    const size_t dlen = (size_t(bits) + 7) / 8;
    const unsigned kind = unsigned(rel >> 5); // 0..4
    off += 3;

    // RELIABLE (and every ordered/sequenced variant): message number.
    if (kind >= 2) {
      if (off + 3 > n) {
        ++malformed_;
        return;
      }
      off += 3;
    }
    bool ordered = false;
    uint8_t channel = 0;
    uint32_t ordering = 0;
    if (kind == 1 || kind == 3 || kind == 4) {
      if (off + 4 > n) {
        ++malformed_;
        return;
      }
      ordering = rdU24(d + off);
      channel = d[off + 3];
      ordered = (kind == 3);
      off += 4;
    }

    uint32_t split_count = 0, split_index = 0;
    uint16_t split_id = 0;
    const bool split = (rel & 0x10) != 0;
    if (split) {
      if (off + 10 > n) {
        ++malformed_;
        return;
      }
      split_count = rdU32BE(d + off);
      split_id = rdU16BE(d + off + 4);
      split_index = rdU32BE(d + off + 6);
      off += 10;
    }
    if (off + dlen > n || dlen > kMaxPayload) {
      ++malformed_;
      return;
    }
    const uint8_t* data = d + off;
    off += dlen;

    if (dlen == 0) continue;

    if (!split) {
      if (ordered) {
        RaknetAppMessage msg;
        msg.id = data[0];
        msg.reliability = rel;
        msg.ordered = true;
        msg.channel = channel;
        msg.ordering = ordering;
        msg.payload.assign(data, data + dlen);
        deliverOrdered(msg, sink);
      } else {
        deliver(data, dlen, rel, false, channel, ordering, sink);
      }
      continue;
    }

    // ---- split message ------------------------------------------------
    if (split_count == 0 || split_count > kMaxSplitCount ||
        split_index >= split_count) {
      ++malformed_;
      continue;
    }
    auto it = splits_.find(split_id);
    if (it == splits_.end()) {
      if (splits_.size() >= kMaxPendingSplits) {
        ++malformed_; // never let a hostile peer grow the buffer unboundedly
        continue;
      }
      SplitBuffer b;
      b.count = split_count;
      b.parts.resize(split_count);
      b.reliability = rel;
      b.channel = channel;
      b.ordering = ordering;
      b.ordered = ordered;
      it = splits_.emplace(split_id, std::move(b)).first;
    }
    SplitBuffer& b = it->second;
    if (b.count != split_count) {
      ++malformed_; // split id reused with a different shape
      splits_.erase(it);
      continue;
    }
    if (!b.parts[split_index].empty()) {
      ++duplicate_splits_; // retransmitted fragment
      continue;
    }
    b.parts[split_index].assign(data, data + dlen);
    ++b.have;
    if (b.have != b.count) continue;

    size_t total = 0;
    for (const auto& p : b.parts) total += p.size();
    if (total == 0 || total > kMaxPayload) {
      ++malformed_;
      splits_.erase(it);
      continue;
    }
    RaknetAppMessage msg;
    msg.reliability = b.reliability;
    msg.ordered = b.ordered;
    msg.channel = b.channel;
    msg.ordering = b.ordering;
    msg.payload.reserve(total);
    for (auto& p : b.parts) msg.payload.insert(msg.payload.end(), p.begin(), p.end());
    msg.id = msg.payload[0];
    ++split_messages_;
    splits_.erase(it);
    if (msg.ordered) {
      deliverOrdered(msg, sink);
    } else {
      deliver(msg.payload.data(), msg.payload.size(), msg.reliability, false,
              msg.channel, msg.ordering, sink);
    }
  }
}

void RaknetRxStream::deliver(const uint8_t* payload, size_t len,
                             uint8_t reliability, bool ordered,
                             uint8_t channel, uint32_t ordering,
                             const Sink& sink) {
  if (!sink || !len) return;
  RaknetAppMessage msg;
  msg.id = payload[0];
  msg.reliability = reliability;
  msg.ordered = ordered;
  msg.channel = channel;
  msg.ordering = ordering;
  msg.payload.assign(payload, payload + len);
  ++messages_;
  sink(msg);
}

void RaknetRxStream::deliverOrdered(const RaknetAppMessage& msg,
                                    const Sink& sink) {
  const uint8_t ch = msg.channel;
  if (!ord_started_[ch]) {
    ord_started_[ch] = true;
    ord_next_[ch] = msg.ordering;
  }
  const uint32_t next = ord_next_[ch];
  if (msg.ordering < next) {
    ++duplicate_ordering_; // retransmit we already delivered
    return;
  }
  if (msg.ordering != next) {
    // Out of order: hold it until the gap fills. Duplicates replace nothing.
    ord_buf_[ch].emplace(msg.ordering, msg);
    return;
  }
  ++messages_;
  if (sink) sink(msg);
  uint32_t n = next + 1;
  auto& buf = ord_buf_[ch];
  for (;;) {
    auto it = buf.find(n);
    if (it == buf.end()) break;
    RaknetAppMessage held = std::move(it->second);
    buf.erase(it);
    ++messages_;
    if (sink) sink(held);
    ++n;
  }
  ord_next_[ch] = n;
}

} // namespace rbxclient
