// raknet.cpp — Roblox RakNet handshake + RUPP session framing.
#include "raknet/raknet.h"

#include "common/hex.h"
#include "common/rupp.h"
#include "raknet/protocol.h"
#include "raknet/rx_stream.h"
#include "raknet/session_crypto.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <openssl/rand.h>
#include <map>
#include <random>
#include <set>
#include <vector>

namespace rbxclient {

namespace {

constexpr uint8_t kMagic[16] = {0x00, 0xff, 0xff, 0x00, 0xfe, 0xfe, 0xfe, 0xfe,
                                0xfd, 0xfd, 0xfd, 0xfd, 0x12, 0x34, 0x56, 0x78};
constexpr uint8_t kReq1 = 0x7b;   // OpenConnectionRequest1
constexpr uint8_t kReply1 = 0x7e; // OpenConnectionReply1
constexpr uint8_t kReq2 = 0x78;   // OpenConnectionRequest2
constexpr uint8_t kReply2 = 0x7d; // OpenConnectionReply2

const char* idName(uint8_t id) {
  switch (id) {
  case kReq1: return "Req1";
  case kReply1: return "Reply1";
  case kReq2: return "Req2";
  case kReply2: return "Reply2";
  default: return nullptr;
  }
}

// RUPP view: total-prefixed header + TLV area + payload. Accepts the short
// server headers (total 4 or 23) as well as the 31-byte client header.
struct RuppView {
  bool ok = false;
  const uint8_t* payload = nullptr;
  size_t len = 0;
  const uint8_t* tlvs = nullptr;
  size_t tlvlen = 0;
  uint16_t total = 0;
  bool has_token = false;
  uint8_t token[16]{};
  uint8_t token_sub = 0; // 1 = join token, 2 = session token
};

RuppView ruppView(const uint8_t* pkt, size_t len) {
  RuppView v;
  if (len < 4 || pkt[0] != 0x01 || pkt[1] != 0x00) return v;
  const size_t total = (size_t(pkt[2]) << 8) | size_t(pkt[3]);
  if (total < 4 || total > len) return v;
  v.ok = true;
  v.total = uint16_t(total);
  v.tlvs = pkt + 4;
  v.tlvlen = total - 4;
  v.payload = pkt + total;
  v.len = len - total;
  const uint8_t* p = v.tlvs;
  const uint8_t* end = v.tlvs + v.tlvlen;
  while (p + 2 <= end) {
    const uint8_t type = p[0];
    const size_t tlen = p[1];
    if (p + 2 + tlen > end) break;
    if (type == 1 && tlen == 17 && (p[2] == 1 || p[2] == 2)) {
      v.has_token = true;
      v.token_sub = p[2];
      std::memcpy(v.token, p + 3, 16);
    }
    p += 2 + tlen;
  }
  return v;
}

// Client header: TLV1 (type 1, len 17, subtype + 16B token) + optional TLV2
// (type 2, len 6, ip:port). 31 bytes with TLV2 (the native game path), 23
// without.
//
// TLV2 is the RCC this session belongs to: the native capture's `02 06
// 0abe0005 c3b9` (10.190.0.5:50105) is exactly the address its log prints as
// "Joining game ... at 10.190.0.5" plus the port it connects to, i.e. the
// server, not the client. The server validates it even on Req1.
size_t buildClientHeader(uint8_t* out, size_t cap, const uint8_t ip[4],
                         uint16_t port, uint8_t sub, const uint8_t token[16],
                         bool with_rcc) {
  const size_t total = with_rcc ? 31 : 23;
  if (cap < total) return 0;
  out[0] = 0x01;
  out[1] = 0x00;
  out[2] = uint8_t(total >> 8);
  out[3] = uint8_t(total & 0xff);
  out[4] = 0x01;
  out[5] = 0x11;
  out[6] = sub;
  std::memcpy(out + 7, token, 16);
  if (with_rcc) {
    out[23] = 0x02;
    out[24] = 0x06;
    std::memcpy(out + 25, ip, 4);
    out[29] = uint8_t(port >> 8);
    out[30] = uint8_t(port & 0xff);
  }
  return total;
}

} // namespace

struct RaknetClient::Impl {
  explicit Impl(LogFn l) : log(std::move(l)), rng(std::random_device{}()) {}

  LogFn log;
  RaknetConfig cfg;
  int fd = -1;
  sockaddr_in peer{};
  int stage = 0;
  uint16_t mtu = 0;
  uint64_t guid = 0;
  uint8_t session_token[16]{}; // token the server announced (diagnostics only)
  uint8_t own_token[16]{};     // the token WE present after 0x09 (subtype 2)
  bool own_token_live = false; // true from the 0x13 onwards, like the native
  // The server hands the client its post-handshake RUPP token in Reply2
  // (TLV1, subtype 2 — wire_rx_26 of the 1818 capture), and the native's very
  // next datagram uses exactly those 16 bytes. Inventing a random token gets
  // every subtype-2 datagram dropped, so the issued token is the default.
  // RBX_SERVER_TOKEN=0 falls back to a locally generated one for A/B runs.
  bool echo_server_token = true;
  // RBX_TX_TOKEN=join|server|own: which 16-byte token every data datagram
  // carries, independent of the subtype. The pair (subtype, token) is what the
  // RCC's drop keys on, so the two have to be separable to test them.
  int tx_token_mode = -1; // -1 = auto (join token before the switch, then echo)
  int tx_sub_mode = 0;    // RBX_TX_SUB=1|2: force the data subtype
  bool ack_sub1 = false; // RBX_ACK_SUB=1: ACK under subtype 1 after the switch
  int ack_token_mode = 0; // RBX_ACK_TOKEN: 1 = join token, 2 = server's token
  bool have_session = false;
  uint64_t pkts_rx = 0, payloads_rx = 0, bytes_rx = 0, non_rupp = 0;
  uint32_t split_seq = 0;   // split ids, counted like the native (0, 1, 2, ...)
  uint64_t split_tx = 0;    // fragments emitted
  uint64_t dgram_tx = 0;    // datagrams emitted (diagnostics only)
  uint64_t rtx_tx = 0;      // reliable datagrams resent (diagnostics only)
  std::mt19937_64 rng;
  std::map<int, int> id_hist;
  SessionKeys keys;
  bool crypto_ready = false;
  bool reply2_ok_flag = false;
  std::vector<uint8_t> reply2_plain;

  // Post-handshake datagram layer (see raknet.h).
  bool data_ready = false;
  // The server's 0x10 (RakNetPasswordAccepted) body ends with a 24-byte
  // extension.  The native's 0x13 builder (IDA sub_447C0BC) reads three u64
  // from it and echoes the *second* one back as the first u64 of its own
  // 0x13 extension; random bytes there are what the RCC answers with
  // 0x95 SecurityKeyRejected.  Keep the raw bytes so the echo is byte-exact.
  bool have_accept10 = false;
  uint8_t accept10_q2[8]{};
  uint64_t tx_ctr = 0, rx_ctr = 0;
  uint32_t dgram_no = 0, rel_msg = 0, ord_idx = 0;
  uint8_t session_guid[8]{};
  std::vector<uint8_t> last_dgram;
  uint64_t data_rx_ok = 0, data_rx_frames = 0;

  // Receive bookkeeping: reliable datagrams are acked once (0xc0 datagram)
  // and retransmits are dropped before they reach the message layer.
  std::set<uint32_t> seen_dgrams;
  std::vector<uint32_t> pending_acks;
  uint64_t acks_sent = 0, dups_dropped = 0, server_acks = 0;
  int last_app_id = -1;

  // --- NAK: ask the RCC to resend datagrams we never received -------------
  // The receive stream is ordered, so one lost server datagram blocks every
  // later application message on that channel, and RakNet only resends after
  // a negative acknowledgement. This client never generated one: a single
  // dropped packet (WARP/UDP loss) froze replication until the RCC reaped the
  // session, which is the "stays a minute, then disconnects" symptom.
  // The body uses the same record layout as our ACKs (`[count u16 BE]` then
  // `[01][u24 id]` singles), which the RCC accepts on 0xc0.
  bool rx_seq_started = false;
  uint32_t rx_highest = 0;
  std::set<uint32_t> rx_missing;
  std::map<uint32_t, std::chrono::steady_clock::time_point> missing_since;
  uint64_t naks_sent = 0;
  int nak_rounds = 0;
  std::chrono::steady_clock::time_point next_nak{};

  void noteRxDatagram(uint32_t num) {
    if (!rx_seq_started) {
      rx_seq_started = true;
      rx_highest = num;
      return;
    }
    if (num > rx_highest) {
      const uint32_t gap = num - rx_highest;
      // The RCC's datagram id space is not fully visible from here: ids also
      // move on for datagrams that are not data frames under this token. Only
      // a *small* jump is credible packet loss; a wide one is an id range we
      // simply never see, and asking for those would NAK packets that never
      // existed. Measured on a live 1818 session: normal jumps are 1, with
      // occasional 3-4 id holes.
      if (gap > 1 && gap <= 4) {
        const auto now = std::chrono::steady_clock::now();
        for (uint32_t m = rx_highest + 1; m != num; ++m) {
          if (rx_missing.size() >= 8) break;
          rx_missing.insert(m);
          missing_since.emplace(m, now);
        }
      }
      rx_highest = num;
    }
    rx_missing.erase(num);
    missing_since.erase(num);
    if (rx_missing.empty()) nak_rounds = 0;
  }

  void flushNaks() {
    if (!data_ready || rx_missing.empty()) return;
    // RBX_NAK=0 disables negative acks for A/B runs.
    static const bool nak_on = [] {
      const char* e = getenv("RBX_NAK");
      return !(e && *e == '0');
    }();
    if (!nak_on) {
      rx_missing.clear();
      missing_since.clear();
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next_nak) return;
    if (nak_rounds >= 2) {
      // Two NAK rounds went unanswered: the gap is not recoverable (or the id
      // never existed), and repeating it only adds traffic the RCC could
      // reject. Let the ordered stream sort itself out.
      rx_missing.clear();
      missing_since.clear();
      return;
    }
    std::vector<uint32_t> ids;
    for (uint32_t id : rx_missing) {
      auto it = missing_since.find(id);
      if (it == missing_since.end()) continue;
      // A reordered pair must not NAK a datagram that is merely late.
      if (now - it->second >= std::chrono::milliseconds(60)) ids.push_back(id);
    }
    if (ids.empty()) {
      next_nak = now + std::chrono::milliseconds(15);
      return;
    }
    uint8_t d[1600];
    size_t o = 0;
    d[o++] = 0xa0; // negative acknowledgement
    d[o++] = uint8_t((ids.size() >> 8) & 0xff);
    d[o++] = uint8_t(ids.size() & 0xff);
    for (uint32_t id : ids) {
      d[o++] = 0x01;
      d[o++] = uint8_t(id & 0xff);
      d[o++] = uint8_t((id >> 8) & 0xff);
      d[o++] = uint8_t((id >> 16) & 0xff);
    }
    if (!sealAndSend(d, o, /*data=*/false)) return;
    ++naks_sent;
    ++nak_rounds;
    next_nak = now + std::chrono::milliseconds(60);
    if (naks_sent <= 8)
      s_log("raknet: NAK sent for " + std::to_string(ids.size()) +
            " missing datagram(s), highest rx " + std::to_string(rx_highest));
  }

