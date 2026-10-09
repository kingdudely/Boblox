#include "quic_client.h"

#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <openssl/err.h>
#include <openssl/rand.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace rbx {

namespace {

ngtcp2_tstamp mono_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ngtcp2_tstamp(ts.tv_sec) * 1000000000ull + ngtcp2_tstamp(ts.tv_nsec);
}

std::string openssl_err() {
  unsigned long e = ERR_get_error();
  if (!e) return "";
  char buf[256];
  ERR_error_string_n(e, buf, sizeof buf);
  return buf;
}

} // namespace

QuicClient::~QuicClient() {
  if (ssl_) SSL_set_app_data(ssl_, nullptr); // stop conn_ref callbacks first
  if (conn_) {
    ngtcp2_conn_del(conn_);
    conn_ = nullptr;
  }
  if (ssl_) {
    SSL_free(ssl_);
    ssl_ = nullptr;
  }
  if (ossl_ctx_) {
    ngtcp2_crypto_ossl_ctx_del(ossl_ctx_);
    ossl_ctx_ = nullptr;
  }
  if (ssl_ctx_) {
    SSL_CTX_free(ssl_ctx_);
    ssl_ctx_ = nullptr;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

void QuicClient::fail(const std::string& why) {
  if (failed_) return;
  failed_ = true;
  last_error_ = why;
  fprintf(stderr, "[quic] error: %s\n", why.c_str());
}

bool QuicClient::init(const Config& cfg) {
  cfg_ = cfg;

  // ---- remote/local addresses -------------------------------------------
  sockaddr_in ra{};
  ra.sin_family = AF_INET;
  ra.sin_port = htons(cfg.port);
  if (inet_pton(AF_INET, cfg.udmux_ip.c_str(), &ra.sin_addr) != 1) {
    fail("bad udmux ip: " + cfg.udmux_ip);
    return false;
  }
  std::memcpy(&remote_sa_, &ra, sizeof ra);
  remote_len_ = sizeof ra;

  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) {
    fail("socket: " + std::string(strerror(errno)));
    return false;
  }
  sockaddr_in la{};
  la.sin_family = AF_INET;
  la.sin_addr.s_addr = htonl(INADDR_ANY);
  if (::bind(fd_, reinterpret_cast<sockaddr*>(&la), sizeof la) != 0) {
    fail("bind: " + std::string(strerror(errno)));
    return false;
  }
  if (::connect(fd_, reinterpret_cast<sockaddr*>(&ra), sizeof ra) != 0) {
    fail("connect: " + std::string(strerror(errno)));
    return false;
  }
  int fl = ::fcntl(fd_, F_GETFL, 0);
  ::fcntl(fd_, F_SETFL, fl | O_NONBLOCK);
  local_len_ = sizeof local_sa_;
  ::getsockname(fd_, reinterpret_cast<sockaddr*>(&local_sa_), &local_len_);

  path_.local.addr = reinterpret_cast<ngtcp2_sockaddr*>(&local_sa_);
  path_.local.addrlen = local_len_;
  path_.remote.addr = reinterpret_cast<ngtcp2_sockaddr*>(&remote_sa_);
  path_.remote.addrlen = remote_len_;

  // RUPP header: token + rcc_ip:NetStackPort (working Python-client bytes).
  std::memcpy(rupp_.token, cfg.token, 16);
  if (inet_pton(AF_INET, cfg.rcc_ip.c_str(), rupp_.rcc_ip) != 1) {
    fail("bad rcc ip: " + cfg.rcc_ip);
    return false;
  }
  rupp_.rcc_port = cfg.port;

  // ---- SSL_CTX ----------------------------------------------------------
  static const bool ossl_inited = (ngtcp2_crypto_ossl_init() == 0);
  (void)ossl_inited;

  ssl_ctx_ = SSL_CTX_new(TLS_client_method());
  if (!ssl_ctx_) {
    fail("SSL_CTX_new: " + openssl_err());
    return false;
  }
  SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_3_VERSION);
  SSL_CTX_set_max_proto_version(ssl_ctx_, TLS1_3_VERSION);
  // Native CH offers only 0x1302, 0x1303 (TLS_AES_256_GCM_SHA384 +
  // TLS_CHACHA20_POLY1305_SHA256) — in that order.
  SSL_CTX_set_ciphersuites(
      ssl_ctx_, "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256");
  // Native CH supported_groups = x25519 only.
  SSL_CTX_set1_groups_list(ssl_ctx_, "X25519");
  SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_NONE, nullptr);
  // Native (OpenSSL 3.5.0) predates RFC8879 cert compression: no
  // compress_certificate (0x001b) extension in its CH. This build's OpenSSL
  // 3.5.7 sends one by default — disable it (NO_RX = don't advertise).
  SSL_CTX_set_options(ssl_ctx_, SSL_OP_NO_TX_CERTIFICATE_COMPRESSION |
                                    SSL_OP_NO_RX_CERTIFICATE_COMPRESSION);

  // Custom extension 0xFF00 — registered FIRST so it appears first in the
  // ClientHello (native order: 0xff00, 0x0039 TP, 0x0000 SNI, ...). Native
  // context: 1152 = SSL_EXT_CLIENT_HELLO|SSL_EXT_TLS1_3_ENCRYPTED_EXTENSIONS.
  if (SSL_CTX_add_custom_ext(
          ssl_ctx_, 0xFF00,
          SSL_EXT_CLIENT_HELLO | SSL_EXT_TLS1_3_ENCRYPTED_EXTENSIONS,
          ext_add_cb, nullptr, this, ext_parse_cb, this) != 1) {
    fail("SSL_CTX_add_custom_ext(0xFF00): " + openssl_err());
    return false;
  }

  // ---- conn_ref ---------------------------------------------------------
  conn_ref_.get_conn = &QuicClient::get_conn_cb;
  conn_ref_.user_data = this;

  // ---- random material --------------------------------------------------
  RAND_bytes(static_secret_, sizeof static_secret_);
  std::memcpy(caps_wire_, &cfg.local_caps, 8); // u64 LE on the wire

  ngtcp2_cid scid{}, dcid{};
  scid.datalen = 20; // native Initials use 20-byte DCID/SCID
  dcid.datalen = 20;
  RAND_bytes(scid.data, scid.datalen);
  RAND_bytes(dcid.data, dcid.datalen);

  // ---- ngtcp2 settings (runtime dump: run/quicdump_settings) ------------
  ngtcp2_settings settings{};
  ngtcp2_settings_default(&settings);
  settings.initial_ts = mono_ns(); // dump had 0; consistent clock base required
  settings.initial_rtt = 333333333;
  settings.max_tx_udp_payload_size = 1452;
  settings.ack_thresh = 2;
  settings.handshake_timeout = 10000000000ull; // 10s (native)
  settings.glitch_ratelim_burst = 10000;
  settings.glitch_ratelim_rate = 330;
  settings.cc_algo = NGTCP2_CC_ALGO_BBR; // value 2 = BBR v2 in this build
  settings.initial_pkt_num = 0;
  {
    uint32_t pn = 0;
    RAND_bytes(reinterpret_cast<uint8_t*>(&pn), 4);
    settings.initial_pkt_num = pn & 0x7fffffffu; // random 31-bit (native)
  }

  // ---- transport params (stock encoder => byte-identical 88B extension) --
  ngtcp2_transport_params params{};
  ngtcp2_transport_params_default(&params);
  params.initial_max_stream_data_bidi_local = 1410065407;
  params.initial_max_stream_data_bidi_remote = 1410065407;
  params.initial_max_stream_data_uni = 1410065407;
  params.initial_max_data = 141006550700;
  params.initial_max_streams_bidi = 0;
  params.initial_max_streams_uni = 1024;
  params.max_idle_timeout = 20000000000ull; // 20s
  params.max_udp_payload_size = 65527;
  params.active_connection_id_limit = 2;
  params.ack_delay_exponent = 3;
  params.max_ack_delay = 25000000; // 25ms
  params.max_datagram_frame_size = 65535;

  // ---- ngtcp2 conn ------------------------------------------------------
  static const ngtcp2_callbacks cbs = [] {
    ngtcp2_callbacks c{};
    c.client_initial = ngtcp2_crypto_client_initial_cb;
    c.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    c.handshake_completed = &QuicClient::cb_handshake_completed;
    c.encrypt = ngtcp2_crypto_encrypt_cb;
    c.decrypt = ngtcp2_crypto_decrypt_cb;
    c.hp_mask = ngtcp2_crypto_hp_mask_cb;
    c.recv_stream_data = &QuicClient::cb_recv_stream_data;
    c.acked_stream_data_offset = &QuicClient::cb_acked_stream_data_offset;
    c.recv_retry = ngtcp2_crypto_recv_retry_cb;
    c.rand = &QuicClient::cb_rand;
    c.update_key = ngtcp2_crypto_update_key_cb;
    c.stream_reset = &QuicClient::cb_stream_reset;
    c.handshake_confirmed = &QuicClient::cb_handshake_confirmed;
    c.recv_new_token = &QuicClient::cb_recv_new_token;
    c.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    c.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    c.stream_stop_sending = &QuicClient::cb_stream_stop_sending;
    c.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    c.get_new_connection_id2 = &QuicClient::cb_get_new_connection_id2;
    c.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
    c.stream_close2 = &QuicClient::cb_stream_close2;
    c.recv_datagram = &QuicClient::cb_recv_datagram;
    return c;
  }();

  int rv = ngtcp2_conn_client_new(&conn_, &dcid, &scid, &path_,
                                  NGTCP2_PROTO_VER_V1, &cbs, &settings,
                                  &params, nullptr, this);
  if (rv != 0) {
    fail(std::string("ngtcp2_conn_client_new: ") + ngtcp2_strerror(rv));
    return false;
  }
  // Keepalive: after 1s of TX idle send a keep-alive packet — mirrors
  // probe10's 1s QUIC PING loop (server idle refresh).
  ngtcp2_conn_set_keep_alive_timeout(conn_, 1000000000ull);

  // ---- SSL (order mirrors ngtcp2 examples/tls_client_session_ossl.cc) ----
  ssl_ = SSL_new(ssl_ctx_);
  if (!ssl_) {
    fail("SSL_new: " + openssl_err());
    return false;
  }
  ngtcp2_crypto_ossl_ctx_new(&ossl_ctx_, ssl_);
  if (ngtcp2_crypto_ossl_configure_client_session(ssl_) != 0) {
    fail("configure_client_session: " + openssl_err());
    return false;
  }
  SSL_set_app_data(ssl_, &conn_ref_);
  SSL_set_connect_state(ssl_);

  // ALPN: native game conn uses "rbx-rtcio" (wire format = len-prefixed).
  {
    std::string a = cfg.alpn;
    if (a.size() > 255) {
      fail("alpn too long");
      return false;
    }
    unsigned char wire[256];
    wire[0] = (unsigned char)a.size();
    std::memcpy(wire + 1, a.data(), a.size());
    if (SSL_set_alpn_protos(ssl_, wire, (unsigned)(1 + a.size())) != 0) {
      fail("SSL_set_alpn_protos: " + openssl_err());
      return false;
    }
  }

  // Native sends a literal-IP SNI (its udmux endpoint).
  SSL_set_tlsext_host_name(ssl_, cfg.udmux_ip.c_str());

  // Native ClientHello offers server_certificate_type (0x0014) with body
  // "0102" — verify against our tx dump and tune the val/len if OpenSSL
  // serializes differently.
  if (SSL_set1_server_cert_type(ssl_, (const unsigned char*)"\x02", 1) != 1) {
    fprintf(stderr, "[quic] warn: SSL_set1_server_cert_type: %s\n",
            openssl_err().c_str());
  }

  ngtcp2_conn_set_tls_native_handle(conn_, ossl_ctx_);
  return true;
}

