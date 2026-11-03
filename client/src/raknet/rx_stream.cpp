// rx_stream.cpp — see rx_stream.h.
//
// The byte layout lives in raknet/protocol.cpp (one implementation, tested
// without a socket); this file is only the reassembly policy: splits by id,
// per-channel ordering, duplicate suppression, and the counters.
#include "raknet/rx_stream.h"

#include "raknet/protocol.h"

#include <algorithm>
#include <cstring>

namespace rbxclient {
namespace {

constexpr size_t kMaxPayload = 1u << 20;   // 1 MiB: no app message is larger
constexpr uint32_t kMaxSplitCount = 1024;  // fragments per split message
constexpr size_t kMaxPendingSplits = 64;   // concurrent split ids

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
  raknet::DatagramHeader h;
  if (!raknet::parse_datagram_header(d, n, &h)) {
    ++malformed_;
    return;
  }
  ++datagrams_;
  // ACK and NAK datagrams carry only transport state.
  if (h.kind == raknet::DatagramKind::Ack ||
      h.kind == raknet::DatagramKind::Nak) {
    ++ack_datagrams_;
    return;
  }
  if (h.kind != raknet::DatagramKind::Data) {
    ++malformed_;
    return;
  }

  auto onMessage = [&](const raknet::MessageView& m) -> bool {
    if (m.size == 0) return true;  // empty keepalive slot, not an app message
    const bool ordered = m.reliability == raknet::Reliability::ReliableOrdered;

    if (!m.split) {
      if (ordered) {
        RaknetAppMessage msg;
        msg.id = m.data[0];
        msg.reliability = m.reliability_byte;
        msg.ordered = true;
        msg.channel = m.channel;
        msg.ordering = m.ordering_index;
        msg.payload.assign(m.data, m.data + m.size);
        deliverOrdered(msg, sink);
      } else {
        deliver(m.data, m.size, m.reliability_byte, false, m.channel,
                m.ordering_index, sink);
      }
      return true;
    }

    // ---- split message ------------------------------------------------
    if (m.split_count == 0 || m.split_count > kMaxSplitCount ||
        m.split_index >= m.split_count) {
      ++malformed_;
      return true;
    }
    auto it = splits_.find(m.split_id);
    if (it == splits_.end()) {
      if (splits_.size() >= kMaxPendingSplits) {
        ++malformed_;  // never let a hostile peer grow the buffer unboundedly
        return true;
      }
      SplitBuffer b;
      b.count = m.split_count;
      b.parts.resize(m.split_count);
      b.reliability = m.reliability_byte;
      b.channel = m.channel;
      b.ordering = m.ordering_index;
      b.ordered = ordered;
      it = splits_.emplace(m.split_id, std::move(b)).first;
    }
    SplitBuffer& b = it->second;
    if (b.count != m.split_count) {
      ++malformed_;  // split id reused with a different shape
      splits_.erase(it);
      return true;
    }
    if (!b.parts[m.split_index].empty()) {
      ++duplicate_splits_;  // retransmitted fragment
      return true;
    }
    b.parts[m.split_index].assign(m.data, m.data + m.size);
    ++b.have;
    if (b.have != b.count) return true;

    size_t total = 0;
    for (const auto& p : b.parts) total += p.size();
    if (total == 0 || total > kMaxPayload) {
      ++malformed_;
      splits_.erase(it);
      return true;
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
    return true;
  };

  const raknet::WalkResult w =
      raknet::walk_messages(d + h.body, n - h.body, onMessage);
  if (!w.ok) ++malformed_;
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
    ++duplicate_ordering_;  // retransmit we already delivered
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