  // Reliable-datagram retransmission for the join. A single lost 0x90
  // fragment leaves a hole in the RCC's ordered stream: it stops ACKing, the
  // 0x8a behind the hole is never processed, and the 0x9b challenge never
  // comes. The native retransmits until ACKed; we re-send the exact same
  // datagram (same id) twice, which the receiver dedupes. Budgeted to the
  // join burst so the post-join 0x83/0x23 clock ticks are not amplified.
  struct PendingTx {
    std::vector<uint8_t> plain;
    std::chrono::steady_clock::time_point due;
    uint32_t dgram_id = 0;  // the id currently on the wire for this entry
    int tries = 0;
  };
  std::vector<PendingTx> rtx;
  int rtx_budget = 64;
  void queueRtx(const uint8_t* p, size_t n) {
    if (rtx_budget <= 0 || n == 0 || n > 1200) return;
    --rtx_budget;
    if (rtx.size() > 160) rtx.erase(rtx.begin());
    PendingTx t;
    t.plain.assign(p, p + n);
    t.dgram_id = n >= 4 ? (uint32_t(p[1]) | (uint32_t(p[2]) << 8) |
                           (uint32_t(p[3]) << 16))
                        : 0u;
    t.due = std::chrono::steady_clock::now() + std::chrono::milliseconds(180);
    rtx.push_back(std::move(t));
  }
  // Drop retransmits whose datagram the server has acknowledged. Without this
  // every reliable message goes out 1 + 5 times no matter what the server says
  // (measured against the native on one job: 0x13/0x92/0x8f/0x9b six times each
  // and 286 clock ticks where the native sent 17), and the RCC eventually gives
  // up with a receive-stream error.
  void ackRtx(uint32_t id) {
    for (size_t i = 0; i < rtx.size();) {
      if (rtx[i].dgram_id == id)
        rtx.erase(rtx.begin() + long(i));
      else
        ++i;
    }
  }
  // Resend every unacknowledged reliable datagram with a *new* datagram
  // number. The reliable message number and split id stay identical, so the
  // receiver reassembles the same message; only the transport envelope is
  // fresh. Replaying the original datagram id (what this used to do) is
  // discarded by the receiver's datagram-level dedupe before the message
  // layer runs, which is why the join fragments that WARP drops were never
  // recovered: measured server ACK ranges covered 3 of 11 join fragments and
  // every same-id retransmit went unacknowledged.
  void pumpRtx() {
    if (rtx.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < rtx.size();) {
      if (now < rtx[i].due) {
        ++i;
        continue;
      }
      const size_t n = rtx[i].plain.size();
      if (n < 4 || n > 1200) {
        rtx.erase(rtx.begin() + long(i));
        continue;
      }
      uint8_t d[1200];
      std::memcpy(d, rtx[i].plain.data(), n);
      d[1] = uint8_t(dgram_no & 0xff);
      d[2] = uint8_t((dgram_no >> 8) & 0xff);
      d[3] = uint8_t((dgram_no >> 16) & 0xff);
      rtx[i].dgram_id = uint32_t(d[1]) | (uint32_t(d[2]) << 8) |
                        (uint32_t(d[3]) << 16);
      sealAndSend(d, n, /*data=*/true, /*advance=*/true);
      ++rtx_tx;
      // The native was measured sending two copies at most, so a third try is
      // already beyond it; five copies per message is what made our TX stream
      // ~6x the native's.
      if (++rtx[i].tries >= 2) {
        rtx.erase(rtx.begin() + long(i));
      } else {
        rtx[i].due = now + std::chrono::milliseconds(250);
        ++i;
      }
    }
  }

  // Application messages: split reassembly + ordering lives in rx; the sink
  // is what an app uses to feed the shared replication parser.
  RaknetRxStream rx;
  RaknetClient::MessageFn message_sink;
  uint64_t app_bytes = 0;
  // Every id seen in either direction, unknown ids kept separate. Persists
  // across resetSession() so a run's census covers all of its rejoins.
  raknet::Census census;

  // --- application keepalive ---------------------------------------------
  // The RCC pings the session with 0x00 (ID_CONNECTED_PING: the 9-byte form
  // and the MTU-padded 1160-byte form), 0x03 pongs and 0x51..0x56 pings, and
  // the native mirrors every one of them (captured TX: 0x00/0x03/padding
  // pairs interleaved with the join messages). Without the mirror the server
  // keeps pinging and never advances the session to the app-level challenge
  // (`0x9b`) that precedes join data, so the whole game stays gated.
  //
  // Ping layouts, from the native capture:
  //   9B  [id][u32 0][u32 now_ms BE]              (0x00, 0x51..0x53)
  //   25B [id][u64 a][u64 b][u64 clock BE]        (0x03, 0x54..0x56)
  //   1160B MTU pad [id][0a 8c @7:9][clock @14:25]
  // We answer the 1160-byte 0x00 pad with a 0x03 pad (the native's pad
  // handshake: client 0x00 -> server 0x00 -> client 0x03 -> server 0x03) and
  // every other ping with the same id, so a reply can never ping-pong.
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  // Last authenticated server datagram: the RCC closes a dead session without
  // any disconnect message, so silence is the signal (ms_since_rx).
  std::chrono::steady_clock::time_point last_rx_time{};
  bool saw_net_settings = false; // server pushed its 0x93 settings list
  std::map<int, std::chrono::steady_clock::time_point> last_kal;
  std::map<int, int> kal_per_id; // replies already sent per keepalive id (cap 2)
  int pad_replies[256] = {0}; // large MTU pads echoed, per pad id
  uint64_t kal_replies = 0;
  uint64_t clock_ticks_rx = 0;
  // Set once the first 0x97/0x83 app message arrives: from then on pad replies
  // are the pad alone, matching the native's steady state.
  bool app_started = false;

  uint32_t nowMs() const {
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count());
  }

  // Mirror one server ping. Returns true when a reply went out.
  bool mirrorKeepalive(const RaknetAppMessage& m) {
    uint8_t out_id = 0;
    if (m.payload.size() == 9 &&
        (m.id == 0x00 || (m.id >= 0x51 && m.id <= 0x53))) {
      out_id = m.id;
    } else if (m.payload.size() == 25 &&
               (m.id == 0x03 || (m.id >= 0x54 && m.id <= 0x56))) {
      out_id = m.id;
    } else if (m.payload.size() > 64 && (m.id == 0x00 || m.id == 0x03)) {
      // MTU pad exchange. The RCC sends a >=1146-byte pad with its own counter
      // in the body and expects the SAME id echoed back verbatim; it then moves
      // on to the next pad id. Answering the 0x00 probe with a 0x03 pad (what
      // this used to do, capped at one reply) never closes the exchange: the
      // RCC re-sends its 0x00 pad every ~11 ms (447 pads in 5 s, measured
      // 2026-10-30) and the session is gone before the join. The native echoes
      // each pad id three times (TX: 3 x 0x00 1146B, 3 x 0x03 1146B).
      out_id = m.id;
      pad_replies[int(out_id)] += 1;
      if (pad_replies[int(out_id)] > 6) return false;
    } else {
      return false;
    }
    const bool big_pad = m.payload.size() > 64;
    // Answer each keepalive id at most twice, like the native: its whole
    // session carries 2 replies per id (0x51-0x53, 0x54-0x56) and 4 pads. A
    // time-throttled mirror instead answers every pad the RCC sends, the RCC
    // answers back, and the session turns into a 1160-byte echo loop (measured
    // later in the same run: 745 datagrams, 850 kB of pads in 10 s) that never
    // reaches the app layer.
    const auto now = std::chrono::steady_clock::now();
    auto& seen = kal_per_id[int(out_id)];
    auto& seen_at = last_kal[int(out_id)];
    if (!big_pad) {
      // Small pings are the session's liveness signal, not a handshake step.
      // The RCC keeps sending them for the whole session and drops a client
      // that stops answering with 0x15/266 ("your connection timed out");
      // measured 2026-11-06, the first session died ~3.5 minutes in, right
      // after the two-reply lifetime cap below had silenced every id. Throttle
      // per id instead (so a burst can never echo) and keep answering for as
      // long as the server asks.
      if (seen_at.time_since_epoch().count() != 0 &&
          now - seen_at < std::chrono::milliseconds(20))
        return false;
      ++seen;
    }
    seen_at = now;
    // Pings and pads carry a 24-bit client clock at [6:9] (decrypted native
    // capture, /tmp/nat_cmp): the server's 9-byte ping is
    //   <id> 00*5 <u24 clock>            e.g. 00 0000000000 417a04
    // and the native answers with its OWN clock, not the server's value:
    //   53 0000000000 001c88
    // A big pad is generated the same way instead of being echoed verbatim.
    // Echoing the server's own bytes back (what this used to do) gives the RCC
    // its own timestamp, so its MTU/time handshake never completes: it keeps
    // probing with 1160-byte pads forever and never sends the 0x9B challenge.
    const uint32_t ms = nowMs();
    const uint32_t clk24 = ms & 0xFFFFFFu;
    std::vector<uint8_t> r = m.payload;
    if (big_pad && r.size() >= 25) {
      // Native pad shape (r201, r259 of the 1818 capture):
      //   00 <00*5> <u24 clock> 00*5 ...
      //   03 <00*5> <u24 clock> 00*5 <u32 clock BE> 06 5f <5 random> ...
      // and zeros to the MTU length the RCC probed with.
      std::fill(r.begin(), r.end(), 0);
      r[0] = out_id;
      r[6] = uint8_t(clk24 >> 16);
      r[7] = uint8_t(clk24 >> 8);
      r[8] = uint8_t(clk24);
      if (out_id == 0x03) {
        r[14] = uint8_t(ms >> 24);
        r[15] = uint8_t(ms >> 16);
        r[16] = uint8_t(ms >> 8);
        r[17] = uint8_t(ms);
        r[18] = 0x06;
        r[19] = 0x5f;
        const uint64_t rnd = rng();
        for (int i = 0; i < 5; ++i) r[20 + i] = uint8_t(rnd >> (8 * i));
      }
    } else if (r.size() == 9) {
      r[0] = out_id;
      r[5] = r[6] = 0;
      r[7] = uint8_t(clk24 >> 8);
      r[8] = uint8_t(clk24);
    } else if (r.size() == 25) {
      r[0] = out_id;
      // The peer's clock stays at [6:9]; ours goes at [14:18], like native.
      r[14] = uint8_t(ms >> 24);
      r[15] = uint8_t(ms >> 16);
      r[16] = uint8_t(ms >> 8);
      r[17] = uint8_t(ms);
    } else {
      r[0] = out_id;
    }
    // 9-byte pings are unreliable on the wire (the server sends them rel 0x00
    // and the native mirrors them as kind 0); keep the transport class intact
    // or the RCC never matches the reply to its probe.
    const bool ok_send = big_pad
                             ? sendJoinMessage(r.data(), r.size(), /*ordered=*/false)
                             : sendUnreliable(r.data(), r.size());
    if (!ok_send) return false;
    // A big pad is answered with the pad *and* its ping trio, all carrying the
    // server's counter (native TX: pad 0x00 ctr 0b46 + 53/52/51 ctr 0b46, next
    // pad 0x00 ctr 0b4c + 53/52/51; pad 0x03 + 56/55/54). Without the trio the
    // RCC keeps re-sending pads and never reaches the app layer — an endless
    // pad exchange is exactly what our failing joins log.
    // The trio only belongs to the connect handshake. Once the app layer is
    // up the native answers a pad with the pad alone: in a 195 s same-job
    // window it sent 0x53 once and 0x54..0x56 five times, while we mirrored
    // every pad forever (48 of each, every 5 s). Those were the only ids we
    // sent that the native never did.
    if (big_pad && r.size() >= 9 && !app_started) {
      const uint8_t pad_id = r[0];
      const uint8_t pings[3] = {uint8_t(pad_id == 0x03 ? 0x56 : 0x53),
                                uint8_t(pad_id == 0x03 ? 0x55 : 0x52),
                                uint8_t(pad_id == 0x03 ? 0x54 : 0x51)};
      for (uint8_t pid : pings) {
        const std::vector<uint8_t> p = {pid, 0x00, 0x00, 0x00, 0x00, 0x00,
                                        uint8_t(clk24 >> 16), uint8_t(clk24 >> 8),
                                        uint8_t(clk24)};
        sendUnreliable(p.data(), p.size());
      }
    }
    ++kal_replies;
    // First ten in full, then every 25th: a long soak needs the running count
    // to prove the mirror really continues (a stopped mirror is exactly what
    // ends a session with 0x15/266), without a line per ping.
    if (kal_replies <= 10 || kal_replies % 25 == 0) {
      std::string ctr;
      if (r.size() > 8)
        ctr = " ctr=" + hex8(r[7]) + hex8(r[8]);
      s_log("raknet: keepalive mirror 0x" + hex8(out_id) + " (" +
            std::to_string(r.size()) + "B" + ctr + ", " +
            std::to_string(kal_replies) + ")");
    }
    if (getenv("RBX_PAD_TRACE") && big_pad) {
      const auto hx = [](const std::vector<uint8_t>& v) {
        std::string s;
        for (size_t i = 0; i < v.size() && i < 24; ++i) s += hex8(v[i]);
        return s;
      };
      s_log("raknet: PADTRACE rx=" + hx(m.payload) + " tx=" + hx(r));
    }
    return true;
  }