// ---------------------------------------------------------------------------
// events / accessors
// ---------------------------------------------------------------------------

std::string QuicClient::alpn_selected() const {
  if (!ssl_) return "";
  const unsigned char* p = nullptr;
  unsigned int n = 0;
  SSL_get0_alpn_selected(ssl_, &p, &n);
  return std::string(reinterpret_cast<const char*>(p), n);
}

std::string QuicClient::cipher_name() const {
  if (!ssl_) return "";
  const SSL_CIPHER* c = SSL_get_current_cipher(ssl_);
  return c ? SSL_CIPHER_get_name(c) : "";
}

// ---------------------------------------------------------------------------
// event loop
// ---------------------------------------------------------------------------

void QuicClient::pump(int timeout_ms) {
  if (!conn_ || failed_) return;

  write_pending();
  if (failed_) return;

  ngtcp2_tstamp now = mono_ns();
  int64_t wait_ns = int64_t(ngtcp2_conn_get_expiry(conn_)) - int64_t(now);
  int64_t cap_ns = int64_t(timeout_ms) * 1000000;
  if (cap_ns < 0) cap_ns = 0;
  if (wait_ns > cap_ns) wait_ns = cap_ns;
  if (wait_ns < 0) wait_ns = 0;
  int wait_ms = int((wait_ns + 999999) / 1000000);

  pollfd p{fd_, POLLIN, 0};
  int pr = ::poll(&p, 1, wait_ms);
  if (pr < 0) {
    if (errno != EINTR) fail(std::string("poll: ") + strerror(errno));
    return;
  }
  if (pr > 0) {
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      fail("socket error revents=" + std::to_string(p.revents));
      return;
    }
    if (p.revents & POLLIN) recv_ready();
    if (failed_) return;
  }

  now = mono_ns();
  if (ngtcp2_conn_get_expiry(conn_) <= now) {
    NgGuard g(in_ng_);
    int rv = ngtcp2_conn_handle_expiry(conn_, now);
    if (rv != 0) {
      fail(std::string("handle_expiry: ") + ngtcp2_strerror(rv));
      return;
    }
  }
  write_pending();
  if (failed_) return;
  deliverEvents(); // app callbacks — OUTSIDE ngtcp2
}

