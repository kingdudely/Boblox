// raknet.cpp — Roblox RakNet handshake + RUPP session framing.
#include "raknet.h"

#include "rupp.h"
#include "session_crypto.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstring>
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

// Client header: TLV1 (type 1, len 17, subtype) + optional TLV2 (type 2,
// len 6, rcc ip:port). 31 bytes with TLV2 (native game path), 23 without.
size_t buildClientHeader(uint8_t* out, size_t cap, const RaknetConfig& cfg,
                         uint8_t sub, const uint8_t token[16],
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
    std::memcpy(out + 25, cfg.rcc_ip, 4);
    out[29] = uint8_t(cfg.rcc_port >> 8);
    out[30] = uint8_t(cfg.rcc_port & 0xff);
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
  uint8_t session_token[16]{};
  bool have_session = false;
  uint64_t pkts_rx = 0, payloads_rx = 0, bytes_rx = 0, non_rupp = 0;
  uint32_t split_seq = 0;   // split ids, counted like the native (0, 1, 2, ...)
  uint64_t split_tx = 0;    // fragments emitted
  uint64_t dgram_tx = 0;    // datagrams emitted (diagnostics only)
  std::mt19937_64 rng;
  std::map<int, int> id_hist;
  SessionKeys keys;
  bool crypto_ready = false;
  bool reply2_ok_flag = false;
  std::vector<uint8_t> reply2_plain;

  // Post-handshake datagram layer (see raknet.h).
  bool data_ready = false;
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
    uint8_t frame[1600];
    const size_t flen = datagramSeal(keys.data_tx, tx_ctr, d, size_t(p - d),
                                     frame, sizeof frame);
    if (!flen) return false;
    // Every datagram rides the join script's RUPP token (TLV1 subtype 1).
    //
    // The Reply2 header's `01 11 02 <16B>` TLV is NOT a token the server
    // accepts from us: echoing it (the obvious reading) gets every
    // post-handshake datagram silently dropped — the connection stalls on the
    // server's retransmits and no ACK ever comes back. Sending the join token
    // instead makes the server accept the whole session (ACKs, then the
    // 0x93/0x65 replication stream), which is the live-verified 2026-10-27
    // behaviour. --token-sub 2 keeps the old path for experiments.
    uint8_t sub = 0x01;
    const uint8_t* tok = cfg.join_token;
    if (cfg.force_token_sub) {
      sub = cfg.force_token_sub;
      tok = (sub == 1) ? cfg.join_token : session_token;
    }
    ++tx_ctr;
    ++dgram_no;
    ++dgram_tx;
    ++rel_msg;
    return sendWrapped(sub, tok, frame, flen);
  }

  // ACK datagram (native 0xc0): flag, count u16 BE, then [minEqualsMax u8,
  // min u24 LE, max u24 LE] per range. Payload is transport-only, but it is
  // sealed and counter-tracked exactly like any other datagram.
  bool sealAndSend(const uint8_t* plain, size_t len) {
    uint8_t frame[1600];
    const size_t flen = datagramSeal(keys.data_tx, tx_ctr, plain, len, frame,
                                     sizeof frame);
    if (!flen) return false;
    uint8_t sub = 0x01; // join token; see sendRaknetDatagram for why
    const uint8_t* tok = cfg.join_token;
    if (cfg.force_token_sub) {
      sub = cfg.force_token_sub;
      tok = (sub == 1) ? cfg.join_token : session_token;
    }
    ++tx_ctr;
    ++dgram_no;
    ++dgram_tx;
    return sendWrapped(sub, tok, frame, flen);
  }

  // One fragment of a split message. Native layout (verified against the live
  // 0x90/0x8a captures): 0x83 <u24 dgram> 0x70 <u16 BE bits> <u24 msg>
  // <u24 ordering index> <u08 channel> <u32 BE count> <u16 BE split id>
  // <u32 BE index> <data>.
  bool sendSplitFragment(const uint8_t* data, size_t len, uint32_t count,
                         uint16_t split_id, uint32_t index, uint32_t ord,
                         bool ordered) {
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
    *p++ = uint8_t(rel_msg & 0xff);
    *p++ = uint8_t((rel_msg >> 8) & 0xff);
    *p++ = uint8_t((rel_msg >> 16) & 0xff);
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
    return sealAndSend(d, size_t(p - d));
  }

  // Reliable-ordered message, fragmented when it does not fit one datagram.
  int sendJoinMessage(const uint8_t* msg, size_t len, bool ordered) {
    static constexpr size_t kFrag = 1061; // native split size (two sessions)
    if (!data_ready || len == 0) return 0;
    if (len <= kFrag) return sendRaknetDatagram(msg, len, ordered) ? 1 : 0;
    const uint32_t count = uint32_t((len + kFrag - 1) / kFrag);
    const uint16_t split_id = split_seq++;
    const uint32_t ord = ordered ? ord_idx++ : 0;
    for (uint32_t i = 0; i < count; ++i) {
      const size_t off = size_t(i) * kFrag;
      const size_t n = std::min(kFrag, len - off);
      if (!sendSplitFragment(msg + off, n, count, split_id, i, ord, ordered))
        return 0;
      ++rel_msg;
      ++split_tx;
    }
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
    d[o++] = 0xc0;
    d[o++] = uint8_t((ranges.size() >> 8) & 0xff);
    d[o++] = uint8_t(ranges.size() & 0xff);
    for (const Range& r : ranges) {
      const bool same = r.lo == r.hi;
      d[o++] = same ? 1 : 0;
      d[o++] = uint8_t(r.lo & 0xff);
      d[o++] = uint8_t((r.lo >> 8) & 0xff);
      d[o++] = uint8_t((r.lo >> 16) & 0xff);
      if (!same) {
        d[o++] = uint8_t(r.hi & 0xff);
        d[o++] = uint8_t((r.hi >> 8) & 0xff);
        d[o++] = uint8_t((r.hi >> 16) & 0xff);
      }
    }
    pending_acks.clear();
    if (!sealAndSend(d, o)) return false;
    ++acks_sent;
    return true;
  }

  // Walk a decrypted RakNet datagram and log the message ids inside it.
  // Header per message: reliability byte, u16 BE bit length, then the
  // reliability/ordering fields, then the data whose first byte is the id.
  void parseMessages(const uint8_t* d, size_t len) {
    if (len < 4) return;
    size_t off = 4;
    while (off + 3 <= len) {
      const uint8_t rel = d[off];
      const uint16_t bits = uint16_t((d[off + 1] << 8) | d[off + 2]);
      const uint8_t kind = uint8_t(rel >> 5);
      size_t dlen = (size_t(bits) + 7) / 8;
      off += 3;
      unsigned ord = 0, msgnum = 0;
      if (kind >= 2) {
        msgnum = unsigned(d[off]) | (unsigned(d[off + 1]) << 8) |
                 (unsigned(d[off + 2]) << 16);
        off += 3;                              // reliableMessageNumber
      }
      if (kind == 1 || kind == 3 || kind == 4) {
        ord = unsigned(d[off]) | (unsigned(d[off + 1]) << 8) |
              (unsigned(d[off + 2]) << 16);
        off += 4;                              // ordering + channel
      }
      if (rel & 0x10) off += 10;               // split header
      if (off + dlen > len) break;
      if (dlen) {
        char b[160];
        snprintf(b, sizeof b,
                 "raknet: server message id 0x%02x (%zuB, rel 0x%02x, msg %u, ord %u, first %02x%02x%02x%02x)",
                 d[off], dlen, rel, unsigned(msgnum), unsigned(ord),
                 dlen > 1 ? d[off + 1] : 0, dlen > 2 ? d[off + 2] : 0,
                 dlen > 3 ? d[off + 3] : 0, dlen > 4 ? d[off + 4] : 0);
        log(b);
        last_app_id = int(d[off]);
      }
      off += dlen;
    }
  }

  bool sendWrapped(uint8_t sub, const uint8_t* token, const uint8_t* payload,
                   size_t len) {
    if (fd < 0) return false;
    uint8_t buf[64 + 2048];
    const size_t hdr = buildClientHeader(buf, sizeof buf, cfg, sub, token, true);
    if (!hdr || hdr + len > sizeof buf) return false;
    std::memcpy(buf + hdr, payload, len);
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
  s.log(std::string("raknet: ID_CONNECTION_REQUEST sent ") + (ok ? "ok" : "failed"));
  return ok;
}