  static std::string hex8(uint8_t v) {
    char b[4];
    snprintf(b, sizeof b, "%02x", v);
    return b;
  }

  void s_log(const std::string& m) { if (log) log(m); }

  // Req2 — the encrypted handshake request (native layout, see raknet.h).
  //   78 <magic> 03 <aadlen> <ctlen BE16> <ver BE16> <client pub 32B>
  //   11 <prelen> <pre blob> | plaintext: <caps 8B> <session id 8B> <mtu BE16>
  //   02 04 <~peer ip BE32> <peer port BE16> <authlen> <auth blob>
  // then ciphertext, the 12-byte nonce "UniqueNumber", the 16-byte tag and
  // zero padding out to the probe size (native: 1129B payload / 1160B datagram).
  bool sendReq2() {
    if (fd < 0 || !crypto_ready) return false;
    const size_t pre = cfg.ticket_pre.size();
    const size_t auth = cfg.ticket_auth.size();
    if (pre == 0 || pre > 250 || auth == 0 || auth > 250) return false;
    const size_t aad_len = 57 + pre;
    const size_t pt_len = 27 + auth;
    const size_t nonce_off = aad_len + pt_len;
    if (nonce_off + 28 > cfg.probe_size) return false;
    std::vector<uint8_t> p(cfg.probe_size, 0);
    p[0] = kReq2;
    std::memcpy(p.data() + 1, kMagic, sizeof kMagic);
    p[17] = 0x03;
    p[18] = uint8_t(aad_len);
    p[19] = uint8_t(pt_len >> 8);
    p[20] = uint8_t(pt_len & 0xff);
    p[21] = 0x00;
    p[22] = cfg.protocol_version;
    std::memcpy(p.data() + 23, keys.pub, 32);
    p[55] = 0x11;
    p[56] = uint8_t(pre);
    std::memcpy(p.data() + 57, cfg.ticket_pre.data(), pre);
    uint8_t* pt = p.data() + aad_len;
    std::memcpy(pt, cfg.capability_word, 8);
    bool zero_id = true;
    for (int i = 0; i < 8; ++i)
      if (cfg.session_id[i]) zero_id = false;
    if (zero_id) {
      const uint64_t r = rng();
      std::memcpy(pt + 8, &r, 8);
      std::memcpy(session_guid, &r, 8);
    } else {
      std::memcpy(pt + 8, cfg.session_id, 8);
      std::memcpy(session_guid, cfg.session_id, 8);
    }
    pt[16] = uint8_t(mtu >> 8);
    pt[17] = uint8_t(mtu & 0xff);
    pt[18] = cfg.encryption_mode;
    pt[19] = cfg.application_id;
    const uint8_t* ip = reinterpret_cast<const uint8_t*>(&peer.sin_addr.s_addr);
    for (int i = 0; i < 4; ++i) pt[20 + i] = uint8_t(~ip[i]);
    pt[24] = uint8_t(ntohs(peer.sin_port) >> 8);
    pt[25] = uint8_t(ntohs(peer.sin_port) & 0xff);
    pt[26] = uint8_t(auth);
    std::memcpy(pt + 27, cfg.ticket_auth.data(), auth);
    std::memcpy(p.data() + nonce_off, kHandshakeNonce, 12);
    if (!aeadSeal(keys.tx, p.data(), nonce_off + 28, nonce_off, aad_len)) {
      log("raknet: Req2 AEAD seal failed");
      return false;
    }
    if (!sendWrapped(0x01, cfg.join_token, p.data(), p.size())) return false;
    char extra[96];
    snprintf(extra, sizeof extra,
             "raknet: Req2 sent (%zuB aad=%zu ct=%zu ticket=%zu+%zu)", p.size(),
             aad_len, pt_len, pre, auth);
    log(extra);
    return true;
  }

  // Reply2 — 7d <magic> 01 <aadlen> <server fields> | AAD | ciphertext |
  // nonce | tag, decrypted with our derived rx key. A verifying tag is the
  // server's acceptance of the Req2 (ticket + ephemeral key).
  void handleReply2(const uint8_t* payload, size_t len) {
    stage = 2;
    log("raknet: Reply2 " + std::to_string(len) + "B (encrypted tail)");
    if (!crypto_ready || len < 18 + 28 ||
        std::memcmp(payload + 1, kMagic, sizeof kMagic) != 0) {
      return;
    }
    const size_t aad_len = payload[18];
    if (aad_len + 28 > len) return;
    const size_t nonce_off = len - 28;
    reply2_plain.assign(len - aad_len, 0);
    size_t outlen = 0;
    if (aeadOpen(keys.rx, payload, len, nonce_off, aad_len, reply2_plain.data(),
                 reply2_plain.size(), &outlen)) {
      reply2_plain.resize(outlen);
      reply2_ok_flag = true;
      // Data-layer key: 23 bytes live in the AAD tail at [21:44], the last 9
      // bytes are the start of the Reply2 plaintext (native sub_44871B0 reads
      // a 32-byte key at bit 168 of the reconstructed buffer).
      if (outlen >= 9 && aad_len >= 44) {
        uint8_t peer[32];
        std::memcpy(peer, payload + 21, 23);
        std::memcpy(peer + 23, reply2_plain.data(), 9);
        if (keys.installData(peer)) {
          data_ready = true;
          rtx.clear();
          rtx_budget = 64;
          tx_ctr = rx_ctr = kDataCounterBase;
          log("raknet: datagram keys installed (AES-256-GCM, counter base "
              "LE64(\"UniqueNu\"))");
        }
      }
      std::string hex;
      for (size_t i = 0; i < outlen && i < 64; ++i) {
        char b[4];
        snprintf(b, sizeof b, "%02x", reply2_plain[i]);
        hex += b;
      }
      log("raknet: Reply2 tag verified (session keys agreed); tail " +
          std::to_string(outlen) + "B: " + hex);
    } else {
      reply2_plain.clear();
      log("raknet: Reply2 tag REJECTED (rx key mismatch)");
    }
  }

  // One RakNet datagram: 0x81 <u24 dgram> then one message
  //   0x40 (RELIABLE) <u16 BE bitlen> <u24 LE message number> <data>
  // or 0x60 (RELIABLE_ORDERED) which adds <u24 LE ordering index><u8 channel>.
  // Plaintext TX datagram dump (RBX_RAKNET_TXDUMP=<dir>), the mirror of the
  // RX dump: tools/raknet_timeline.py decodes both so our datagram pattern can
  // be compared with the native capture datagram for datagram.
  // TLV1 token for an outgoing data datagram. RBX_TX_TOKEN overrides the
  // join-token/echo selection so subtype and token can be varied independently.
  const uint8_t* dataToken() const {
    switch (tx_token_mode) {
    case 0: return cfg.join_token;
    case 1: return session_token;
    case 2: return own_token;
    default: return echo_server_token ? session_token : own_token;
    }
  }

  // RBX_TX_SUB=1|2 forces the RUPP subtype of every data datagram, so the
  // (subtype, token) pair can be tested as a matrix instead of one variable at
  // a time.
  uint8_t dataSub() const {
    if (tx_sub_mode) return uint8_t(tx_sub_mode);
    return own_token_live ? 0x02 : 0x01;
  }
  const uint8_t* dataTok(uint8_t sub) const {
    if (tx_token_mode >= 0) return dataToken();
    return sub == 0x02 ? (echo_server_token ? session_token : own_token)
                       : cfg.join_token;
  }

  void dumpTx(const uint8_t* d, size_t n) {
    static const char* dir = getenv("RBX_RAKNET_TXDUMP");
    static int idx = 0;
    if (!dir) return;
    char path[512];
    snprintf(path, sizeof path, "%s/t%06d.bin", dir, idx++);
    if (FILE* f = fopen(path, "wb")) {
      fwrite(d, 1, n, f);
      fclose(f);
    }
  }