void QuicClient::deliverEvents() {
  while (!evq_.empty() && !failed_) {
    Event ev = std::move(evq_.front());
    evq_.pop_front();
    switch (ev.type) {
    case Event::HS:
      if (on_handshake_complete) on_handshake_complete();
      break;
    case Event::CONFIRMED:
      break;
    case Event::STREAM:
      if (on_stream_data)
        on_stream_data(ev.sid, ev.data.data(), ev.data.size(), ev.fin);
      break;
    case Event::RESET:
      if (on_stream_reset) on_stream_reset(ev.sid, ev.code);
      break;
    case Event::STOP:
      if (on_stop_sending) on_stop_sending(ev.sid, ev.code);
      break;
    }
  }
}

bool QuicClient::run_until_handshake(int timeout_ms) {
  ngtcp2_tstamp deadline = mono_ns() + ngtcp2_tstamp(timeout_ms) * 1000000ull;
  while (!hs_complete_ && !failed_) {
    ngtcp2_tstamp now = mono_ns();
    if (now >= deadline) {
      fail("handshake timeout");
      break;
    }
    int remain = int((deadline - now) / 1000000ull) + 1;
    pump(remain);
  }
  return hs_complete_;
}

void QuicClient::write_pending() {
  NgGuard g(in_ng_);
  drainTx(); // queued stream payloads first (acks piggyback)
  if (failed_) return;
  uint8_t buf[4096];
  for (;;) {
    ngtcp2_pkt_info pi{};
    ngtcp2_ssize n = ngtcp2_conn_write_pkt(conn_, &path_, &pi, buf, sizeof buf,
                                           mono_ns());
    if (n < 0) {
      fail(std::string("write_pkt: ") + ngtcp2_strerror(n));
      return;
    }
    if (n == 0) break;
    send_raw(buf, size_t(n));
    if (failed_) return;
  }
}

