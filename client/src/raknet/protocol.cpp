// protocol.cpp — see protocol.h. No I/O, no crypto, no session state.
#include "raknet/protocol.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rbxclient {
namespace raknet {
namespace {

constexpr size_t kMaxMessage = 1u << 20;  // 1 MiB: no app message is larger
constexpr uint32_t kMaxSplitCount = 1024;

inline uint16_t rd16be(const uint8_t* p) {
  return uint16_t((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}
inline uint16_t rd16le(const uint8_t* p) {
  return uint16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
}
inline uint32_t rd24le(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
}
inline uint32_t rd32be(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline void wr16be(std::vector<uint8_t>* v, uint16_t x) {
  v->push_back(uint8_t(x >> 8));
  v->push_back(uint8_t(x));
}
inline void wr24le(std::vector<uint8_t>* v, uint32_t x) {
  v->push_back(uint8_t(x));
  v->push_back(uint8_t(x >> 8));
  v->push_back(uint8_t(x >> 16));
}
inline void wr32be(std::vector<uint8_t>* v, uint32_t x) {
  v->push_back(uint8_t(x >> 24));
  v->push_back(uint8_t(x >> 16));
  v->push_back(uint8_t(x >> 8));
  v->push_back(uint8_t(x));
}

} // namespace

// --- reliability -----------------------------------------------------------

const char* reliability_name(Reliability r) {
  switch (r) {
  case Reliability::Unreliable: return "unreliable";
  case Reliability::UnreliableSequenced: return "unreliable-sequenced";
  case Reliability::Reliable: return "reliable";
  case Reliability::ReliableOrdered: return "reliable-ordered";
  case Reliability::ReliableSequenced: return "reliable-sequenced";
  }
  return "unknown";
}

bool reliability_is_reliable(Reliability r) {
  return unsigned(r) >= unsigned(Reliability::Reliable);
}

bool reliability_has_ordering(Reliability r) {
  return r == Reliability::UnreliableSequenced ||
         r == Reliability::ReliableOrdered ||
         r == Reliability::ReliableSequenced;
}

// --- datagrams -------------------------------------------------------------

const char* datagram_kind_name(DatagramKind k) {
  switch (k) {
  case DatagramKind::Data: return "data";
  case DatagramKind::Ack: return "ack";
  case DatagramKind::Nak: return "nak";
  case DatagramKind::Invalid: return "invalid";
  }
  return "invalid";
}

bool parse_datagram_header(const uint8_t* d, size_t n, DatagramHeader* out) {
  if (!out) return false;
  *out = DatagramHeader{};
  if (!d || n < 1) return false;
  const uint8_t flags = d[0];
  out->flags = flags;
  out->valid = (flags & 0x80) != 0;
  out->continuous_send = (flags & 0x20) != 0;
  if ((flags & 0xc0) == 0xc0) {
    out->kind = DatagramKind::Ack;
    out->body = 1;
    out->valid = true;
    return true;
  }
  if ((flags & 0xe0) == 0xa0) {
    out->kind = DatagramKind::Nak;
    out->body = 1;
    out->valid = true;
    return true;
  }
  if (!out->valid) {
    out->kind = DatagramKind::Invalid;
    return false;
  }
  if (n < 4) return false;
  out->kind = DatagramKind::Data;
  out->datagram_id = rd24le(d + 1);
  out->body = 4;
  return true;
}

size_t build_datagram_header(uint8_t* out, size_t cap, uint8_t flags,
                             uint32_t datagram_id, bool has_id) {
  if (!out) return 0;
  if (has_id) {
    if (cap < 4) return 0;
    out[0] = flags;
    out[1] = uint8_t(datagram_id);
    out[2] = uint8_t(datagram_id >> 8);
    out[3] = uint8_t(datagram_id >> 16);
    return 4;
  }
  if (cap < 1) return 0;
  out[0] = flags;
  return 1;
}

// --- messages --------------------------------------------------------------

WalkResult walk_messages(const uint8_t* d, size_t n, const MessageFn& fn) {
  WalkResult r;
  if (!d || n == 0) {
    r.ok = false;
    return r;
  }
  size_t off = 0;
  while (off + 3 <= n) {
    MessageView m;
    m.reliability_byte = d[off];
    m.reliability = Reliability(m.reliability_byte >> 5);
    m.split = (m.reliability_byte & 0x10) != 0;
    if (m.reliability_byte & 0x0f) {
      // Low nibble is reserved apart from the split bit; a value there is a
      // framing error, not a reliability class.
      r.ok = false;
      r.bad_offset = off;
      return r;
    }
    const uint16_t bits = rd16be(d + off + 1);
    const size_t dlen = (size_t(bits) + 7) / 8;
    off += 3;
    if (dlen > kMaxMessage) {
      r.ok = false;
      r.bad_offset = off - 3;
      return r;
    }
    if (reliability_is_reliable(m.reliability)) {
      if (off + 3 > n) {
        r.ok = false;
        r.bad_offset = off;
        return r;
      }
      m.message_number = rd24le(d + off);
      off += 3;
    }
    if (reliability_has_ordering(m.reliability)) {
      if (off + 4 > n) {
        r.ok = false;
        r.bad_offset = off;
        return r;
      }
      m.ordering_index = rd24le(d + off);
      m.channel = d[off + 3];
      off += 4;
    }
    if (m.split) {
      if (off + 10 > n) {
        r.ok = false;
        r.bad_offset = off;
        return r;
      }
      m.split_count = rd32be(d + off);
      m.split_id = rd16be(d + off + 4);
      m.split_index = rd32be(d + off + 6);
      off += 10;
      if (m.split_count == 0 || m.split_count > kMaxSplitCount ||
          m.split_index >= m.split_count) {
        r.ok = false;
        r.bad_offset = off - 10;
        return r;
      }
    }
    if (off + dlen > n) {
      r.ok = false;
      r.bad_offset = off;
      return r;
    }
    m.data = d + off;
    m.size = dlen;
    off += dlen;
    ++r.messages;
    if (fn && !fn(m)) {
      r.consumed = off;
      return r;
    }
  }
  r.consumed = off;
  if (off != n) {
    r.ok = false;
    r.bad_offset = off;
  }
  return r;
}

std::vector<uint8_t> build_message(const MessageSpec& m) {
  std::vector<uint8_t> out;
  out.reserve(3 + 10 + 10 + 4 + m.size);
  uint8_t rel = uint8_t(unsigned(m.reliability) << 5);
  if (m.split) rel |= 0x10;
  out.push_back(rel);
  const size_t bits = m.size * 8;
  wr16be(&out, uint16_t(bits > 0xffff ? 0xffff : bits));
  if (reliability_is_reliable(m.reliability)) wr24le(&out, m.message_number);
  if (reliability_has_ordering(m.reliability)) {
    wr24le(&out, m.ordering_index);
    out.push_back(m.channel);
  }
  if (m.split) {
    wr32be(&out, m.split_count);
    wr16be(&out, m.split_id);
    wr32be(&out, m.split_index);
  }
  if (m.data && m.size) out.insert(out.end(), m.data, m.data + m.size);
  return out;
}

bool append_message(std::vector<uint8_t>* out, const MessageSpec& m) {
  if (!out) return false;
  const std::vector<uint8_t> bytes = build_message(m);
  out->insert(out->end(), bytes.begin(), bytes.end());
  return true;
}

// --- ACK / NAK -------------------------------------------------------------

namespace {

bool ack_kind_ok(DatagramKind k) {
  return k == DatagramKind::Ack || k == DatagramKind::Nak;
}

uint8_t ack_flag(DatagramKind k) {
  return k == DatagramKind::Nak ? kFlagNak : kFlagAck;
}

} // namespace

std::vector<uint8_t> build_ack_datagram(DatagramKind kind,
                                        const std::vector<uint32_t>& ids) {
  std::vector<uint8_t> out;
  if (!ack_kind_ok(kind)) return out;
  std::vector<uint32_t> sorted = ids;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  std::vector<AckRecord> ranges;
  for (uint32_t id : sorted) {
    if (!ranges.empty() && id == ranges.back().hi + 1) {
      ranges.back().hi = id;
    } else {
      ranges.push_back({id, id});
    }
  }
  out.push_back(ack_flag(kind));
  wr16be(&out, uint16_t(ranges.size()));
  for (const AckRecord& r : ranges) {
    if (r.single()) {
      out.push_back(0x01);
      wr24le(&out, r.lo);
    } else {
      out.push_back(0x00);
      wr24le(&out, r.lo);
      wr24le(&out, r.hi);
    }
  }
  return out;
}

bool parse_ack_datagram(const uint8_t* d, size_t n, std::vector<AckRecord>* out) {
  if (!d || n < 3 || !out) return false;
  DatagramHeader h;
  if (!parse_datagram_header(d, n, &h)) return false;
  if (!ack_kind_ok(h.kind)) return false;
  const uint16_t count = rd16be(d + 1);
  size_t off = 3;
  out->clear();
  out->reserve(count + 4);
  for (uint16_t i = 0; i < count; ++i) {
    if (off + 1 > n) return false;
    const uint8_t tag = d[off++];
    if (tag == 0x01) {
      if (off + 3 > n) return false;
      const uint32_t id = rd24le(d + off);
      off += 3;
      out->push_back({id, id});
    } else if (tag == 0x00) {
      if (off + 6 > n) return false;
      const uint32_t lo = rd24le(d + off);
      const uint32_t hi = rd24le(d + off + 3);
      off += 6;
      if (hi < lo) return false;
      out->push_back({lo, hi});
    } else {
      // Not a record tag. The RCC's form is `d0 0001 01 <u24> <u24> ...`
      // (one single record, then bare ids), so rewind and read the rest as
      // bare u24 ids: a tag byte that is not 0/1 is the low byte of one.
      off -= 1;
      break;
    }
  }
  // Trailing bare u24 ids (server form, covered above too).
  while (off + 3 <= n) {
    const uint32_t id = rd24le(d + off);
    off += 3;
    out->push_back({id, id});
  }
  return off == n;
}

void expand_acks(const std::vector<AckRecord>& records,
                 std::vector<uint32_t>* out) {
  if (!out) return;
  for (const AckRecord& r : records) {
    if (r.single()) {
      out->push_back(r.lo);
      continue;
    }
    // Guard against a hostile range: the id space is 24-bit, so a full range
    // is bounded by that alone.
    for (uint32_t id = r.lo; id <= r.hi && id - r.lo < (1u << 24); ++id)
      out->push_back(id);
  }
}

// --- packet ids ------------------------------------------------------------

namespace {

// The table is intentionally explicit: a wrong name is worse than no name.
// `note` records where Roblox's build differs from stock RakNet or where the
// id is reused by the application layer.
const PacketInfo kPackets[] = {
    {0x00, "ID_CONNECTED_PING", "9B ping [id][u32 0][u24 clock]"},
    {0x01, "ID_UNCONNECTED_PING", "stock; not used by the game session"},
    {0x02, "ID_UNCONNECTED_PING_OPEN_CONNECTIONS", "stock"},
    {0x03, "ID_CONNECTED_PONG", "25B pong [id][u64][u64][u32 clock]"},
    {0x04, "ID_DETECT_LOST_CONNECTIONS", "stock"},
    {0x05, "ID_OPEN_CONNECTION_REQUEST_1", "stock id; Roblox uses 0x7b"},
    {0x06, "ID_OPEN_CONNECTION_REPLY_1", "stock id; Roblox uses 0x7e"},
    {0x07, "ID_OPEN_CONNECTION_REQUEST_2", "stock id; Roblox uses 0x78"},
    {0x08, "ID_OPEN_CONNECTION_REPLY_2", "stock id; Roblox uses 0x7d"},
    {0x09, "ID_CONNECTION_REQUEST", "09 <guid8> <15B build constant>"},
    {0x0a, "ID_CONNECTION_REQUEST_ACCEPTED", "stock"},
    {0x0b, "ID_NEW_INCOMING_CONNECTION", "stock id; Roblox uses 0x13"},
    {0x0c, "ID_NO_FREE_INCOMING_CONNECTIONS", "stock"},
    {0x0d, "ID_DISCONNECTION_NOTIFICATION", "stock id; Roblox uses 0x15"},
    {0x0e, "ID_CONNECTION_LOST", "stock"},
    {0x0f, "ID_CONNECTION_BANNED", "stock"},
    {0x10, "ID_INVALID_PASSWORD", "Roblox: RakNetPasswordAccepted + address list"},
    {0x11, "ID_PONG", "stock"},
    {0x12, "ID_ADVERTISE_SYSTEM", "stock"},
    {0x13, "ID_REMOTE_SYSTEM_REQUIRES_PUBLIC_KEY",
     "Roblox: ID_NEW_INCOMING_CONNECTION (102B, 11 addresses + 24B tail)"},
    {0x14, "ID_OUR_SYSTEM_REQUIRES_SECURITY", "stock"},
    {0x15, "ID_PUBLIC_KEY_MISMATCH",
     "Roblox: ID_DISCONNECTION_NOTIFICATION [u16 0][u16 BE code]"},
    {0x16, "ID_OUT_OF_BAND_INTERNAL", "stock"},
    {0x17, "ID_SND_RECEIPT_ACKED", "stock"},
    {0x18, "ID_SND_RECEIPT_LOSS", "stock"},
    {0x19, "ID_INCOMPATIBLE_PROTOCOL_VERSION", "stock"},
    {0x1a, "ID_IP_RECENTLY_CONNECTED", "stock"},
    {0x1b, "ID_TIMESTAMP", "stock"},
    {0x1c, "ID_UNCONNECTED_PONG", "stock"},
    {0x51, "ID_CLOCK_PING_51", "9B [id][u32 0][u24 clock]"},
    {0x52, "ID_CLOCK_PING_52", "9B, same layout as 0x51"},
    {0x53, "ID_CLOCK_PING_53", "9B, same layout as 0x51"},
    {0x54, "ID_CLOCK_PING_54", "25B [id][u64][u64][u32 clock]"},
    {0x55, "ID_CLOCK_PING_55", "25B, same layout as 0x54"},
    {0x56, "ID_CLOCK_PING_56", "25B, same layout as 0x54"},
    {0x78, "OPEN_CONNECTION_REQUEST_2", "Roblox encrypted Req2 (RUPP subtype 1)"},
    {0x7b, "OPEN_CONNECTION_REQUEST_1", "Roblox Req1: 1129B payload, RUPP 1160B"},
    {0x7d, "OPEN_CONNECTION_REPLY_2", "Roblox encrypted Reply2 (verifies the join)"},
    {0x7e, "OPEN_CONNECTION_REPLY_1", "Roblox Reply1: guid8 + security + mtu BE16"},
    {0x81, "APP_PEER_INIT", "app: peer assignment/init, sent once"},
    {0x83, "APP_DATA", "app: replication container; subtype = payload[1]"},
    {0x84, "APP_SEND_MARKER", "app: snapshot marker; client answers 0x04 + id"},
    {0x8a, "APP_TICKET_SUBMIT", "app: sealed ClientTicket + session JSON"},
    {0x8d, "APP_REGION_CHUNK", "app: streamed terrain/region chunk"},
    {0x8f, "APP_SETTINGS_ACK", "app: 2B settings acknowledgement"},
    {0x90, "APP_CLIENT_INFO", "app: sealed join document"},
    {0x92, "APP_NONCE", "app: LEB zigzag nonce"},
    {0x93, "APP_NETWORK_SETTINGS", "app: RCC override list; join waits for it"},
    {0x95, "APP_SECURITY_KEY_REJECTED", "app: RUPP/token mismatch; terminal"},
    {0x96, "APP_GAME_LOADED", "app: [96 00] idle / [96 01 08 000000] flag form"},
    {0x97, "APP_JOIN_DATA", "app: zstd join schema + snapshot"},
    {0x99, "APP_CAPABILITIES", "app: one-shot capability report"},
    {0x9b, "APP_CHALLENGE", "app: anti-cheat challenge and our 9B answer"},
    {0xa0, "ID_NEGATIVE_ACK", "NAK (no datagram id)"},
    {0xa1, "APP_CAMERA",
     "app: [a1][u32 clock] + 39B camera body (client->server); server->client "
     "0xA1 is a ~1KB character-state stream"},
    {0xa4, "APP_AUTH", "app: join-progress auth (not yet reproduced)"},
    {0xa6, "APP_INPUT", "app: ControlState input stream"},
    {0xa7, "APP_SCRIPT_HASHES", "app: [a7][u16 count][16B hashes]"},
    {0xa8, "APP_EARLY_AUTH", "app: version + base64 pre/auth blobs (RbxTransport)"},
    {0xa9, "APP_COMPRESSED", "app: compressed-packet wrapper recognized by the native"},
    {0xc0, "ID_ACK", "ACK (no datagram id; server form 0xd0)"},
    {0xd0, "ID_ACK_SERVER", "server ACK flag form"},
};

struct SubEntry {
  uint8_t sub;
  const char* name;
};
// Every application id in 0x81..0xa9. Entries with a null name are the ids
// that no live/native capture in this tree has produced; keeping them here
// makes the coverage table complete without giving them invented semantics.
const AppPacketInfo kAppPackets[] = {
    {0x81, "APP_PEER_INIT", AppDirection::ServerToClient, true, false,
     "peer assignment/init"},
    {0x82, nullptr, AppDirection::None, false, false, "unobserved"},
    {0x83, "APP_DATA", AppDirection::Both, true, true,
     "[83][subtype][items...][00] container"},
    {0x84, "APP_SEND_MARKER", AppDirection::ServerToClient, true, false,
     "[84][u32 marker]; client answers 04 + marker"},
    {0x85, nullptr, AppDirection::None, false, false, "unobserved"},
    {0x86, nullptr, AppDirection::None, false, false, "unobserved"},
    {0x87, nullptr, AppDirection::None, false, false, "unobserved"},
    {0x88, nullptr, AppDirection::None, false, false, "unobserved"},
    {0x89, nullptr, AppDirection::None, false, false, "unobserved"},
    {0x8a, "APP_TICKET_SUBMIT", AppDirection::ClientToServer, false, true,
     "LEB uid + ticket + session JSON + C001CAFE"},
    {0x8b, nullptr, AppDirection::None, false, false, "unobserved"},
    {0x8c, nullptr, AppDirection::None, false, false, "unobserved"},
    {0x8d, "APP_REGION_CHUNK", AppDirection::ServerToClient, true, false,
     "streamed terrain/region chunk"},
    {0x8e, nullptr, AppDirection::None, false, false, "unobserved"},
    {0x8f, "APP_SETTINGS_ACK", AppDirection::ClientToServer, false, true,
     "[8f][u16 BE string length][server id]"},
    {0x90, "APP_CLIENT_INFO", AppDirection::ClientToServer, false, true,
     "9-byte prefix + sealed JSON join document"},
    {0x91, nullptr, AppDirection::None, false, false, "unobserved"},
    {0x92, "APP_NONCE", AppDirection::ClientToServer, false, true,
     "[92][zigzag LEB nonce]"},
    {0x93, "APP_NETWORK_SETTINGS", AppDirection::ServerToClient, true, false,
     "[93][u8 flags][u16 count]([u16 nlen][name][u16 vlen][value])*"},
    {0x94, nullptr, AppDirection::None, false, false, "unobserved"},
    {0x95, "APP_SECURITY_KEY_REJECTED", AppDirection::ServerToClient, false,
     false, "terminal RUPP/token rejection"},
    {0x96, "APP_GAME_LOADED", AppDirection::ClientToServer, false, true,
     "[96][mode]; 00 idle / 01 u32 flags"},
    {0x97, "APP_JOIN_DATA", AppDirection::ServerToClient, true, false,
     "zstd join schema + snapshot"},
    {0x98, nullptr, AppDirection::None, false, false, "unobserved"},
    {0x99, "APP_CAPABILITIES", AppDirection::ClientToServer, false, true,
     "14-byte capability report"},
    {0x9a, nullptr, AppDirection::None, false, false, "unobserved"},
    {0x9b, "APP_CHALLENGE", AppDirection::Both, true, true,
     "server challenge blob / 9-byte answer"},
    {0x9c, nullptr, AppDirection::None, false, false, "unobserved"},
    {0x9d, nullptr, AppDirection::None, false, false, "unobserved"},
    {0x9e, nullptr, AppDirection::None, false, false, "unobserved"},
    {0x9f, nullptr, AppDirection::None, false, false, "unobserved"},
    {0xa0, "ID_NEGATIVE_ACK", AppDirection::Transport, true, false,
     "transport NAK, not an application message"},
    {0xa1, "APP_CAMERA", AppDirection::Both, true, true,
     "39B camera body / 17B client-state envelope + records"},
    {0xa2, nullptr, AppDirection::None, false, false, "unobserved"},
    {0xa3, nullptr, AppDirection::None, false, false, "unobserved"},
    {0xa4, "APP_AUTH", AppDirection::Both, false, false,
     "join-progress auth; payload not recovered"},
    {0xa5, nullptr, AppDirection::None, false, false, "unobserved"},
    {0xa6, "APP_INPUT", AppDirection::ClientToServer, false, true,
     "[a6][count] ControlState + route step pairs"},
    {0xa7, "APP_SCRIPT_HASHES", AppDirection::ClientToServer, false, true,
     "[a7][u16 count][count x 16B hashes]"},
    {0xa8, "APP_EARLY_AUTH", AppDirection::ClientToServer, false, true,
     "[a8][version][u8 len][pre][u8 len][auth]"},
    {0xa9, "APP_COMPRESSED", AppDirection::ServerToClient, false, false,
     "native compressed-packet wrapper; format not decoded here"},
};

const SubEntry kSubs[] = {
    {0x01, "delete-instance"},
    {0x02, "spawn-instance"},
    {0x03, "property-triples"},
    {0x04, "player-attributes"},
    {0x05, "data-heartbeat"},
    {0x06, "instance-ack-records"},
    {0x07, "event"},
    {0x0a, "id-pairs"},
    {0x0b, "entry-batch"},
    {0x0d, "records"},
    {0x0e, "client-replication"},
    {0x16, "snapshot-batch"},
    {0x1b, "entry-batch-1b"},
    {0x1c, "client-replication-1c"},
    {0x20, "batch-20"},
    {0x23, "clock"},
    {0x25, "entry-batch-25"},
    {0x78, "script-bulk"},
};

} // namespace

const PacketInfo* packet_info(uint8_t id) {
  for (const PacketInfo& p : kPackets)
    if (p.id == id) return &p;
  return nullptr;
}

const char* packet_name(uint8_t id) {
  const PacketInfo* p = packet_info(id);
  return p ? p->name : nullptr;
}

bool is_internal_packet(uint8_t id) {
  // Transport-owned ids: handshake, keepalives, disconnect, ACK/NAK.
  if (id <= 0x1c) return true;
  if (id >= 0x51 && id <= 0x56) return true;  // clock pings
  if (id == 0x78 || id == 0x7b || id == 0x7d || id == 0x7e) return true;
  return id == 0xa0 || id == 0xc0 || id == 0xd0;
}

bool is_app_message(uint8_t id) {
  // 0xa0 is the transport NAK that happens to share this numeric range. The
  // range extends to 0xa9: the native checks 0xa9 as a compressed wrapper and
  // 0xa8 is a real early-auth builder, so stopping at 0xa7 hid two ids.
  return id >= 0x81 && id <= 0xa9 && id != 0xa0;
}

const char* app_direction_name(AppDirection d) {
  switch (d) {
  case AppDirection::ClientToServer: return "c2s";
  case AppDirection::ServerToClient: return "s2c";
  case AppDirection::Both: return "both";
  case AppDirection::Transport: return "transport";
  case AppDirection::None: return "none";
  }
  return "none";
}

const AppPacketInfo* app_packet_info(uint8_t id) {
  for (const AppPacketInfo& p : kAppPackets)
    if (p.id == id) return &p;
  return nullptr;
}

bool app_can_receive(uint8_t id) {
  const AppPacketInfo* p = app_packet_info(id);
  return p && p->receive_decoded;
}

bool app_can_send(uint8_t id) {
  const AppPacketInfo* p = app_packet_info(id);
  return p && p->send_buildable;
}

bool parseNetworkSettings(const uint8_t* d, size_t n, NetworkSettings* out,
                          std::string* err) {
  if (!d || !out) {
    if (err) *err = "not a 0x93 network-settings message";
    return false;
  }
  auto u16 = [](const uint8_t* p) {
    return uint16_t(p[0] | (uint16_t(p[1]) << 8));
  };
  // Live callers pass the payload after the app id; offline captures and the
  // unit vector include the id. Accept both, but require flags+count.
  size_t p = (n >= 4 && d[0] == 0x93) ? 1 : 0;
  if (n - p < 3) {
    if (err) *err = "truncated 0x93 settings header";
    return false;
  }
  out->flags = d[p];
  const uint16_t count = u16(d + p + 1);
  p += 3;
  out->settings.clear();
  out->settings.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    if (p + 2 > n) {
      if (err) *err = "truncated 0x93 name length at entry " + std::to_string(i);
      return false;
    }
    const uint16_t nlen = u16(d + p);
    p += 2;
    if (p + nlen + 2 > n) {
      if (err) *err = "truncated 0x93 name at entry " + std::to_string(i);
      return false;
    }
    NetworkSetting s;
    s.name.assign(reinterpret_cast<const char*>(d + p), nlen);
    p += nlen;
    const uint16_t vlen = u16(d + p);
    p += 2;
    if (p + vlen > n) {
      if (err) *err = "truncated 0x93 value at entry " + std::to_string(i);
      return false;
    }
    s.value.assign(reinterpret_cast<const char*>(d + p), vlen);
    p += vlen;
    out->settings.push_back(std::move(s));
  }
  if (p != n) {
    if (err) *err = "0x93 has " + std::to_string(n - p) + " trailing bytes";
    return false;
  }
  return true;
}

std::string app_manifest() {
  std::string out = "app id  dir        recv send  name | layout\n";
  char b[160];
  for (int raw = 0x81; raw <= 0xa9; ++raw) {
    const uint8_t id = uint8_t(raw);
    const AppPacketInfo* p = app_packet_info(id);
    std::snprintf(b, sizeof b, "0x%02x    %-9s  %-4s %-4s  ", unsigned(id),
                  p ? app_direction_name(p->direction) : "none",
                  p && p->receive_decoded ? "yes" : "no",
                  p && p->send_buildable ? "yes" : "no");
    out += b;
    out += p && p->name ? p->name : "(unobserved)";
    out += " | ";
    out += p && p->layout ? p->layout : "no capture; no named codec";
    out += '\n';
  }
  return out;
}

const char* app_subtype_name(uint8_t sub) {
  for (const SubEntry& e : kSubs)
    if (e.sub == sub) return e.name;
  return nullptr;
}

// --- census ----------------------------------------------------------------

namespace {

std::string join_map(const std::map<uint8_t, uint64_t>& m) {
  std::string s;
  char b[32];
  for (const auto& kv : m) {
    if (!s.empty()) s += ',';
    std::snprintf(b, sizeof b, "%02x:%llu", unsigned(kv.first),
                  (unsigned long long)kv.second);
    s += b;
  }
  return s.empty() ? "-" : s;
}

} // namespace

void Census::note_datagram(const DatagramHeader& h) {
  ++datagrams;
  switch (h.kind) {
  case DatagramKind::Data: ++data_datagrams; break;
  case DatagramKind::Ack: ++ack_datagrams; break;
  case DatagramKind::Nak: ++nak_datagrams; break;
  case DatagramKind::Invalid: ++malformed; break;
  }
}

void Census::note_internal(uint8_t id) {
  ++internal[id];
  const PacketInfo* p = packet_info(id);
  if (!p || !p->name) note_unknown(id, false);
}

void Census::note_app(uint8_t id, bool has_sub, uint8_t sub) {
  ++app[id];
  if (has_sub) ++app_sub[sub];
  // An app id with no named packet entry is unknown even when it sits inside
  // 0x81..0xa9. The old check also required !is_app_message(id), which made
  // every unnamed app id invisible to unk_app forever.
  const PacketInfo* p = packet_info(id);
  if (!p || !p->name) note_unknown(id, true);
}

void Census::note_unknown(uint8_t id, bool app) {
  ++unknown[id];
  if (app)
    ++unknown_app;
  else
    ++unknown_internal;
}

std::string Census::summary() const {
  std::string s;
  char b[160];
  std::snprintf(b, sizeof b,
                "census dgrams=%llu data=%llu acks=%llu naks=%llu msgs=%llu "
                "malformed=%llu unk_internal=%llu unk_app=%llu",
                (unsigned long long)datagrams,
                (unsigned long long)data_datagrams,
                (unsigned long long)ack_datagrams,
                (unsigned long long)nak_datagrams,
                (unsigned long long)messages,
                (unsigned long long)malformed,
                (unsigned long long)unknown_internal,
                (unsigned long long)unknown_app);
  s = b;
  s += " internal=" + join_map(internal);
  s += " app=" + join_map(app);
  s += " sub=" + join_map(app_sub);
  if (!unknown.empty()) s += " unknown=" + join_map(unknown);
  return s;
}

// --- bit stream ------------------------------------------------------------

void BitWriter::put_bit(bool bit) {
  if ((bits_ & 7) == 0) buf_.push_back(0);
  if (bit) buf_.back() |= uint8_t(0x80u >> (bits_ & 7));
  ++bits_;
}

void BitWriter::put_bits(uint64_t value, unsigned bits) {
  for (unsigned i = bits; i-- > 0;) put_bit(((value >> i) & 1) != 0);
}

void BitWriter::put_bytes(const uint8_t* data, size_t len) {
  if (!data) return;
  for (size_t i = 0; i < len; ++i) put_bits(data[i], 8);
}

void BitWriter::put_u16_le(uint16_t v) {
  put_bits(uint64_t(v & 0xff), 8);
  put_bits(uint64_t(v >> 8), 8);
}

void BitWriter::put_u24_le(uint32_t v) {
  for (int i = 0; i < 3; ++i) put_bits((v >> (8 * i)) & 0xff, 8);
}

void BitWriter::put_u32_le(uint32_t v) {
  for (int i = 0; i < 4; ++i) put_bits((v >> (8 * i)) & 0xff, 8);
}

void BitWriter::put_u64_le(uint64_t v) {
  for (int i = 0; i < 8; ++i) put_bits((v >> (8 * i)) & 0xff, 8);
}

void BitWriter::put_string(const std::string& s) {
  const size_t n = s.size() > 65535 ? 65535 : s.size();
  put_bits(n, 16);
  put_bytes(reinterpret_cast<const uint8_t*>(s.data()), n);
}

void BitWriter::align() {
  while (bits_ & 7) put_bit(false);
}

bool BitReader::get_bit(bool* out) {
  if (!out || bit_ >= size_bits_) {
    ok_ = false;
    return false;
  }
  *out = ((data_[bit_ >> 3] >> (7 - (bit_ & 7))) & 1) != 0;
  ++bit_;
  return true;
}

bool BitReader::get_bits(unsigned bits, uint64_t* out) {
  if (!out || bits > 64 || remaining_bits() < bits) {
    ok_ = false;
    return false;
  }
  uint64_t v = 0;
  for (unsigned i = 0; i < bits; ++i) {
    bool b = false;
    get_bit(&b);
    v = (v << 1) | (b ? 1u : 0u);
  }
  *out = v;
  return true;
}

bool BitReader::get_bytes(uint8_t* out, size_t len) {
  if (!out) return false;
  for (size_t i = 0; i < len; ++i) {
    uint64_t v = 0;
    if (!get_bits(8, &v)) return false;
    out[i] = uint8_t(v);
  }
  return true;
}

bool BitReader::get_u8(uint8_t* out) {
  uint64_t v = 0;
  if (!get_bits(8, &v)) return false;
  *out = uint8_t(v);
  return true;
}

bool BitReader::get_u16_be(uint16_t* out) {
  uint64_t v = 0;
  if (!get_bits(16, &v)) return false;
  *out = uint16_t(v);
  return true;
}

bool BitReader::get_u16_le(uint16_t* out) {
  uint8_t b[2];
  if (!get_bytes(b, 2)) return false;
  *out = uint16_t(uint16_t(b[0]) | (uint16_t(b[1]) << 8));
  return true;
}

bool BitReader::get_u24_le(uint32_t* out) {
  uint8_t b[3];
  if (!get_bytes(b, 3)) return false;
  *out = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16);
  return true;
}

bool BitReader::get_u32_be(uint32_t* out) {
  uint64_t v = 0;
  if (!get_bits(32, &v)) return false;
  *out = uint32_t(v);
  return true;
}

bool BitReader::get_u32_le(uint32_t* out) {
  uint8_t b[4];
  if (!get_bytes(b, 4)) return false;
  *out = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) |
         (uint32_t(b[3]) << 24);
  return true;
}

bool BitReader::get_u64_be(uint64_t* out) { return get_bits(64, out); }

bool BitReader::get_u64_le(uint64_t* out) {
  uint8_t b[8];
  if (!get_bytes(b, 8)) return false;
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | b[i];
  *out = v;
  return true;
}

bool BitReader::get_string(std::string* out, size_t max_len) {
  uint16_t n = 0;
  if (!out || !get_u16_be(&n) || n > max_len || remaining_bits() < size_t(n) * 8) {
    ok_ = false;
    return false;
  }
  out->assign(n, '\0');
  return get_bytes(reinterpret_cast<uint8_t*>(&(*out)[0]), n);
}

bool BitReader::align() {
  const size_t rem = bit_ & 7;
  if (!rem) return true;
  const unsigned pad = unsigned(8 - rem);
  uint64_t ignored = 0;
  return get_bits(pad, &ignored);
}

// --- handshake -------------------------------------------------------------

bool parse_reply1(const uint8_t* d, size_t n, Reply1* out) {
  if (!out) return false;
  *out = Reply1{};
  if (!d || n < 28) return false;
  // [0x7e][magic 16][guid 8][security u8][mtu u16 BE]
  static const uint8_t kMagic[16] = {0x00, 0xff, 0xff, 0x00, 0xfe, 0xfe, 0xfe,
                                     0xfe, 0xfd, 0xfd, 0xfd, 0xfd, 0x12, 0x34,
                                     0x56, 0x78};
  if (d[0] != 0x7e || std::memcmp(d + 1, kMagic, sizeof kMagic) != 0)
    return false;
  out->guid = uint64_t(rd16le(d + 17)) | (uint64_t(rd16le(d + 19)) << 16) |
              (uint64_t(rd16le(d + 21)) << 32) |
              (uint64_t(rd16le(d + 23)) << 48);
  out->security = d[25] != 0;
  out->mtu = rd16be(d + 26);
  out->ok = out->mtu != 0;
  return out->ok;
}

} // namespace raknet
} // namespace rbxclient