  bool sendRaknetDatagram(const uint8_t* data, size_t len, bool ordered = false) {
    if (!data_ready || len == 0 || len > 1200) return false;
    uint8_t d[1600];
    uint8_t* p = d;
    *p++ = 0x81;
    *p++ = uint8_t(dgram_no & 0xff);
    *p++ = uint8_t((dgram_no >> 8) & 0xff);
    *p++ = uint8_t((dgram_no >> 16) & 0xff);
    *p++ = ordered ? 0x60 : 0x40;
    const uint16_t bits = uint16_t(len * 8);
    *p++ = uint8_t(bits >> 8);
    *p++ = uint8_t(bits & 0xff);
    *p++ = uint8_t(rel_msg & 0xff);
    *p++ = uint8_t((rel_msg >> 8) & 0xff);
    *p++ = uint8_t((rel_msg >> 16) & 0xff);
    if (ordered) {
      *p++ = uint8_t(ord_idx & 0xff);
      *p++ = uint8_t((ord_idx >> 8) & 0xff);
      *p++ = uint8_t((ord_idx >> 16) & 0xff);
      *p++ = 0x00; // ordering channel
      ++ord_idx;
    }
    std::memcpy(p, data, len);
    p += len;
    dumpTx(d, size_t(p - d));
    queueRtx(d, size_t(p - d));
    uint8_t frame[1600];
    const size_t flen = datagramSeal(keys.data_tx, tx_ctr, d, size_t(p - d),
                                     frame, sizeof frame);
    if (!flen) return false;
    // RUPP token (TLV1): the join token travels with subtype 1 until Reply2,
    // then every datagram carries OUR OWN 16 random bytes under subtype 2.
    //
    // A fresh native capture (wire_rx + wire_tx from one session,
    // tools/gdb/rc_capture.gdb + a recvfrom hook) shows both sides announce a
    // *private* token and rotate it every few seconds: client tokens
    // 1511cfad…, abf40b45…, 3dc869f5…, 52f7f102…; server tokens b4c84eb2…,
    // 8152bda8…, 50a102ca… — never equal. The token is not the peer's token
    // echoed back (echoing Reply2's gets every datagram dropped), it is a local
    // identity the sender generates. Subtype 2 is what marks a datagram as
    // belonging to the established session; the native never sends anything
    // else after its first three datagrams.
    const uint8_t sub = dataSub();
    const uint8_t* tok = dataTok(sub);
    ++tx_ctr;
    ++dgram_no;
    ++dgram_tx;
    ++rel_msg;
    return sendWrapped(sub, tok, frame, flen);
  }

  // Unreliable application message (RakNet kind 0): [81][u24 dgram][00]
  // [u16 BE bits][data]. No message number and no ordering header, which is
  // how the native sends 0xA6 input and the ping/pad mirrors.
  bool sendUnreliable(const uint8_t* data, size_t len) {
    if (!data_ready || len == 0 || len > 1200) return false;
    uint8_t d[1600];
    uint8_t* p = d;
    *p++ = 0x81;
    *p++ = uint8_t(dgram_no & 0xff);
    *p++ = uint8_t((dgram_no >> 8) & 0xff);
    *p++ = uint8_t((dgram_no >> 16) & 0xff);
    *p++ = 0x00; // unreliable
    const uint16_t bits = uint16_t(len * 8);
    *p++ = uint8_t(bits >> 8);
    *p++ = uint8_t(bits & 0xff);
    std::memcpy(p, data, len);
    p += len;
    dumpTx(d, size_t(p - d));
    uint8_t frame[1600];
    const size_t flen = datagramSeal(keys.data_tx, tx_ctr, d, size_t(p - d),
                                     frame, sizeof frame);
    if (!flen) return false;
    const uint8_t sub = dataSub();
    const uint8_t* tok = dataTok(sub);
    ++tx_ctr;
    ++dgram_no;
    ++dgram_tx;
    return sendWrapped(sub, tok, frame, flen);
  }

  // ACK datagram (native 0xc0): flag, count u16 BE, then [minEqualsMax u8,
  // min u24 LE, max u24 LE] per range. Payload is transport-only, but it is
  // sealed and counter-tracked exactly like any other datagram.
  //
  // `data` controls the RakNet datagram number. ACK/NAK datagrams carry no
  // datagram id on the wire (their first byte is the 0xc0 flag), so they must
  // not consume one either: the native's 0x13 is datagram 1 straight after the
  // 0x09, while ours was 2 because an ACK took id 1. The RCC tracks datagram
  // ids as a sequence, so the hole stalls everything behind it.
  bool sealAndSend(const uint8_t* plain, size_t len, bool data = true,
                   bool advance = true) {
    dumpTx(plain, len);
    uint8_t frame[1600];
    const size_t flen = datagramSeal(keys.data_tx, tx_ctr, plain, len, frame,
                                     sizeof frame);
    if (!flen) return false;
    const uint8_t sub = dataSub();
    const uint8_t* tok = dataTok(sub);
    ++tx_ctr;
    if (data && advance) ++dgram_no;
    ++dgram_tx;
    // RBX_ACK_SUB=1: send ACKs under subtype 1 with the join token even after
    // the switch. If the RCC then stops retransmitting its 0x10, our subtype-2
    // ACKs were being dropped on their own — i.e. the RCC validates the token
    // (or the subtype) rather than the ACK body.
    if (!data && ack_sub1) return sendWrapped(0x01, cfg.join_token, frame, flen);
    // RBX_ACK_TOKEN=join|server: keep subtype 2 but change which token the ACK
    // carries, so the retransmission count isolates token from subtype.
    if (!data) {
      if (ack_token_mode == 1) return sendWrapped(0x02, cfg.join_token, frame, flen);
      if (ack_token_mode == 2) return sendWrapped(0x02, session_token, frame, flen);
    }
    return sendWrapped(sub, tok, frame, flen);
  }

  // One fragment of a split message. Native layout (verified against the live
  // 0x90/0x8a captures): 0x83 <u24 dgram> 0x70 <u16 BE bits> <u24 msg>
  // <u24 ordering index> <u08 channel> <u32 BE count> <u16 BE split id>
  // <u32 BE index> <data>.
  bool sendSplitFragment(const uint8_t* data, size_t len, uint32_t count,
                         uint16_t split_id, uint32_t index, uint32_t ord,
                         bool ordered, uint32_t msgno) {
    if (len == 0 || len > 1200) return false;
    uint8_t d[1600];
    uint8_t* p = d;
    *p++ = 0x83;
    *p++ = uint8_t(dgram_no & 0xff);
    *p++ = uint8_t((dgram_no >> 8) & 0xff);
    *p++ = uint8_t((dgram_no >> 16) & 0xff);
    *p++ = ordered ? 0x70 : 0x50;
    const uint16_t bits = uint16_t(len * 8);
    *p++ = uint8_t(bits >> 8);
    *p++ = uint8_t(bits & 0xff);
    *p++ = uint8_t(msgno & 0xff);
    *p++ = uint8_t((msgno >> 8) & 0xff);
    *p++ = uint8_t((msgno >> 16) & 0xff);
    if (ordered) {
      *p++ = uint8_t(ord & 0xff);
      *p++ = uint8_t((ord >> 8) & 0xff);
      *p++ = uint8_t((ord >> 16) & 0xff);
      *p++ = 0x00; // ordering channel
    }
    *p++ = uint8_t(count >> 24);
    *p++ = uint8_t(count >> 16);
    *p++ = uint8_t(count >> 8);
    *p++ = uint8_t(count);
    *p++ = uint8_t(split_id >> 8);
    *p++ = uint8_t(split_id);
    *p++ = uint8_t(index >> 24);
    *p++ = uint8_t(index >> 16);
    *p++ = uint8_t(index >> 8);
    *p++ = uint8_t(index);
    std::memcpy(p, data, len);
    p += len;
    queueRtx(d, size_t(p - d));
    return sealAndSend(d, size_t(p - d));
  }

  // Reliable-ordered message, fragmented when it does not fit one datagram.
  //
  // Split fragments go out paced (RBX_SPLIT_PACE_MS, default 2 ms). An 11-part
  // 0x90 blasted into the socket back-to-back loses the session on this path
  // about one run in three: the RCC stops ACKing mid-burst and then never
  // issues the 0x9B, i.e. it reads a hole and gives up on the stream. The
  // native writes its fragments through a queue that drains over ~20 ms, so a
  // small pace matches it and costs 20 ms on the join.
  int sendJoinMessage(const uint8_t* msg, size_t len, bool ordered) {
    static constexpr size_t kFrag = 1061; // native split size (two sessions)
    if (!data_ready || len == 0) return 0;
    if (len <= kFrag) return sendRaknetDatagram(msg, len, ordered) ? 1 : 0;
    const uint32_t count = uint32_t((len + kFrag - 1) / kFrag);
    const uint16_t split_id = split_seq++;
    const uint32_t ord = ordered ? ord_idx++ : 0;
    int pace_ms = 2;
    if (const char* p = getenv("RBX_SPLIT_PACE_MS")) pace_ms = atoi(p);
    for (uint32_t i = 0; i < count; ++i) {
      const size_t off = size_t(i) * kFrag;
      const size_t n = std::min(kFrag, len - off);
      if (!sendSplitFragment(msg + off, n, count, split_id, i, ord, ordered,
                             rel_msg + i))
        return 0;
      ++split_tx;
      if (pace_ms > 0 && i + 1 < count)
        std::this_thread::sleep_for(std::chrono::milliseconds(pace_ms));
    }
    rel_msg += count;
    return int(count);
  }

  bool flushAcks() {
    if (!data_ready || pending_acks.empty()) return false;
    std::sort(pending_acks.begin(), pending_acks.end());
    pending_acks.erase(std::unique(pending_acks.begin(), pending_acks.end()),
                       pending_acks.end());
    struct Range {
      uint32_t lo, hi;
    };
    std::vector<Range> ranges;
    for (uint32_t n : pending_acks) {
      if (!ranges.empty() && n == ranges.back().hi + 1) ranges.back().hi = n;
      else ranges.push_back({n, n});
    }
    uint8_t d[1600];
    size_t o = 0;
    d[o++] = 0xc0; // the native's ACK flag byte (0xd0 is the server's form)
    d[o++] = uint8_t((ranges.size() >> 8) & 0xff);
    d[o++] = uint8_t(ranges.size() & 0xff);
    // Native layout, byte-for-byte (every captured ACK, /tmp/rkc7 tx):
    //   c0 <count u16 BE> { [01][id u24] | [00][lo u24][hi u24] }
    //
    // The byte after the count is a *type tag*, not a "min equals max" flag:
    // 01 announces one id, 00 announces an inclusive range. Writing a range
    // with the 01 tag (what this used to do) leaves the server parsing the
    // trailing lo/hi bytes as further entries, so the whole ACK is dropped and
    // the RCC retransmits its 0x93 forever — which is exactly what stalled the
    // join. Keep the entries in ascending order with no overlap.
    for (const Range& r : ranges) {
      if (r.lo == r.hi) {
        d[o++] = 0x01;
        d[o++] = uint8_t(r.lo & 0xff);
        d[o++] = uint8_t((r.lo >> 8) & 0xff);
        d[o++] = uint8_t((r.lo >> 16) & 0xff);
      } else {
        d[o++] = 0x00;
        d[o++] = uint8_t(r.lo & 0xff);
        d[o++] = uint8_t((r.lo >> 8) & 0xff);
        d[o++] = uint8_t((r.lo >> 16) & 0xff);
        d[o++] = uint8_t(r.hi & 0xff);
        d[o++] = uint8_t((r.hi >> 8) & 0xff);
        d[o++] = uint8_t((r.hi >> 16) & 0xff);
      }
    }
    pending_acks.clear();
    if (!sealAndSend(d, o, /*data=*/false)) return false;
    if (const char* dbg = getenv("RBX_RAKNET_ACKLOG")) {
      if (*dbg && *dbg != '0') {
        std::string s = "raknet: ACK sent";
        for (const Range& r : ranges) {
          char b[32];
          snprintf(b, sizeof b, " %u-%u", r.lo, r.hi);
          s += b;
        }
        log(s);
      }
    }
    ++acks_sent;
    return true;
  }