void QuicClient::recv_ready() {
  uint8_t buf[65536];
  for (;;) {
    ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EINTR) continue;
      fail(std::string("recv: ") + strerror(errno));
      return;
    }
    if (n == 0) break;
    size_t plen = 0;
    const uint8_t* q = rupp_strip(buf, size_t(n), &plen);
    if (!q) {
      rupp_dropped_++; // non-RUPP datagram — drop (Python-client semantics)
      continue;
    }
    if (plen == 0) continue;
    rx_packets_++;
    ngtcp2_pkt_info pi{};
    int rv;
    {
      NgGuard g(in_ng_);
      rv = ngtcp2_conn_read_pkt(conn_, &path_, &pi, q, plen, mono_ns());
    }
    if (rv != 0) {
      fail(std::string("read_pkt: ") + ngtcp2_strerror(rv));
      return;
    }
  }
}

void QuicClient::send_raw(const uint8_t* quic, size_t len) {
  uint8_t wire[RUPP_HEADER_LEN + 4096];
  size_t wl = rupp_wrap(wire, sizeof wire, rupp_, quic, len);
  if (wl == 0) {
    fail("rupp wrap overflow");
    return;
  }
  ssize_t s = ::send(fd_, wire, wl, 0);
  if (s < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) return;
    fail(std::string("send: ") + strerror(errno));
    return;
  }
  tx_packets_++;
  if (!tx_dump_dir_.empty()) {
    char path[512];
    snprintf(path, sizeof path, "%s/tx%03d.bin", tx_dump_dir_.c_str(),
             ++tx_dump_n_);
    if (FILE* f = fopen(path, "wb")) {
      fwrite(quic, 1, len, f);
      fclose(f);
    }
  }
}

// ---------------------------------------------------------------------------
// streams
// ---------------------------------------------------------------------------

