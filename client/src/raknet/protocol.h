// protocol.h — the complete RakNet wire vocabulary used by Roblox's RakNet.
//
// This is the single source of truth for the transport layer: packet ids and
// their meanings, the reliability classes, datagram headers, message headers
// (message number / ordering / split), ACK and NAK records, the bit-level
// stream RakNet is defined on, and the per-session packet census.
//
// It is deliberately separate from raknet.cpp (I/O, crypto, session state) and
// rx_stream.cpp (reassembly policy) so every byte layout has one implementation
// that the tests can pin without a socket or a key.
//
// Wire facts (native captures; see FINDINGS.md "RakNet inner protocol"):
//
//   datagram : [flags][u24 datagram id LE] <message>*
//   flags    : bit7 valid, bit6 ACK, bit5 continuous-send.
//              data 0x81, split-fragment datagrams 0x83,
//              client ACK 0xc0, server ACK 0xd0, NAK 0xa0.
//   message  : [reliability][u16 BE bit length]
//              [if reliable: u24 message number LE]
//              [if ordered/sequenced: u24 ordering index LE][u8 channel]
//              [if split: u32 BE count][u16 BE split id][u32 BE index]
//              [payload]
//   reliability: kind = byte >> 5 (0..4), split flag = byte & 0x10.
//   ACK/NAK  : [flags][u16 BE count] { [01][u24 id] | [00][u24 lo][u24 hi] }
//              (the RCC also appends bare u24 ids after the records; parse
//              tolerates that form, which the native client sends.)
//
// Roblox's RakNet is a modified build: the offline handshake uses 0x7b/0x7e/
// 0x78/0x7d instead of stock 0x05/0x06/0x07/0x08, and application messages
// ride the same 0x83 container as stock ID_DATA. Ids 0x83..0xa7 therefore have
// two readings depending on direction; packet_info() names the one this client
// actually sees, and `note` says so where they differ.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace rbxclient {
namespace raknet {

// --- reliability -----------------------------------------------------------

enum class Reliability : uint8_t {
  Unreliable = 0,
  UnreliableSequenced = 1,
  Reliable = 2,
  ReliableOrdered = 3,
  ReliableSequenced = 4,
};

const char* reliability_name(Reliability r);
bool reliability_is_reliable(Reliability r);
bool reliability_has_ordering(Reliability r);

// --- datagrams -------------------------------------------------------------

enum class DatagramKind : uint8_t { Invalid = 0, Data, Ack, Nak };

const char* datagram_kind_name(DatagramKind k);

constexpr uint8_t kFlagData = 0x81;
constexpr uint8_t kFlagDataSplit = 0x83;
constexpr uint8_t kFlagAck = 0xc0;
constexpr uint8_t kFlagAckServer = 0xd0;
constexpr uint8_t kFlagNak = 0xa0;

struct DatagramHeader {
  uint8_t flags = 0;
  DatagramKind kind = DatagramKind::Invalid;
  bool valid = false;          // bit 7
  bool continuous_send = false; // bit 5
  uint32_t datagram_id = 0;    // data datagrams only
  size_t body = 0;             // offset of the first message / record
};

bool parse_datagram_header(const uint8_t* d, size_t n, DatagramHeader* out);

// Writes flags + u24 datagram id. ACK/NAK datagrams carry no datagram id, so
// pass has_id=false for them (their body starts right after the flags byte).
size_t build_datagram_header(uint8_t* out, size_t cap, uint8_t flags,
                             uint32_t datagram_id, bool has_id = true);

// --- messages --------------------------------------------------------------

struct MessageView {
  Reliability reliability = Reliability::Unreliable;
  uint8_t reliability_byte = 0;
  bool split = false;
  uint32_t message_number = 0;
  uint32_t ordering_index = 0;
  uint8_t channel = 0;
  uint32_t split_count = 0;
  uint16_t split_id = 0;
  uint32_t split_index = 0;
  const uint8_t* data = nullptr;
  size_t size = 0;  // ceil(bit_length / 8)
};

// Called for each message in order. Returning false stops the walk (not an
// error). `ok` is false when the datagram ends mid-header or mid-payload and
// `bad_offset` then points at the first byte that could not be decoded.
struct WalkResult {
  bool ok = true;
  size_t messages = 0;
  size_t bad_offset = 0;
  size_t consumed = 0;
};
using MessageFn = std::function<bool(const MessageView&)>;
WalkResult walk_messages(const uint8_t* d, size_t n, const MessageFn& fn);

struct MessageSpec {
  Reliability reliability = Reliability::ReliableOrdered;
  bool split = false;
  uint32_t message_number = 0;
  uint32_t ordering_index = 0;
  uint8_t channel = 0;
  uint32_t split_count = 0;
  uint16_t split_id = 0;
  uint32_t split_index = 0;
  const uint8_t* data = nullptr;
  size_t size = 0;
};

// Complete message header + payload, as it appears inside a datagram.
std::vector<uint8_t> build_message(const MessageSpec& m);
bool append_message(std::vector<uint8_t>* out, const MessageSpec& m);

// --- ACK / NAK -------------------------------------------------------------

struct AckRecord {
  uint32_t lo = 0;
  uint32_t hi = 0;
  bool single() const { return lo == hi; }
};

// Sorted + deduplicated, consecutive ids coalesced into ranges. `kind` must be
// DatagramKind::Ack (0xc0) or DatagramKind::Nak (0xa0).
std::vector<uint8_t> build_ack_datagram(DatagramKind kind,
                                        const std::vector<uint32_t>& ids);

// Accepts both the client's compact form and the RCC's trailing-u24 form.
bool parse_ack_datagram(const uint8_t* d, size_t n, std::vector<AckRecord>* out);

void expand_acks(const std::vector<AckRecord>& records,
                 std::vector<uint32_t>* out);

// --- packet ids ------------------------------------------------------------

struct PacketInfo {
  uint8_t id;
  const char* name;
  const char* note;
};

// nullptr for ids this build has no verified meaning for.
const PacketInfo* packet_info(uint8_t id);
const char* packet_name(uint8_t id);

// True for the transport's own packets (handshake, keepalive, ACK/NAK), false
// for application messages that ride the data stream.
bool is_internal_packet(uint8_t id);
// Direction of an application message. 0xa0 is a transport packet that lives
// inside the same numeric range as the app ids, so it gets its own value here
// instead of being misclassified as an app message.
enum class AppDirection : uint8_t {
  None = 0,
  ClientToServer,
  ServerToClient,
  Both,
  Transport,
};

const char* app_direction_name(AppDirection d);

// Coverage entry for every id in Roblox's app-message range (0x81..0xa9).
// `name == nullptr` means the id has never been observed or named by this
// build; it is reported as unobserved rather than invented. `receive_decoded`
// and `send_buildable` say whether this client has a real codec/parser for the
// direction, not whether the packet merely appears in a capture.
struct AppPacketInfo {
  uint8_t id = 0;
  const char* name = nullptr;
  AppDirection direction = AppDirection::None;
  bool receive_decoded = false;
  bool send_buildable = false;
  const char* layout = nullptr;
};

const AppPacketInfo* app_packet_info(uint8_t id);
bool app_can_receive(uint8_t id);
bool app_can_send(uint8_t id);

// 0x93 RCC network/feature settings: [93][u8 flags][u16 count]
// followed by count * [u16 name_len][name][u16 value_len][value].
// Values are the strings the RCC sends ("true", "false", or a setting
// value); they are kept exactly as bytes, not guessed into bools.
struct NetworkSetting {
  std::string name;
  std::string value;
};
struct NetworkSettings {
  uint8_t flags = 0;
  std::vector<NetworkSetting> settings;
};
bool parseNetworkSettings(const uint8_t* d, size_t n, NetworkSettings* out,
                          std::string* err);

// 0xA9 compressed app packet. Native Client::OnReceive accepts both a bare
// 0xA9 and a 0x1B timestamp envelope with 0xA9 at byte 9. The confirmed body is
// [A9][LEB128 decompressed size][LZ4 block][optional u16 hash]; the hash is a
// native fflag form and is not needed to recover the payload. The returned
// bytes are the decompressed inner application payload, with any timestamp and
// the 0xA9 wrapper removed. Trailing bytes other than the optional 2-byte hash
// are rejected.
bool decompressCompressedApp(const uint8_t* d, size_t n,
                             std::vector<uint8_t>* out, std::string* err);

// 0x86 server->client instance-pair stream. Observed on 189707: a one-byte
// version (0x01), then N records of 11 bytes. Each record carries two u32
// references and three bytes that are constant/observable in the capture.
// The structure is pinned; the semantic name of the pair is not assigned yet,
// so the fields keep their wire names instead of inventing an owner/link role.
struct AppInstancePair {
  uint32_t ref_a = 0;
  uint8_t ref_kind = 0;  // observed 0x01 in every live record
  uint32_t ref_b = 0;
  uint8_t state = 0;     // observed 0x00..0x05
  uint8_t flag = 0;      // observed 0x00/0x01
};
bool parseAppInstancePairs(const uint8_t* d, size_t n,
                           std::vector<AppInstancePair>* out,
                           std::string* err);

// Human-readable coverage table: every 0x81..0xa9 id with its direction and
// codec status. Used by --packet-manifest so "all packets" is inspectable.
std::string app_manifest();

// True for the Roblox application ids (0x81..0xa9, excluding the 0xa0 NAK).
bool is_app_message(uint8_t id);

// Subtype names inside the 0x83 ID_DATA container (payload[1]).
const char* app_subtype_name(uint8_t sub);

// --- census ----------------------------------------------------------------

// Counts every packet the session sees, so "we understand all packets" is a
// number per session instead of an impression. Unknown ids are kept apart from
// known ones and reported; they are never silently dropped.
struct Census {
  uint64_t datagrams = 0;
  uint64_t data_datagrams = 0;
  uint64_t ack_datagrams = 0;
  uint64_t nak_datagrams = 0;
  uint64_t messages = 0;
  uint64_t malformed = 0;
  uint64_t unknown_internal = 0;
  uint64_t unknown_app = 0;
  std::map<uint8_t, uint64_t> internal;
  std::map<uint8_t, uint64_t> app;
  std::map<uint8_t, uint64_t> app_sub;
  std::map<uint8_t, uint64_t> unknown;