  // Turn one decrypted datagram into complete application messages. RakNet
  // framing (reliability headers, split reassembly, ordering) is this
  // transport's whole contribution; the payloads themselves are the shared
  // replication bytes (see rx_stream.h).
  void onReplDatagram(const uint8_t* d, size_t len) {
    rx.onDatagram(d, len, [this](const RaknetAppMessage& m) {
      app_bytes += m.payload.size();
      last_app_id = int(m.id);
      {
        const bool has_sub = (m.id == 0x83 && m.payload.size() > 1);
        const uint8_t sub = has_sub ? m.payload[1] : 0;
        const bool known = raknet::packet_info(m.id) != nullptr;
        if (!known && census.unknown.find(m.id) == census.unknown.end())
          s_log("raknet: UNKNOWN packet id 0x" + hex8(m.id) + " (" +
                std::to_string(m.payload.size()) + "B) — counted, not parsed");
        if (raknet::is_internal_packet(m.id))
          census.note_internal(m.id);
        else
          census.note_app(m.id, has_sub, sub);
        ++census.messages;
      }
      // The RCC pushes its network-settings override list (0x93) as soon as
      // the session is open. The native only sends its join after that push
      // has landed (capture: 0x93 at T, 0x90 at T+180ms), so remember it: the
      // join sequencer waits for it instead of racing the server's setup.
      if (m.id == 0x93) saw_net_settings = true;
      // 0x10 = RakNetPasswordAccepted. Its 104-byte body is:
      //   [id][7B addr][u16][10 x 7B addr][24B extension]
      // and the native 0x13 echoes the extension's second u64 (bytes 88..96
      // of the message) as its own first u64. Parse it before the 0x13 is
      // built; without the echo the RCC rejects the join with 0x95.
      if (m.id == 0x10 && m.payload.size() >= 96) {
        std::memcpy(accept10_q2, m.payload.data() + 88, sizeof accept10_q2);
        have_accept10 = true;
        log("raknet: 0x10 accepted; echo u64=" +
            rbx::hexEncode(accept10_q2, sizeof accept10_q2));
      }
      const char* known = idName(m.id);
      // 0x83 is the ID_DATA container: its sub-id is what the message *means*
      // (0x01 delete, 0x02 new-instance, 0x03 property, 0x04 marker, 0x05 ping,
      // 0x06 ping-back, 0x07 event, 0x0B join data...). Without it the log
      // cannot tell a data ping from a replication chain.
      char sub[8] = "";
      if (m.id == 0x83 && m.payload.size() > 1)
        snprintf(sub, sizeof sub, "/%02x", m.payload[1]);
      char b[160];
      snprintf(b, sizeof b, "raknet: app message 0x%02x%s%s%s (%zuB, rel 0x%02x, ord %u)",
               m.id, sub, known ? " " : "", known ? known : "", m.payload.size(),
               m.reliability, m.ordering);
      std::string line = b;
      // Small messages are the protocol's verdicts (the RCC answers a rejected
      // challenge with a 5-byte 0x15). Without the bytes there is nothing to
      // compare against the native's RX capture.
      if (m.payload.size() <= 16)
        line += " " + rbx::hexEncode(m.payload.data(), m.payload.size());
      log(line);
      // Past the handshake a pad reply is the pad alone (see mirrorKeepalive).
      if (data_ready) app_started = true;
      // Each server clock tick is answered by one of ours (see pumpInput).
      if (m.id == 0x83 && m.payload.size() > 1 && m.payload[1] == 0x23)
        ++clock_ticks_rx;
      // RBX_NO_MIRROR: diagnostics knob. The RCC disconnects us with a
      // receive-error 0x15 once the join lands; silencing each post-join
      // stream in turn is how the offending one gets isolated.
      if (data_ready && getenv("RBX_NO_MIRROR") == nullptr) mirrorKeepalive(m);
      if (message_sink) {
        // Native Client::OnReceive decompresses 0xA9 before dispatching the
        // application payload, including the timestamp envelope form
        // (0x1B + 8 bytes + 0xA9). Feed the inner message through the same
        // sink so join/challenge/replication handling is not duplicated.
        const bool compressed =
            m.id == 0xa9 ||
            (m.id == 0x1b && m.payload.size() >= 10 && m.payload[9] == 0xa9);
        if (compressed) {
          std::vector<uint8_t> decoded;
          std::string derr;
          if (raknet::decompressCompressedApp(m.payload.data(),
                                              m.payload.size(), &decoded,
                                              &derr)) {
            s_log("raknet: 0xA9 decompressed " +
                  std::to_string(m.payload.size()) + "B -> " +
                  std::to_string(decoded.size()) + "B");
            message_sink(decoded.data(), decoded.size());
          } else {
            s_log("raknet: 0xA9 decompression failed: " + derr);
          }
        } else {
          message_sink(m.payload.data(), m.payload.size());
        }
      }
    });
  }

  bool sendWrapped(uint8_t sub, const uint8_t* token, const uint8_t* payload,
                   size_t len) {
    if (fd < 0) return false;
    uint8_t buf[64 + 2048];
    // TLV2 = the join script's server IP and NetStackPort (the native sends the
    // same TLV2 on every datagram, Req1 included). Substituting the UDP
    // endpoint port instead makes the server stop answering Req1 outright, so
    // the join script's own pair is what it validates.
    //
    // RBX_TLV2=peer advertises the datagram's real 5-tuple instead. The native
    // capture always has TLV2 == its peer endpoint (10.190.0.5:50105), while
    // our join script's pair is the RCC's *internal* address and does not
    // describe where the datagram comes from — a candidate for the RCC
    // dropping our subtype-2 traffic.
    uint8_t tlv_ip[4];
    std::memcpy(tlv_ip, cfg.rcc_ip, 4);
    uint16_t tlv_port = cfg.rcc_port;
    if (const char* tm = getenv("RBX_TLV2")) {
      if (!std::strcmp(tm, "peer")) {
        std::memcpy(tlv_ip, &peer.sin_addr, 4);
        tlv_port = cfg.port;
      } else if (!std::strcmp(tm, "rccpeerport")) {
        tlv_port = cfg.port;
      }
    }
    // TLV2's port is subtype-dependent, and this is the difference that made
    // every post-handshake session die after 2-5 datagrams:
    //   subtype 1 (Req1/Req2/0x09): the join script's NetStackPort. The edge
    //     validates it; sending the RCC port instead gets Req1 ignored.
    //   subtype 2 (the established session): the port the session really
    //     sends to — ServerConnections[0].Port. The RCC routes that path by
    //     TLV2, so advertising NetStackPort black-holes every subtype-2
    //     datagram: it ACKed our 0x09 and then nothing at all.
    // Measured on one job, same account, both directions decrypted: with the
    // RCC port the RCC ACKs the 0x13, pushes 0x93, issues the 0x9B and streams
    // 0x97 replication. The native's TLV2 matches its dial port on every
    // datagram because for its join script NetStackPort == ServerConnections
    // port, which is why the byte diff pointed here but looked harmless.
    // RBX_TLV2_SUB2=0 restores the old value for A/B runs.
    if (sub == 0x02) {
      const char* s2 = getenv("RBX_TLV2_SUB2");
      if (!s2 || *s2 != '0')
        tlv_port = cfg.rcc_data_port ? cfg.rcc_data_port : cfg.port;
    }
    const size_t hdr = buildClientHeader(buf, sizeof buf, tlv_ip, tlv_port, sub,
                                        token, true);
    if (!hdr || hdr + len > sizeof buf) return false;
    std::memcpy(buf + hdr, payload, len);
    // RBX_RAKNET_WIRE=<dir>: the exact bytes handed to sendto, so a datagram
    // can be diffed against the native capture byte for byte.
    if (const char* wd = getenv("RBX_RAKNET_WIRE")) {
      static int widx = 0;
      char path[512];
      snprintf(path, sizeof path, "%s/w%06d.bin", wd, widx++);
      if (FILE* f = fopen(path, "wb")) {
        fwrite(buf, 1, hdr + len, f);
        fclose(f);
      }
    }
    const ssize_t n = ::sendto(fd, buf, hdr + len, 0,
                               reinterpret_cast<const sockaddr*>(&peer),
                               sizeof peer);
    return n == ssize_t(hdr + len);
  }

  bool sendReq1() {
    std::vector<uint8_t> p(cfg.probe_size, 0);
    p[0] = kReq1;
    std::memcpy(p.data() + 1, kMagic, sizeof kMagic);
    p[17] = cfg.protocol_version;
    return sendWrapped(0x01, cfg.join_token, p.data(), p.size());
  }
};

RaknetClient::RaknetClient(LogFn log) : impl_(new Impl(std::move(log))) {}

RaknetClient::~RaknetClient() { close(); }