// ID_NEW_INCOMING_CONNECTION: 13 <our address> <peer address> <12 dummy
// 04 ff ff ff ff 00 00 entries>. RakNet addresses are [ver][ip4][port BE].
bool RaknetClient::sendNewIncoming() {
  Impl& s = *impl_;
  if (!s.data_ready) return false;
  uint8_t d[160];
  size_t o = 0;
  d[o++] = 0x13;
  auto addr = [&](const uint8_t ip[4], uint16_t port) {
    d[o++] = 0x04;
    std::memcpy(d + o, ip, 4);
    o += 4;
    d[o++] = uint8_t(port >> 8);
    d[o++] = uint8_t(port & 0xff);
  };
  uint8_t local_ip[4] = {127, 0, 0, 1};
  uint16_t local_port = 0;
  sockaddr_in self{};
  socklen_t sl = sizeof self;
  if (s.fd >= 0 && getsockname(s.fd, reinterpret_cast<sockaddr*>(&self), &sl) == 0) {
    std::memcpy(local_ip, &self.sin_addr.s_addr, 4);
    local_port = ntohs(self.sin_port);
  }
  addr(local_ip, local_port);
  addr(reinterpret_cast<const uint8_t*>(&s.peer.sin_addr.s_addr),
       s.cfg.rcc_port ? s.cfg.rcc_port : ntohs(s.peer.sin_port));
  for (int i = 0; i < 12; ++i) {
    const uint8_t dummy[4] = {0xff, 0xff, 0xff, 0xff};
    addr(dummy, 0);
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
uint64_t RaknetClient::datagrams_tx() const { return impl_->dgram_tx; }
uint64_t RaknetClient::split_messages_tx() const { return impl_->split_tx; }

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
uint64_t RaknetClient::dups_dropped() const { return impl_->dups_dropped; }

const uint8_t* RaknetClient::last_datagram() const {
  return impl_->last_dgram.empty() ? nullptr : impl_->last_dgram.data();
}
size_t RaknetClient::last_datagram_len() const { return impl_->last_dgram.size(); }
uint64_t RaknetClient::data_datagrams_rx() const { return impl_->data_rx_ok; }
uint64_t RaknetClient::data_frames_rx() const { return impl_->data_rx_frames; }

bool RaknetClient::send(const uint8_t* payload, size_t len) {
  Impl& s = *impl_;
  if (s.stage < 1) return false;
  const uint8_t* tok = s.have_session ? s.session_token : s.cfg.join_token;
  const uint8_t sub = s.have_session ? 0x02 : 0x01;
  return s.sendWrapped(sub, tok, payload, len);
}

int RaknetClient::pump(int timeout_ms) {
  Impl& s = *impl_;
  if (s.fd < 0) return 0;
  int got = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - std::chrono::steady_clock::now())
                          .count();
    if (left <= 0) break;
    timeval tv{};
    tv.tv_sec = int(left / 1000);
    tv.tv_usec = int((left % 1000) * 1000);
    ::setsockopt(s.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    uint8_t buf[2048];
    sockaddr_in from{};
    socklen_t flen = sizeof from;
    const ssize_t n = ::recvfrom(s.fd, buf, sizeof buf, 0,
                                 reinterpret_cast<sockaddr*>(&from), &flen);
    if (n <= 0) break;
    ++got;
    ++s.pkts_rx;
    s.bytes_rx += size_t(n);
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
    if (v.has_token && v.token_sub == 2 && !s.have_session) {
      std::memcpy(s.session_token, v.token, 16);
      s.have_session = true;
      char hex[33];
      for (int i = 0; i < 16; ++i) snprintf(hex + i * 2, 3, "%02x", v.token[i]);
      s.log(std::string("raknet: session token issued: ") + hex);
    }
    ++s.payloads_rx;
    if (v.len == 0) continue;
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
        s.last_dgram.assign(out, out + olen);
        ++s.data_rx_ok;
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
          // ACK from the server: transport only, nothing to reply to.
          const unsigned cnt = (unsigned(out[1]) << 8) | out[2];
          char b[160];
          snprintf(b, sizeof b, "raknet: server ACK (%u range%s, %zuB)", cnt,
                   cnt == 1 ? "" : "s", olen);
          s.log(b);
          ++s.server_acks;
          continue;
        }
        if (olen >= 4 && (out[0] & 0x80)) {
          const uint32_t num = uint32_t(out[1]) | (uint32_t(out[2]) << 8) |
                               (uint32_t(out[3]) << 16);
          if (!s.seen_dgrams.insert(num).second) {
            ++s.dups_dropped;
            continue; // retransmission of a datagram we already have
          }
          s.pending_acks.push_back(num);
          s.parseMessages(out, olen);
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
    if (idName(id)) {
      s.log(std::string("raknet: ") + idName(id) + " " +
            std::to_string(v.len) + "B tlv=" + std::to_string(v.total));
    }
  }
  return got;
}

void RaknetClient::close() {
  Impl& s = *impl_;
  if (s.fd >= 0) {
    ::close(s.fd);
    s.fd = -1;
  }
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
