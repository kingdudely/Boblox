// rx_stream_test.cpp — RakNet datagram reassembly (raknet/rx_stream.h).
//
// Synthetic datagrams, constructed exactly like the native session's
// post-handshake frames (FINDINGS.md "RakNet inner protocol"):
//   [flags][dgram u24 LE][reliability][u16 BE bits][msgno u24 LE]
//   [ordering u24 LE][channel u8][split: count u32 BE, id u16 BE, index u32 BE]
//   [data]
// The point of the test is the contract the replication parser relies on:
// split messages come out whole and ordered, ACKs never do, retransmits are
// dropped rather than parsed twice.
#include "raknet/rx_stream.h"

#include <cstdio>
#include <string>
#include <vector>

using rbxclient::RaknetAppMessage;
using rbxclient::RaknetRxStream;

static int g_fail = 0;

static void check(bool ok, const std::string& what) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++g_fail;
}

namespace {

void putU24(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t(x & 0xff));
  v.push_back(uint8_t((x >> 8) & 0xff));
  v.push_back(uint8_t((x >> 16) & 0xff));
}
void putU24BE(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t((x >> 16) & 0xff));
  v.push_back(uint8_t((x >> 8) & 0xff));
  v.push_back(uint8_t(x & 0xff));
}
void putU32BE(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t((x >> 24) & 0xff));
  v.push_back(uint8_t((x >> 16) & 0xff));
  v.push_back(uint8_t((x >> 8) & 0xff));
  v.push_back(uint8_t(x & 0xff));
}
void putU16BE(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(uint8_t((x >> 8) & 0xff));
  v.push_back(uint8_t(x & 0xff));
}

std::vector<uint8_t> datagram(uint8_t flags, uint32_t dgram) {
  std::vector<uint8_t> d{flags};
  putU24(d, dgram);
  return d;
}

// kind: 0 unreliable, 2 reliable, 3 reliable ordered, 4 reliable sequenced.
struct Msg {
  uint8_t kind = 3;
  uint32_t msgno = 0;
  uint32_t ordering = 0;
  uint8_t channel = 0;
  bool split = false;
  uint32_t split_count = 0;
  uint16_t split_id = 0;
  uint32_t split_index = 0;
  std::vector<uint8_t> data;
};

void appendMessage(std::vector<uint8_t>& d, const Msg& m) {
  uint8_t rel = uint8_t(m.kind << 5);
  if (m.split) rel |= 0x10;
  d.push_back(rel);
  const uint16_t bits = uint16_t(m.data.size() * 8);
  putU16BE(d, bits);
  if (m.kind >= 2) putU24(d, m.msgno);
  if (m.kind == 1 || m.kind == 3 || m.kind == 4) {
    putU24(d, m.ordering);
    d.push_back(m.channel);
  }
  if (m.split) {
    putU32BE(d, m.split_count);
    putU16BE(d, m.split_id);
    putU32BE(d, m.split_index);
  }
  d.insert(d.end(), m.data.begin(), m.data.end());
}

std::vector<uint8_t> bytes(std::initializer_list<uint8_t> l) {
  return std::vector<uint8_t>(l);
}

} // namespace