bool RaknetClient::connect(const RaknetConfig& cfg, std::string* err) {
  Impl& s = *impl_;
  s.cfg = cfg;
  s.t0 = std::chrono::steady_clock::now();
  // RBX_SERVER_TOKEN=1 makes subtype-2 datagrams echo the server's own token
  // instead of ours, to test which of the two the RCC actually accepts.
  if (const char* e = getenv("RBX_SERVER_TOKEN")) s.echo_server_token = (e[0] == '1');
  if (const char* e = getenv("RBX_ACK_SUB")) s.ack_sub1 = (e[0] == '1');
  if (const char* e = getenv("RBX_TX_TOKEN"))
    s.tx_token_mode = !std::strcmp(e, "join") ? 0
                       : !std::strcmp(e, "server") ? 1
                       : !std::strcmp(e, "own") ? 2
                                                 : -1;
  if (const char* e = getenv("RBX_TX_SUB")) s.tx_sub_mode = atoi(e);
  if (const char* e = getenv("RBX_ACK_TOKEN"))
    s.ack_token_mode = std::strcmp(e, "join") ? (std::strcmp(e, "server") ? 0 : 2) : 1;
  // Our own RUPP token for subtype-2 datagrams: 16 random bytes, announced in
  // TLV1 of every post-handshake datagram (see sendRaknetDatagram).
  if (RAND_bytes(s.own_token, 16) != 1) {
    if (err) *err = "cannot generate a RUPP session token";
    return false;
  }
  // RBX_SESSION_TOKEN=<32 hex>: pin the 16-byte RUPP token used by subtype-2
  // datagrams instead of taking whatever Reply2's TLV yields. The native uses
  // subtype 2 for the whole post-handshake session; searching which 16 bytes
  // of Reply2 that token is needs an external override.
  if (const char* tv = getenv("RBX_SESSION_TOKEN")) {
    const size_t tl = std::strlen(tv);
    if (tl == 32) {
      bool good = true;
      for (size_t i = 0; i < 16 && good; ++i) {
        auto nib = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          return -1;
        };
        const int hi = nib(tv[i * 2]), lo = nib(tv[i * 2 + 1]);
        if (hi < 0 || lo < 0) { good = false; break; }
        s.session_token[i] = uint8_t((hi << 4) | lo);
      }
      if (good) {
        std::memcpy(s.own_token, s.session_token, 16);
        s.log(std::string("raknet: session token pinned from RBX_SESSION_TOKEN: ") + tv);
      }
    } else {
      s.log("raknet: RBX_SESSION_TOKEN must be 32 hex chars — ignored");
    }
  }
  std::memset(&s.peer, 0, sizeof s.peer);
  s.peer.sin_family = AF_INET;
  s.peer.sin_port = htons(cfg.port);
  if (inet_pton(AF_INET, cfg.host.c_str(), &s.peer.sin_addr) != 1) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(cfg.host.c_str(), nullptr, &hints, &res) != 0 || !res) {
      if (err) *err = "cannot resolve RakNet host " + cfg.host;
      return false;
    }
    s.peer.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    freeaddrinfo(res);
  }
  s.fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (s.fd < 0) {
    if (err) *err = "socket() failed";
    return false;
  }

  timeval tv{};
  tv.tv_sec = 2;
  ::setsockopt(s.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  char abuf[64];
  inet_ntop(AF_INET, &s.peer.sin_addr, abuf, sizeof abuf);
  s.log(std::string("raknet: udp -> ") + abuf + ":" + std::to_string(cfg.port) +
        " (rcc TLV " + std::to_string(cfg.rcc_port) + ")");

  for (int attempt = 0; attempt < 3 && s.stage == 0; ++attempt) {
    if (!s.sendReq1()) {
      if (err) *err = "Req1 send failed";
      return false;
    }
    s.log("raknet: Req1 sent (" + std::to_string(cfg.probe_size) + "B probe)");
    const int got = pump(2000);
    if (s.stage >= 1) break;
    (void)got;
    s.log("raknet: no Reply1 yet, retrying Req1");
  }
  if (s.stage < 1) {
    if (err) *err = "no OpenConnectionReply1 from the RakNet endpoint";
    return false;
  }
  s.log("raknet: Reply1 guid=" + std::to_string(s.guid) + " mtu=" +
        std::to_string(s.mtu) + " (offline handshake accepted)");
  if (cfg.have_crypto) {
    if (!s.keys.generate()) {
      if (err) *err = "cannot generate the session X25519 keypair";
      return false;
    }
    if (!s.keys.install(cfg.peer_pub)) {
      if (err) *err = "X25519 with the join's EphemeralEarlyPubKey failed";
      return false;
    }
    s.crypto_ready = true;
    for (int attempt = 0; attempt < 3 && !s.reply2_ok_flag; ++attempt) {
      if (!s.sendReq2()) {
        if (err) *err = "Req2 build/send failed (ticket blobs too large?)";
        return false;
      }
      pump(2000);
      if (!s.reply2_ok_flag && attempt < 2)
        s.log("raknet: no verified Reply2 yet, retrying Req2");
    }
    if (!s.reply2_ok_flag) {
      if (err)
        *err = s.stage >= 2 ? "Reply2 tag did not verify (rx key mismatch)"
                            : "no OpenConnectionReply2 from the RakNet endpoint";
      return false;
    }
    s.log("raknet: encrypted handshake complete (Reply2 verified)");
    return true;
  }
  if (!cfg.send_req2) {
    s.log("raknet: Req2 held back: tail is ticket-encrypted (pass the join "
          "script's EphemeralEarlyPubKey + ClientTicket to enable it)");
    return true;
  }
  std::vector<uint8_t> p(cfg.probe_size, 0);
  p[0] = kReq2;
  std::memcpy(p.data() + 1, kMagic, sizeof kMagic);
  const uint64_t cguid = s.rng();
  std::memcpy(p.data() + 17, &cguid, 8);
  uint16_t mtu_be = htons(s.mtu);
  std::memcpy(p.data() + 25, &mtu_be, 2);
  if (!s.sendWrapped(0x01, cfg.join_token, p.data(), p.size())) {
    if (err) *err = "Req2 send failed";
    return false;
  }
  s.log("raknet: Req2 sent (experimental, unencrypted tail)");
  pump(2000);
  return true;
}

bool RaknetClient::data_ready() const { return impl_->data_ready; }

bool RaknetClient::sendReliable(const uint8_t* data, size_t len) {
  return impl_->sendRaknetDatagram(data, len);
}

bool RaknetClient::sendUnreliable(const uint8_t* data, size_t len) {
  return impl_->sendUnreliable(data, len);
}

// RakNet ID_CONNECTION_REQUEST: 09 <guid8> 000000000000 0dac00 374f5e116c45.
// The 15 bytes after the GUID are identical in every native session captured
// (2026-10-27), so they are a build constant.
bool RaknetClient::sendConnectionRequest() {
  Impl& s = *impl_;
  if (!s.data_ready) return false;
  static const uint8_t kTail[15] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x0d, 0xac, 0x00, 0x37, 0x4f, 0x5e,
                                    0x11, 0x6c, 0x45};
  uint8_t d[24];
  d[0] = 0x09;
  std::memcpy(d + 1, s.session_guid, 8);
  std::memcpy(d + 9, kTail, sizeof kTail);
  const bool ok = s.sendRaknetDatagram(d, sizeof d);
  // The native's first three datagrams (Req1, Req2, CONNECTION_REQUEST) all
  // ride the join token under subtype 1, and only the datagram *after* 0x09
  // switches to subtype 2 with the client's own token (capture: wire_tx_2 is
  // sub=1, wire_tx_3 - the 0x13 - is sub=2). Switching earlier makes the
  // server drop 0x09, and then nothing after it is read at all.
  //
  // The 0x09 stays on subtype 1 (native order). The session died for two
  // months because subtype-2 datagrams carried NetStackPort in TLV2, not the
  // RCC port the session sends to — see sendWrapped(). That is fixed, so
  // sendNewIncoming() flips to subtype 2 unless --token-sub 1 pins it.
  s.own_token_live = ok && s.cfg.force_token_sub == 2 && s.have_session;
  // Say which RUPP subtype the session actually rides. The old text claimed the
  // subtype-2 switch unconditionally, which made every log look like the
  // dropped-session variant even when the client stayed on subtype 1.
  s.log(std::string("raknet: ID_CONNECTION_REQUEST sent ") + (ok ? "ok" : "failed") +
        (ok ? (s.own_token_live ? " (RUPP subtype 2, own token)"
                               : " (RUPP subtype 1, join token)")
            : ""));
  return ok;
}