  void note_datagram(const DatagramHeader& h);
  void note_internal(uint8_t id);
  void note_app(uint8_t id, bool has_sub = false, uint8_t sub = 0);
  void note_unknown(uint8_t id, bool app);
  void note_malformed() { ++malformed; }
  bool saw_unknown() const { return unknown_internal + unknown_app > 0; }
  std::string summary() const;
};

// --- bit stream ------------------------------------------------------------

// RakNet is defined on a bit stream; Roblox's messages happen to be byte
// aligned today, but the reader/writer stay bit-exact so a future field at a
// non-byte offset cannot silently misparse. Bits are MSB-first within a byte.
class BitWriter {
 public:
  void put_bit(bool bit);
  void put_bits(uint64_t value, unsigned bits);
  void put_bytes(const uint8_t* data, size_t len);
  void put_u8(uint8_t v) { put_bits(v, 8); }
  void put_u16_be(uint16_t v) { put_bits(v, 16); }
  void put_u16_le(uint16_t v);
  void put_u24_le(uint32_t v);
  void put_u32_be(uint32_t v) { put_bits(v, 32); }
  void put_u32_le(uint32_t v);
  void put_u64_be(uint64_t v) { put_bits(v, 64); }
  void put_u64_le(uint64_t v);
  void put_string(const std::string& s);  // u16 BE length + bytes
  void align();                           // zero-pad to the next byte
  const std::vector<uint8_t>& bytes() const { return buf_; }
  size_t bit_size() const { return bits_; }
  void clear() { buf_.clear(); bits_ = 0; }