int64_t QuicClient::open_bidi_stream() {
  int64_t sid = -1;
  int rv = ngtcp2_conn_open_bidi_stream(conn_, &sid, nullptr);
  if (rv != 0) {
    fail(std::string("open_bidi_stream: ") + ngtcp2_strerror(rv));
    return -1;
  }
  streams_[sid]; // register
  return sid;
}

bool QuicClient::send_stream(int64_t sid, const uint8_t* data, size_t len,
                             bool fin) {
  if (!conn_ || failed_) return false;
  TxChunk c;
  c.data.assign(data, data + len);
  c.fin = fin;
  txq_[sid].push_back(std::move(c));
  // Attempt immediate drain (matches aioquic's eager flush); flow-control
  // blocks simply leave data queued for later write_pending() calls. Never
  // drain while inside an ngtcp2 call (would be reentrant).
  if (in_ng_ == 0) drainStream(sid);
  return !failed_;
}

void QuicClient::drainTx() {
  // Snapshot keys: drainStream may not modify the map shape (only chunks).
  std::vector<int64_t> sids;
  sids.reserve(txq_.size());
  for (auto& kv : txq_) sids.push_back(kv.first);
  for (int64_t sid : sids) {
    drainStream(sid);
    if (failed_) return;
  }
}

void QuicClient::drainStream(int64_t sid) {
  auto it = txq_.find(sid);
  if (it == txq_.end()) return;
  auto& q = it->second;
  uint8_t buf[4096];
  int guard = 0;
  NgGuard g(in_ng_);
  while (!q.empty() && guard++ < 512) {
    TxChunk& c = q.front();
    size_t nleft = c.data.size() - c.off;
    // FIN applies only on the call that writes the final bytes of the final
    // chunk queued for this stream.
    bool final_call =
        (q.size() == 1) && (c.off + nleft == c.data.size());
    uint32_t flags = (c.fin && final_call) ? NGTCP2_WRITE_STREAM_FLAG_FIN : 0;
    ngtcp2_pkt_info pi{};
    ngtcp2_ssize nd = 0;
    ngtcp2_ssize n =
        ngtcp2_conn_write_stream(conn_, &path_, &pi, buf, sizeof buf, &nd,
                                 flags, sid, c.data.data() + c.off, nleft,
                                 mono_ns());
    if (n < 0) {
      fail(std::string("write_stream: ") + ngtcp2_strerror(n));
      return;
    }
    if (n == 0) break; // blocked (flow/congestion control) — retry on next pump
    send_raw(buf, size_t(n));
    if (failed_) return;
    if (nd > 0) c.off += size_t(nd);
    if (c.off >= c.data.size()) q.pop_front();
  }
}

// ---------------------------------------------------------------------------
// ngtcp2 callbacks
// ---------------------------------------------------------------------------

ngtcp2_conn* QuicClient::get_conn_cb(ngtcp2_crypto_conn_ref* ref) {
  return static_cast<QuicClient*>(ref->user_data)->conn_;
}

int QuicClient::cb_handshake_completed(ngtcp2_conn*, void* user_data) {
  auto* c = static_cast<QuicClient*>(user_data);
  c->hs_complete_ = true;
  fprintf(stderr, "[quic] handshake completed (ALPN=%s cipher=%s)\n",
          c->alpn_selected().c_str(), c->cipher_name().c_str());
  Event ev;
  ev.type = Event::HS;
  c->evq_.push_back(std::move(ev));
  return 0;
}

int QuicClient::cb_handshake_confirmed(ngtcp2_conn*, void* user_data) {
  auto* c = static_cast<QuicClient*>(user_data);
  c->hs_confirmed_ = true;
  fprintf(stderr, "[quic] handshake confirmed\n");
  Event ev;
  ev.type = Event::CONFIRMED;
  c->evq_.push_back(std::move(ev));
  return 0;
}