// ID_NEW_INCOMING_CONNECTION: 13 <our address> <peer address> <12 dummy
// 04 ff ff ff ff 00 00 entries>. RakNet addresses are [ver][ip4][port BE].
bool RaknetClient::sendNewIncoming() {
  Impl& s = *impl_;
  if (!s.data_ready) return false;
  // The native's 0x13 is the first datagram of the established session and the
  // first one under subtype 2 with the token the server issued in Reply2
  // (wire_tx_3, sub=2, token == Reply2's TLV1 token). The 102-byte body below
  // now matches the native's shape byte for byte apart from the per-session
  // random fields (same-job capture, /tmp/nat_same); the earlier drops were
  // the subtype-2 TLV2 port, not this payload.
  // The native's 0x13 is the first datagram under subtype 2, so the switch is
  // the default once Reply2 has handed over the session token. --token-sub 1
  // pins the session to subtype 1 for A/B runs.
  if (s.have_session && s.cfg.force_token_sub != 1) s.own_token_live = true;
  uint8_t d[160];
  size_t o = 0;
  // Oracle knob: replay a captured native payload byte for byte (RBX_RAKNET_13).
  // The native form is external + 10 internal addresses + a 24-byte extension;
  // the builder below is still the older 14-entry guess.
  if (const char* path = getenv("RBX_RAKNET_13")) {
    if (FILE* f = fopen(path, "rb")) {
      o = fread(d, 1, sizeof d, f);
      fclose(f);
      if (o >= 8 && d[0] == 0x13) {
        const bool ok = s.sendRaknetDatagram(d, o, /*ordered=*/true);
        s.log(std::string("raknet: ID_NEW_INCOMING_CONNECTION replayed ") +
              std::to_string(o) + "B " + (ok ? "ok" : "failed"));
        return ok;
      }
      o = 0;
    }
  }
  d[o++] = 0x13;
  auto addr = [&](const uint8_t ip[4], uint16_t port) {
    d[o++] = 0x04;
    std::memcpy(d + o, ip, 4);
    o += 4;
    d[o++] = uint8_t(port >> 8);
    d[o++] = uint8_t(port & 0xff);
  };
  // Native shape (two live 1818 captures, /tmp/nat_job and /tmp/nat_diff2):
  // the first address is 127.139.202.222 with a *random* port, not the socket's
  // own address — both captures agree on the 127.139.202 tripet and differ on
  // the port, so the native advertises a synthetic loopback address. Ours used
  // to send the real 0.0.0.0:<port>, which is the one byte range that differs
  // from native in the whole 0x13.
  // Layout: our address, 128.255.254.254:0, NINE 255.255.255.255:0 entries,
  // then a 24-byte extension. The older 14-address form (99B) was dropped by
  // the RCC: it ACKed our 0x09 and then stopped ACKing the very next datagram,
  // while the native's 102B form keeps the ack stream alive (2026-10-30).
  {
    // RBX_13_OLD=1: the pre-diff shape (real socket address, old tail) so the
    // 0x13 change can be A/B'd against a working configuration.
    const char* old13 = getenv("RBX_13_OLD");
    if (old13 && *old13 && *old13 != '0') {
      uint8_t ip[4] = {0, 0, 0, 0};
      uint16_t port = 0;
      sockaddr_in self{};
      socklen_t sl = sizeof self;
      if (s.fd >= 0 &&
          getsockname(s.fd, reinterpret_cast<sockaddr*>(&self), &sl) == 0) {
        std::memcpy(ip, &self.sin_addr.s_addr, 4);
        port = ntohs(self.sin_port);
      }
      addr(ip, port);
    } else {
      uint16_t adv_port = 0;
      std::random_device rd;
      std::mt19937 rng(rd());
      do {
        adv_port = uint16_t(rng());
      } while (adv_port < 1024);
      const uint8_t adv_ip[4] = {127, 139, 202, 222};
      addr(adv_ip, adv_port);
    }
  }
  {
    const uint8_t any_peer[4] = {128, 255, 254, 254};
    addr(any_peer, 0);
  }
  for (int i = 0; i < 9; ++i) {
    const uint8_t dummy[4] = {0xff, 0xff, 0xff, 0xff};
    addr(dummy, 0);
  }
  // 24-byte extension the native appends, measured on the same job as ours
  // (2026-10-30, /tmp/nat_same: 102-byte body, 9 dummy addresses + this tail):
  //   00*5 | A(3B) | 00*6 | B(2B) | 00 06 5f | C(3B) | D(2B)
  // A/B/C/D are rolled per session, so they are random here. The 36-byte form
  // this builder used to send (… | C(5B) | 00 00 48 00 | 00*6 | D(2B)) came
  // from an older capture and made the body 114 bytes where the native's is
  // 102; RBX_13_EXT=1 restores it for A/B runs.
  {
    const char* old13 = getenv("RBX_13_OLD");
    const char* ext13 = getenv("RBX_13_EXT");
    if (old13 && *old13 && *old13 != '0') {
      static const uint8_t kOldTail[24] = {
          0x00, 0x00, 0x00, 0x00, 0x00, 0x43, 0xac, 0xa7, 0x00, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x07, 0xbf, 0x00, 0x06, 0x5f, 0x10, 0xd9, 0x82, 0x5c, 0x36};
      std::memcpy(d + o, kOldTail, sizeof kOldTail);
      o += sizeof kOldTail;
    } else if (ext13 && *ext13 && *ext13 != '0') {
      uint8_t tail[36] = {0};
      RAND_bytes(tail + 5, 3);
      RAND_bytes(tail + 14, 2);
      tail[16] = 0x00;
      tail[17] = 0x06;
      tail[18] = 0x5f;
      RAND_bytes(tail + 19, 5);
      tail[25] = 0x48;  // fixed marker after A/B/C: 00 00 48 00
      RAND_bytes(tail + 34, 2);
      std::memcpy(d + o, tail, sizeof tail);
      o += sizeof tail;
    } else {
      // Native extension (IDA sub_447C0BC, same-job /tmp/our4 0x10 body):
      //   u64 #1 = the second u64 of the server's 0x10 extension (echoed)
      //   u64 #2 = client uptime in ms
      //   u64 #3 = CLOCK_REALTIME in microseconds
      // All three are big-endian on the wire, like the native's bitstream.
      uint8_t tail[24] = {0};
      const char* echo = getenv("RBX_13_ECHO");
      const bool use_echo = !(echo && *echo == '0');
      if (use_echo && s.have_accept10) {
        std::memcpy(tail, s.accept10_q2, 8);
      } else {
        // Legacy random shape (00*5 | 3 random bytes).
        RAND_bytes(tail + 5, 3);
      }
      const uint64_t up_ms = uint64_t(std::chrono::duration_cast<
          std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count()) &
          0xFFFFFFFFull;
      const uint64_t rt_us = uint64_t(std::chrono::duration_cast<
          std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
      for (int i = 0; i < 8; ++i) {
        tail[8 + i] = uint8_t(up_ms >> (56 - 8 * i));
        tail[16 + i] = uint8_t(rt_us >> (56 - 8 * i));
      }
      std::memcpy(d + o, tail, sizeof tail);
      o += sizeof tail;
    }
  }
  const bool ok = s.sendRaknetDatagram(d, o, /*ordered=*/true);
  s.log(std::string("raknet: ID_NEW_INCOMING_CONNECTION sent ") + std::to_string(o) +
        "B " + (ok ? "ok" : "failed"));
  return ok;
}

// ID_SUBMIT_TICKET: [0x8a][ticket bytes], sent as one reliable ordered message.
// The native sends the 5-part ClientTicket exactly once, right after the
// connection is accepted (IDA sub_63DE3A4 splits it on ';').
int RaknetClient::sendJoinMessage(const uint8_t* msg, size_t len, bool ordered) {
  return impl_->sendJoinMessage(msg, len, ordered);
}
void RaknetClient::reserveOrdering() { ++impl_->ord_idx; }
uint64_t RaknetClient::datagrams_tx() const { return impl_->dgram_tx; }
uint64_t RaknetClient::split_messages_tx() const { return impl_->split_tx; }
uint64_t RaknetClient::retransmits_tx() const { return impl_->rtx_tx; }

bool RaknetClient::sendTicket(const std::string& ticket) {
  Impl& s = *impl_;
  if (!s.data_ready || ticket.empty() || ticket.size() > 1100) return false;
  std::vector<uint8_t> d;
  d.reserve(ticket.size() + 1);
  d.push_back(0x8a);
  d.insert(d.end(), ticket.begin(), ticket.end());
  const bool ok = s.sendRaknetDatagram(d.data(), d.size(), /*ordered=*/true);
  s.log(std::string("raknet: ID_SUBMIT_TICKET sent ") + std::to_string(ticket.size()) +
        "B ticket " + (ok ? "ok" : "failed"));
  return ok;
}

bool RaknetClient::sendAcks() { return impl_->flushAcks(); }
uint64_t RaknetClient::acks_sent() const { return impl_->acks_sent; }
uint64_t RaknetClient::server_acks() const { return impl_->server_acks; }
uint64_t RaknetClient::naks_sent() const { return impl_->naks_sent; }
size_t RaknetClient::rx_missing() const { return impl_->rx_missing.size(); }
bool RaknetClient::netSettingsSeen() const { return impl_->saw_net_settings; }
uint64_t RaknetClient::dups_dropped() const { return impl_->dups_dropped; }

const uint8_t* RaknetClient::last_datagram() const {
  return impl_->last_dgram.empty() ? nullptr : impl_->last_dgram.data();
}
size_t RaknetClient::last_datagram_len() const { return impl_->last_dgram.size(); }

long long RaknetClient::ms_since_rx() const {
  const auto& t = impl_->last_rx_time;
  if (t.time_since_epoch().count() == 0) return -1; // nothing received yet
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t)
      .count();
}
uint64_t RaknetClient::data_datagrams_rx() const { return impl_->data_rx_ok; }
uint64_t RaknetClient::data_frames_rx() const { return impl_->data_rx_frames; }

void RaknetClient::setMessageSink(MessageFn fn) {
  impl_->message_sink = std::move(fn);
}
uint64_t RaknetClient::app_messages() const { return impl_->rx.messages(); }
uint64_t RaknetClient::app_split_messages() const {
  return impl_->rx.split_messages();
}
uint64_t RaknetClient::app_bytes() const { return impl_->app_bytes; }
uint64_t RaknetClient::rx_malformed() const { return impl_->rx.malformed(); }

std::string RaknetClient::packet_census() const {
  return impl_->census.summary();
}

bool RaknetClient::send(const uint8_t* payload, size_t len) {
  Impl& s = *impl_;
  if (s.stage < 1) return false;
  const uint8_t* tok = s.own_token_live ? s.own_token : s.cfg.join_token;
  const uint8_t sub = s.own_token_live ? 0x02 : 0x01;
  return s.sendWrapped(sub, tok, payload, len);
}

uint64_t RaknetClient::clock_ticks_rx() const { return impl_->clock_ticks_rx; }