 private:
  std::vector<uint8_t> buf_;
  size_t bits_ = 0;
};

class BitReader {
 public:
  BitReader(const uint8_t* data, size_t len)
      : data_(data), size_bits_(len * 8) {}
  bool get_bit(bool* out);
  bool get_bits(unsigned bits, uint64_t* out);
  bool get_bytes(uint8_t* out, size_t len);
  bool get_u8(uint8_t* out);
  bool get_u16_be(uint16_t* out);
  bool get_u16_le(uint16_t* out);
  bool get_u24_le(uint32_t* out);
  bool get_u32_be(uint32_t* out);
  bool get_u32_le(uint32_t* out);
  bool get_u64_be(uint64_t* out);
  bool get_u64_le(uint64_t* out);
  bool get_string(std::string* out, size_t max_len = 4096);
  bool align();
  size_t bit_offset() const { return bit_; }
  size_t remaining_bits() const { return bit_ <= size_bits_ ? size_bits_ - bit_ : 0; }
  bool ok() const { return ok_; }

 private:
  const uint8_t* data_;
  size_t size_bits_;
  size_t bit_ = 0;
  bool ok_ = true;
};

// --- handshake -------------------------------------------------------------

// Offline handshake reply1 (0x7e <magic16> <guid8> <security u8> <mtu u16 BE>).
struct Reply1 {
  bool ok = false;
  uint64_t guid = 0;
  uint16_t mtu = 0;
  bool security = false;
};
bool parse_reply1(const uint8_t* d, size_t n, Reply1* out);

} // namespace raknet
} // namespace rbxclient