int main() {
  // ---- 1. one reliable-ordered message --------------------------------
  {
    RaknetRxStream rx;
    std::vector<RaknetAppMessage> got;
    std::vector<uint8_t> d = datagram(0x81, 0);
    Msg m;
    m.kind = 3;
    m.ordering = 4;
    m.data = bytes({0x83, 0x23, 0x01});
    appendMessage(d, m);
    rx.onDatagram(d.data(), d.size(),
                  [&](const RaknetAppMessage& a) { got.push_back(a); });
    check(got.size() == 1, "single datagram -> one message");
    check(!got.empty() && got[0].id == 0x83 && got[0].payload.size() == 3,
          "payload is id + body, untouched");
    check(!got.empty() && got[0].ordered && got[0].ordering == 4,
          "ordering index preserved");
    check(rx.messages() == 1 && rx.malformed() == 0, "counters: 1 message, 0 malformed");
  }

  // ---- 2. ACK datagrams carry no application data ----------------------
  {
    RaknetRxStream rx;
    int calls = 0;
    std::vector<uint8_t> d = datagram(0xc0, 9);
    d.insert(d.end(), {0x00, 0x01, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00});
    rx.onDatagram(d.data(), d.size(), [&](const RaknetAppMessage&) { ++calls; });
    check(calls == 0 && rx.ack_datagrams() == 1, "server ACK is counted, not parsed");
  }

  // ---- 3. split message, fragments arriving backwards -------------------
  {
    RaknetRxStream rx;
    std::vector<RaknetAppMessage> got;
    std::vector<uint8_t> part0(bytes({0x97, 0xa3, 0xcf, 0x00}));
    std::vector<uint8_t> part1(bytes({0x00, 0x10, 0x00, 0x00, 0x20}));
    Msg a;
    a.kind = 2; // reliable, not ordered
    a.split = true;
    a.split_count = 2;
    a.split_id = 7;
    a.split_index = 1;
    a.data = part1;
    Msg b = a;
    b.split_index = 0;
    b.data = part0;
    std::vector<uint8_t> d1 = datagram(0x83, 1);
    appendMessage(d1, a);
    std::vector<uint8_t> d2 = datagram(0x83, 2);
    appendMessage(d2, b);
    auto sink = [&](const RaknetAppMessage& x) { got.push_back(x); };
    rx.onDatagram(d1.data(), d1.size(), sink);
    check(got.empty(), "incomplete split waits for the missing fragment");
    rx.onDatagram(d2.data(), d2.size(), sink);
    check(got.size() == 1, "out-of-order fragments reassemble into one message");
    check(!got.empty() && got[0].payload == bytes({0x97, 0xa3, 0xcf, 0x00, 0x00, 0x10,
                                                  0x00, 0x00, 0x20}),
          "reassembled bytes are fragment 0 then fragment 1");
    check(rx.split_messages() == 1 && rx.pending_splits() == 0,
          "split counter + no leaked pending state");
  }

  // ---- 4. retransmitted fragment is dropped, not parsed twice ----------
  {
    RaknetRxStream rx;
    int calls = 0;
    Msg m;
    m.kind = 2;
    m.split = true;
    m.split_count = 2;
    m.split_id = 3;
    m.split_index = 0;
    m.data = bytes({0x81, 0x01});
    std::vector<uint8_t> d = datagram(0x83, 5);
    appendMessage(d, m);
    auto sink = [&](const RaknetAppMessage&) { ++calls; };
    rx.onDatagram(d.data(), d.size(), sink);
    rx.onDatagram(d.data(), d.size(), sink); // same fragment again
    m.split_index = 1;
    m.data = bytes({0x02, 0x03});
    std::vector<uint8_t> d2 = datagram(0x83, 6);
    appendMessage(d2, m);
    rx.onDatagram(d2.data(), d2.size(), sink);
    check(calls == 1, "duplicate fragment does not corrupt the message");
    check(rx.duplicate_splits() == 1, "duplicate fragment counted");
  }

  // ---- 5. one datagram, several messages, delivered in order -----------
  {
    RaknetRxStream rx;
    std::vector<uint8_t> ids;
    std::vector<uint8_t> d = datagram(0x81, 0);
    Msg m;
    m.kind = 2; // reliable: no ordering header
    m.data = bytes({0x51, 0x00});
    appendMessage(d, m);
    m.data = bytes({0x52, 0x00});
    appendMessage(d, m);
    m.kind = 3;
    m.ordering = 0;
    m.data = bytes({0x53, 0x00});
    appendMessage(d, m);
    rx.onDatagram(d.data(), d.size(),
                  [&](const RaknetAppMessage& a) { ids.push_back(a.id); });
    check(ids == bytes({0x51, 0x52, 0x53}), "messages inside one datagram keep their order");
  }

  // ---- 6. ordered message with a gap is held, then flushed -------------
  {
    RaknetRxStream rx;
    std::vector<uint32_t> order;
    auto sink = [&](const RaknetAppMessage& a) { order.push_back(a.ordering); };
    Msg m;
    m.kind = 3;
    m.data = bytes({0x83, 0x00});
    m.ordering = 7;
    std::vector<uint8_t> d1 = datagram(0x81, 0);
    appendMessage(d1, m);
    rx.onDatagram(d1.data(), d1.size(), sink);
    check(order.size() == 1 && order[0] == 7, "first ordered message establishes the sequence");
    m.ordering = 9; // gap at 8
    std::vector<uint8_t> d2 = datagram(0x81, 1);
    appendMessage(d2, m);
    rx.onDatagram(d2.data(), d2.size(), sink);
    check(order.size() == 1 && rx.pending_ordered() == 1, "out-of-order message is buffered");
    m.ordering = 8;
    std::vector<uint8_t> d3 = datagram(0x81, 2);
    appendMessage(d3, m);
    rx.onDatagram(d3.data(), d3.size(), sink);
    check(order == std::vector<uint32_t>({7, 8, 9}), "gap fill releases 8 then 9 in order");
    check(rx.pending_ordered() == 0, "buffer drained");
  }

  // ---- 7. malformed input never crashes or emits -----------------------
  {
    RaknetRxStream rx;
    int calls = 0;
    auto sink = [&](const RaknetAppMessage&) { ++calls; };
    const uint8_t short_dg[] = {0x81, 0x00};
    rx.onDatagram(short_dg, sizeof short_dg, sink);
    std::vector<uint8_t> d = datagram(0x81, 0);
    d.insert(d.end(), {0x70, 0xff, 0xff}); // says 8191 bytes, has none
    rx.onDatagram(d.data(), d.size(), sink);
    check(calls == 0 && rx.malformed() == 2, "truncated datagrams are rejected loudly");
  }

  printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "passed", g_fail,
         g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