int RaknetClient::pump(int timeout_ms) {
  Impl& s = *impl_;
  if (s.fd < 0) return 0;
  int got = 0;
  // timeout_ms <= 0 is a true non-blocking drain: recv until EAGAIN. The
  // frame-driven viewer needs this — a 1ms timeout truncates to zero in the
  // deadline math below and would never call recvfrom at all, so a render
  // loop could not keep up with a 1 MB snapshot.
  const bool nonblocking = timeout_ms <= 0;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(nonblocking ? 0 : timeout_ms);
  for (;;) {
    if (!nonblocking) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now())
                            .count();
      if (left <= 0) break;
      // Wake at least every 50 ms while retransmits are pending, so a lost
      // join fragment is re-sent on time instead of after a long recv block.
      long long slice = left;
      if (!s.rtx.empty() && slice > 50) slice = 50;
      timeval tv{};
      tv.tv_sec = int(slice / 1000);
      tv.tv_usec = int((slice % 1000) * 1000);
      ::setsockopt(s.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    uint8_t buf[2048];
    sockaddr_in from{};
    socklen_t flen = sizeof from;
    const ssize_t n = ::recvfrom(s.fd, buf, sizeof buf,
                                 nonblocking ? MSG_DONTWAIT : 0,
                                 reinterpret_cast<sockaddr*>(&from), &flen);
    if (n <= 0) break;
    ++got;
    ++s.pkts_rx;
    s.bytes_rx += size_t(n);
    // RBX_RAKNET_DUMP also keeps the raw (still RUPP-wrapped, still sealed)
    // datagrams as rNNNNNN.bin next to the authenticated dNNNNNN.bin ones —
    // that is how a datagram we never managed to open is still diagnosable.
    if (const char* dump = getenv("RBX_RAKNET_DUMP")) {
      char path[512];
      snprintf(path, sizeof path, "%s/r%06llu.bin", dump,
               (unsigned long long)(s.pkts_rx - 1));
      if (FILE* f = fopen(path, "wb")) {
        fwrite(buf, 1, size_t(n), f);
        fclose(f);
      }
    }
    RuppView v = ruppView(buf, size_t(n));
    if (!v.ok) {
      // Offline handshake replies can arrive bare (no RUPP header): the
      // server's Reply1 is 28B = id + magic + guid + sec + mtu on its own.
      const bool bare = n >= 17 && idName(buf[0]) != nullptr &&
                        std::memcmp(buf + 1, kMagic, sizeof kMagic) == 0;
      if (!bare) {
        ++s.non_rupp;
        s.log("raknet: non-RUPP datagram " + std::to_string(n) + "B dropped");
        continue;
      }
      v.ok = true;
      v.payload = buf;
      v.len = size_t(n);
    }
    if (v.has_token && v.token_sub == 2) {
      // The 16-byte RUPP token is a shared session security key, and the
      // server rotates it: on a live native capture the server's next frame
      // announces a new token and the native's very next datagram carries it
      // (RX#483 -> SEND#146, 9 rotations in 34 s). Our client used to pin the
      // first value from Reply2 and never update it; when the RCC rotated
      // earlier than that (it announced a new key in its first ACK after the
      // 0x09), every datagram we sent still carried the stale key and the RCC
      // answered 0x95 ID_SECURITY_KEY_REJECTED followed by
      // 0x15 00000102 DisconnectSecurityKeyMismatch.
      const bool first = !s.have_session;
      const bool changed =
          !first && std::memcmp(s.session_token, v.token, 16) != 0;
      const char* adopt_env = getenv("RBX_TOKEN_ADOPT");
      const bool adopt = !(adopt_env && *adopt_env == '0');
      if (first || (changed && adopt)) {
        std::memcpy(s.session_token, v.token, 16);
        s.have_session = true;
        char hex[33];
        for (int i = 0; i < 16; ++i) snprintf(hex + i * 2, 3, "%02x", v.token[i]);
        s.log(std::string(first ? "raknet: session token issued: "
                                : "raknet: session token rotated: ") + hex);
        // The token's only source is this header, so log the header verbatim:
        // a shifted parse would silently produce a token the server rejects.
        if (first) {
          std::string hdr;
          for (size_t i = 0; i < size_t(n) && i < 40; ++i) {
            char b[4];
            snprintf(b, sizeof b, "%02x", buf[i]);
            hdr += b;
          }
          s.log("raknet: token header (total " + std::to_string(v.total) + "): " + hdr);
        }
      }
    }
    ++s.payloads_rx;
    if (v.len == 0) continue;
    // Anything after the handshake that is not a data frame under the
    // session token is unusual: say so instead of dropping it silently, so a
    // server message sent on a different token/stream cannot go unnoticed.
    if (s.data_ready && getenv("RBX_RAKNET_TRACE") &&
        !(v.has_token && v.token_sub == 2)) {
      char b[160];
      snprintf(b, sizeof b,
               "raknet: unopened RUPP total=%u tok=%d sub=%u len=%zu first=%02x %02x",
               unsigned(v.total), v.has_token ? 1 : 0, unsigned(v.token_sub),
               v.len, v.payload[0], v.len > 1 ? v.payload[1] : 0);
      s.log(b);
    }
    // Post-handshake frames are one AEAD datagram: [ct][counter16][tag16].
    if (s.data_ready && v.has_token && v.token_sub == 2 && v.len >= 18) {
      uint8_t out[2048];
      size_t olen = 0;
      uint64_t used = 0;
      ++s.data_rx_frames;
      const bool opened = datagramOpen(s.keys.data_rx, v.payload, v.len, s.rx_ctr,
                                       out, sizeof out, &olen, &used);
      if (!opened) {
        char b[128];
        snprintf(b, sizeof b,
                 "raknet: data frame %zuB FAILED to open (rx_ctr %llu, first %02x)",
                 v.len, (unsigned long long)s.rx_ctr, v.payload[0]);
        s.log(b);
      }
      if (opened) {
        s.rx_ctr = used + 1;
        s.last_rx_time = std::chrono::steady_clock::now();
        s.last_dgram.assign(out, out + olen);
        ++s.data_rx_ok;
        {
          raknet::DatagramHeader dh;
          if (raknet::parse_datagram_header(out, olen, &dh))
            s.census.note_datagram(dh);
          else
            s.census.note_malformed();
        }
        // RBX_RAKNET_DUMP=<dir>: write every authenticated datagram (raw
        // RakNet internal packets, post-decrypt) for offline analysis.
        if (const char* dump = getenv("RBX_RAKNET_DUMP")) {
          char path[512];
          snprintf(path, sizeof path, "%s/d%06llu.bin", dump,
                   (unsigned long long)s.data_rx_ok);
          if (FILE* f = fopen(path, "wb")) {
            fwrite(out, 1, olen, f);
            fclose(f);
          }
        }
        char b[160];
        snprintf(b, sizeof b,
                 "raknet: data datagram %zuB (flags=0x%02x dgram=%u first=0x%02x)",
                 olen, olen > 0 ? out[0] : 0,
                 olen > 3 ? (unsigned(out[1]) | (unsigned(out[2]) << 8) |
                             (unsigned(out[3]) << 16))
                          : 0u,
                 olen > 4 ? out[4] : 0);
        s.log(b);
        if (olen >= 4 && (out[0] & 0xc0) == 0xc0) {
          // ACK from the server: transport only, nothing to reply to. It is
          // the only receipt we get for 0x09/0x13/0x90/0x8a/0x8f, so log it.
          //
          // The RCC's writer is NOT the client's compact form: a native ACK
          // reads `d0 0001 01 <u24 dgram>` plus trailing u24s (e.g.
          // d0 0001 01 421600 682700 682700), which the stock
          // `[count u16][single u8][min u24][max u24]` layout mis-reads. Log
          // the acknowledged id (the u24 right after the leading `d0 00 01 X`)
          // and the raw bytes, so the receipt is never invented.
          const uint32_t acked = olen >= 7
                                     ? uint32_t(out[4]) | (uint32_t(out[5]) << 8) |
                                           (uint32_t(out[6]) << 16)
                                     : 0u;
          std::string hex;
          for (size_t i = 0; i < olen && i < 24; ++i) {
            char h[4];
            snprintf(h, sizeof h, "%02x", out[i]);
            hex += h;
          }
          char b[256];
          snprintf(b, sizeof b, "raknet: server ACK dgram=%u (%zuB) %s", acked,
                   olen, hex.c_str());
          s.log(b);
          ++s.server_acks;
          s.census.note_internal(out[0]);
          // Cancel the matching retransmits. parse_ack_datagram understands
          // both the compact record form and the RCC's `d0 0001 01 <u24>
          // <u24>...` form (one single record followed by bare ids), so a
          // misread trailing field can no longer drop a live fragment.
          std::vector<raknet::AckRecord> recs;
          std::vector<uint32_t> acked_ids;
          if (raknet::parse_ack_datagram(out, olen, &recs)) {
            raknet::expand_acks(recs, &acked_ids);
          } else {
            for (size_t off = 4; off + 3 <= olen; off += 3)
              acked_ids.push_back(uint32_t(out[off]) |
                                  (uint32_t(out[off + 1]) << 8) |
                                  (uint32_t(out[off + 2]) << 16));
          }
          for (uint32_t id : acked_ids) s.ackRtx(id);
          continue;
        }
        if (olen >= 4 && (out[0] & 0x80)) {
          const uint32_t num = uint32_t(out[1]) | (uint32_t(out[2]) << 8) |
                               (uint32_t(out[3]) << 16);
          s.noteRxDatagram(num);
          // ACK the highest reliable *message number* in the datagram, not its
          // datagram id (default; RBX_ACK_MSGNO=0 reverts). The RCC re-sends the
          // same pad message under a fresh datagram id every time, so acking ids
          // leaves its message unacknowledged and it retransmits forever
          // (measured: 1224 identical 1160-byte pads in 8 s; 196 datagrams once
          // the message number is acked instead).
          // ACK the datagram id, which is what RakNet's ACK datagram carries
          // (the native's own ACK writer, captured, packs the datagram id).
          // Acking a *message number* instead (an earlier workaround for the
          // RCC's pad retransmits) leaves the server's reliable window jammed:
          // it resends the same 0x97/replication fragments forever, and the
          // session dies right after the join snapshot (measured 2026-10-30).
          const uint32_t ack_num = num;
          // A retransmission must be ACKed again: our previous ACK may have
          // been the packet that got lost. Skip delivery, never the receipt.
          if (!s.seen_dgrams.insert(num).second) {
            ++s.dups_dropped;
            s.pending_acks.push_back(ack_num);
            s.flushAcks();
            continue;
          }
          s.pending_acks.push_back(ack_num);
          // ACK immediately, from inside the receive path. The app's outer loop
          // only flushes once per second, which is slower than the RCC's
          // retransmit timer (~250 ms): it resent its 0x93 and both pad
          // fragments over and over and the reliable window never advanced.
          s.flushAcks();
          s.onReplDatagram(out, olen);
        }
        continue;
      }
    }
    const uint8_t id = v.payload[0];
    ++s.id_hist[int(id)];
    if (id == kReply1 && v.len >= 28) {
      std::memcpy(&s.guid, v.payload + 17, 8);
      s.mtu = uint16_t((v.payload[26] << 8) | v.payload[27]);
      s.stage = 1;
    } else if (id == kReply2) {
      s.handleReply2(v.payload, v.len);
    }
    s.census.note_internal(id);
    if (idName(id)) {
      s.log(std::string("raknet: ") + idName(id) + " " +
            std::to_string(v.len) + "B tlv=" + std::to_string(v.total));
    }
  }
  s.flushNaks();
  s.pumpRtx();
  return got;
}

void RaknetClient::close() {
  Impl& s = *impl_;
  if (s.fd >= 0) {
    ::close(s.fd);
    s.fd = -1;
  }
}

void RaknetClient::resetSession() {
  Impl& s = *impl_;
  close();
  s.stage = 0;
  s.mtu = 0;
  s.guid = 0;
  std::memset(s.session_token, 0, sizeof s.session_token);
  std::memset(s.own_token, 0, sizeof s.own_token);
  std::memset(s.session_guid, 0, sizeof s.session_guid);
  s.own_token_live = false;
  s.have_session = false;
  s.crypto_ready = false;
  s.reply2_ok_flag = false;
  s.reply2_plain.clear();
  s.data_ready = false;
  s.tx_ctr = s.rx_ctr = 0;
  s.dgram_no = 0;
  s.rel_msg = 0;
  s.ord_idx = 0;
  s.split_seq = 0;
  s.seen_dgrams.clear();
  s.pending_acks.clear();
  s.rx_seq_started = false;
  s.rx_highest = 0;
  s.rx_missing.clear();
  s.missing_since.clear();
  s.nak_rounds = 0;
  s.next_nak = {};
  s.last_rx_time = {};
  s.saw_net_settings = false;
  s.have_accept10 = false;
  s.rx = RaknetRxStream();
  s.id_hist.clear();
  s.last_app_id = -1;
  s.last_dgram.clear();
  s.last_kal.clear();
  s.kal_per_id.clear();
  std::memset(s.pad_replies, 0, sizeof s.pad_replies);
  s.kal_replies = 0;
}

bool RaknetClient::connected() const { return impl_->stage >= 1; }
int RaknetClient::stage() const { return impl_->stage; }
bool RaknetClient::reply2_ok() const { return impl_->reply2_ok_flag; }
const uint8_t* RaknetClient::reply2_plain() const {
  return impl_->reply2_plain.empty() ? nullptr : impl_->reply2_plain.data();
}
size_t RaknetClient::reply2_plain_len() const { return impl_->reply2_plain.size(); }
const uint8_t* RaknetClient::client_pub() const { return impl_->keys.pub; }
uint16_t RaknetClient::mtu() const { return impl_->mtu; }
uint64_t RaknetClient::server_guid() const { return impl_->guid; }
bool RaknetClient::has_session_token() const { return impl_->have_session; }
const uint8_t* RaknetClient::session_token() const {
  return impl_->session_token;
}
uint64_t RaknetClient::pkts_rx() const { return impl_->pkts_rx; }
uint64_t RaknetClient::payloads_rx() const { return impl_->payloads_rx; }
uint64_t RaknetClient::bytes_rx() const { return impl_->bytes_rx; }

} // namespace rbxclient
