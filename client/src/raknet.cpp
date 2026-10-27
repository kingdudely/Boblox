// raknet.cpp — Roblox RakNet offline handshake + RUPP session framing.
#include "raknet.h"

#include "rupp.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
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
  std::mt19937_64 rng;
  std::map<int, int> id_hist;

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
  if (!cfg.send_req2) {
    s.log("raknet: Req2 held back: tail is ticket-encrypted (key exchange not "
          "reversed yet)");
    return true;
  }
  // Req2 shape: [78][magic]<encrypted tail>. Only reachable once the
  // client-ticket/pepper key exchange lands; kept behind the flag so a
  // wrong guess is never sent silently.
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
  s.log("raknet: Req2 sent (experimental)");
  pump(2000);
  return true;
}

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
    const uint8_t id = v.payload[0];
    ++s.id_hist[int(id)];
    if (id == kReply1 && v.len >= 28) {
      std::memcpy(&s.guid, v.payload + 17, 8);
      s.mtu = uint16_t((v.payload[26] << 8) | v.payload[27]);
      s.stage = 1;
    } else if (id == kReply2) {
      s.stage = 2;
      s.log("raknet: Reply2 " + std::to_string(v.len) + "B (encrypted tail)");
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