int QuicClient::cb_recv_stream_data(ngtcp2_conn*, uint32_t flags,
                                    int64_t stream_id, uint64_t offset,
                                    const uint8_t* data, size_t datalen,
                                    void* user_data, void*) {
  auto* c = static_cast<QuicClient*>(user_data);
  auto& st = c->streams_[stream_id];
  if (offset <= st.buf.size()) {
    size_t skip = st.buf.size() - size_t(offset);
    if (skip < datalen)
      st.buf.insert(st.buf.end(), data + skip, data + datalen);
  }
  bool fin = (flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0;
  if (fin) st.fin = true;
  Event ev;
  ev.type = Event::STREAM;
  ev.sid = stream_id;
  ev.data.assign(data, data + datalen);
  ev.fin = fin;
  c->evq_.push_back(std::move(ev));
  return 0;
}

int QuicClient::cb_acked_stream_data_offset(ngtcp2_conn*, int64_t, uint64_t,
                                            uint64_t, void*, void*) {
  return 0;
}

int QuicClient::cb_stream_reset(ngtcp2_conn*, int64_t stream_id,
                                uint64_t, uint64_t app_error_code,
                                void* user_data, void*) {
  auto* c = static_cast<QuicClient*>(user_data);
  auto& st = c->streams_[stream_id];
  st.reset = true;
  st.reset_code = app_error_code;
  fprintf(stderr, "[quic] stream %lld RESET code=%llu\n",
          (long long)stream_id, (unsigned long long)app_error_code);
  Event ev;
  ev.type = Event::RESET;
  ev.sid = stream_id;
  ev.code = app_error_code;
  c->evq_.push_back(std::move(ev));
  return 0;
}

int QuicClient::cb_stream_stop_sending(ngtcp2_conn*, int64_t stream_id,
                                       uint64_t app_error_code, void* user_data,
                                       void*) {
  auto* c = static_cast<QuicClient*>(user_data);
  fprintf(stderr, "[quic] stream %lld STOP_SENDING code=%llu\n",
          (long long)stream_id, (unsigned long long)app_error_code);
  Event ev;
  ev.type = Event::STOP;
  ev.sid = stream_id;
  ev.code = app_error_code;
  c->evq_.push_back(std::move(ev));
  return 0;
}

int QuicClient::cb_stream_close2(ngtcp2_conn*, uint32_t, int64_t,
                                 uint64_t, uint64_t, void*, void*) {
  return 0;
}

int QuicClient::cb_recv_new_token(ngtcp2_conn*, const uint8_t*, size_t,
                                  void*) {
  return 0;
}

int QuicClient::cb_get_new_connection_id2(ngtcp2_conn*, ngtcp2_cid* cid,
                                          ngtcp2_stateless_reset_token* token,
                                          size_t cidlen, void* user_data) {
  auto* c = static_cast<QuicClient*>(user_data);
  cid->datalen = cidlen;
  RAND_bytes(cid->data, int(cidlen));
  if (ngtcp2_crypto_generate_stateless_reset_token(
          token->data, c->static_secret_, sizeof c->static_secret_, cid) != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }
  return 0;
}

void QuicClient::cb_rand(uint8_t* dest, size_t destlen,
                         const ngtcp2_rand_ctx*) {
  RAND_bytes(dest, int(destlen));
}

int QuicClient::cb_recv_datagram(ngtcp2_conn*, uint32_t, const uint8_t* data,
                                 size_t datalen, void*) {
  size_t m = datalen < 40 ? datalen : 40;
  char hex[81];
  for (size_t i = 0; i < m; i++) snprintf(hex + 2 * i, 3, "%02x", data[i]);
  hex[2 * m] = 0;
  fprintf(stderr, "[quic] DATAGRAM %zuB: %s\n", datalen, hex);
  return 0;
}

// ---------------------------------------------------------------------------
// TLS custom extension 0xFF00 — capabilities exchange
// ---------------------------------------------------------------------------

int QuicClient::ext_add_cb(SSL*, unsigned int, unsigned int,
                           const unsigned char** out, size_t* outlen, X509*,
                           size_t, int*, void* add_arg) {
  auto* c = static_cast<QuicClient*>(add_arg);
  // Native: getter result < 256 bytes else alert 80. We always send the
  // 8-byte LE capabilities u64 (native conn#0: 92 08 b7 50 c7 d3 2d 00).
  *out = c->caps_wire_;
  *outlen = 8;
  return 1; // send it
}

int QuicClient::ext_parse_cb(SSL*, unsigned int, unsigned int,
                             const unsigned char* in, size_t inlen, X509*,
                             size_t, int*, void* parse_arg) {
  auto* c = static_cast<QuicClient*>(parse_arg);
  // Receive side (sub_638CFB6): caps valid only if exactly 8 bytes, else 0.
  if (in && inlen == 8) {
    std::memcpy(&c->peer_caps_, in, 8);
    c->peer_caps_present_ = true;
  } else {
    c->peer_caps_ = 0;
    c->peer_caps_present_ = false;
  }
  fprintf(stderr, "[quic] server caps ext: len=%zu present=%d\n", inlen,
          int(c->peer_caps_present_));
  return 1; // native parse cb always accepts at the TLS level
}

} // namespace rbx
