// QuicClient — RbxTransport QUIC connection (ngtcp2 + OpenSSL 3.5), the same
// stack family the native client uses.
//
// Fingerprint parity (see FINDINGS.md "Native game ClientHello + QUIC
// fingerprint" and "TLS custom extension 0xFF00 = CAPABILITIES exchange"):
//   - RUPP-wrapped UDP ([01 00 1f ...] header, subtype-1 token)
//   - TLS 1.3 only, ciphersuites 0x1302+0x1303
//     (TLS_AES_256_GCM_SHA384, TLS_CHACHA20_POLY1305_SHA256), groups X25519
//     only, no RFC8879 compress_certificate ext
//   - ALPN "rbx-rtcio", SNI = literal udmux IP
//   - TLS custom extension 0xFF00 registered FIRST (appears first in CH) and
//     carries the client's capabilities u64 LE (native conn#0:
//     0x002dd3c750b70892, bytes 92 08 b7 50 c7 d3 2d 00); the parse callback
//     stores the server's 8-byte caps (its netStackConfig.caps) and always
//     accepts at the TLS level, exactly like sub_638120C
//   - server_certificate_type (0x0014) offered, body must come out "0102"
//     like the native CH
//   - ngtcp2 settings/transport params mirror the runtime gdb dump
//     (run/quicdump_settings/index.txt): windows 1410065407 / 141006550700,
//     streams_uni 1024, idle 20s, handshake 10s, datagram 65535, BBR,
//     initial_rtt 333333333, max_tx_udp_payload 1452, random31 initial PN —
//     the stock ngtcp2_transport_params_encode then emits the byte-identical
//     88-byte TP extension.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <openssl/ssl.h>

#include "rupp.h"

namespace rbx {

struct StreamState {
  std::vector<uint8_t> buf; // received bytes (reassembly, best effort)
  bool fin = false;
  bool reset = false;
  uint64_t reset_code = 0;
};

class QuicClient {
public:
  struct Config {
    std::string udmux_ip; // UDP destination = native SNI (literal IP)
    uint16_t port = 0;    // NetStackPort (destination and RUPP TLV2 port)
    std::string rcc_ip;   // ServerConnections[0].Address (RUPP TLV2)
    uint8_t token[16]{};
    std::string alpn = "rbx-rtcio";
    uint64_t local_caps = 0x002dd3c750b70892ull; // native conn#0 caps
  };

  // -- events (session layer hooks) --
  std::function<void()> on_handshake_complete;
  std::function<void(int64_t sid, const uint8_t* data, size_t len, bool fin)>
      on_stream_data;
  std::function<void(int64_t sid, uint64_t error_code)> on_stream_reset;
  std::function<void(int64_t sid, uint64_t error_code)> on_stop_sending;

  QuicClient() = default;
  QuicClient(const QuicClient&) = delete;
  QuicClient& operator=(const QuicClient&) = delete;
  ~QuicClient();

  // Creates socket, SSL_CTX (+ custom ext 0xFF00), ngtcp2 conn, SSL.
  bool init(const Config& cfg);

  // One event-loop iteration: flush TX, poll up to min(timeout, next timer),
  // read + ngtcp2_conn_read_pkt, handle expiry, flush again.
  void pump(int timeout_ms);

  // Pump until the TLS/QUIC handshake completes or timeout_ms elapses.
  bool run_until_handshake(int timeout_ms);

  bool hs_complete() const { return hs_complete_; }
  bool hs_confirmed() const { return hs_confirmed_; }
  bool failed() const { return failed_; }
  const std::string& last_error() const { return last_error_; }

  std::string alpn_selected() const;
  std::string cipher_name() const;
  uint64_t peer_caps() const { return peer_caps_; }
  bool peer_caps_present() const { return peer_caps_present_; }

  // Stats (diagnostics).
  uint64_t tx_packets() const { return tx_packets_; }
  uint64_t rx_packets() const { return rx_packets_; }
  uint64_t rupp_dropped() const { return rupp_dropped_; }

  // Streams (queue-based: payloads are queued and drained by the event
  // loop, so flow-control blocks never lose data — mirrors aioquic's
  // send_stream_data buffering).
  int64_t open_bidi_stream();
  bool send_stream(int64_t sid, const uint8_t* data, size_t len, bool fin);
  bool send_stream(int64_t sid, const std::vector<uint8_t>& v,
                   bool fin = false) {
    return send_stream(sid, v.data(), v.size(), fin);
  }
  std::map<int64_t, StreamState>& streams() { return streams_; }

  // Dump every outgoing QUIC datagram (pre-RUPP) as <dir>/txNNN.bin so
  // tools/quic_initial_parse.py can decrypt and diff our ClientHello against
  // run/native_clienthello_tx001.bin.
  void set_tx_dump_dir(const std::string& dir) { tx_dump_dir_ = dir; }

private:
  // App events are queued while inside ngtcp2 (recv/handshake callbacks fire
  // from within ngtcp2_conn_read_pkt; invoking app code that calls back into
  // ngtcp2 would be reentrant). pump() delivers them afterwards, mirroring
  // aioquic's event queue.
  struct Event {
    enum Type { HS, CONFIRMED, STREAM, RESET, STOP } type = STREAM;
    int64_t sid = 0;
    std::vector<uint8_t> data;
    bool fin = false;
    uint64_t code = 0;
  };
  std::deque<Event> evq_;
  int in_ng_ = 0; // >0 while inside an ngtcp2 API call
  struct NgGuard {
    int& c;
    explicit NgGuard(int& x) : c(x) { ++c; }
    ~NgGuard() { --c; }
  };
  void deliverEvents();
  // ngtcp2 callbacks (thunks; user_data = this).
  static ngtcp2_conn* get_conn_cb(ngtcp2_crypto_conn_ref* ref);
  static int cb_handshake_completed(ngtcp2_conn* conn, void* user_data);
  static int cb_handshake_confirmed(ngtcp2_conn* conn, void* user_data);
  static int cb_recv_stream_data(ngtcp2_conn* conn, uint32_t flags,
                                 int64_t stream_id, uint64_t offset,
                                 const uint8_t* data, size_t datalen,
                                 void* user_data, void* stream_user_data);
  static int cb_acked_stream_data_offset(ngtcp2_conn* conn, int64_t stream_id,
                                         uint64_t offset, uint64_t datalen,
                                         void* user_data,
                                         void* stream_user_data);
  static int cb_stream_reset(ngtcp2_conn* conn, int64_t stream_id,
                             uint64_t final_size, uint64_t app_error_code,
                             void* user_data, void* stream_user_data);
  static int cb_stream_stop_sending(ngtcp2_conn* conn, int64_t stream_id,
                                    uint64_t app_error_code, void* user_data,
                                    void* stream_user_data);
  static int cb_stream_close2(ngtcp2_conn* conn, uint32_t flags,
                              int64_t stream_id, uint64_t rx_app_error_code,
                              uint64_t tx_app_error_code, void* user_data,
                              void* stream_user_data);
  static int cb_recv_new_token(ngtcp2_conn* conn, const uint8_t* token,
                               size_t tokenlen, void* user_data);
  static int cb_get_new_connection_id2(ngtcp2_conn* conn, ngtcp2_cid* cid,
                                       ngtcp2_stateless_reset_token* token,
                                       size_t cidlen, void* user_data);
  static void cb_rand(uint8_t* dest, size_t destlen,
                      const ngtcp2_rand_ctx* rand_ctx);
  static int cb_recv_datagram(ngtcp2_conn* conn, uint32_t flags,
                              const uint8_t* data, size_t datalen,
                              void* user_data);

  // OpenSSL custom extension 0xFF00 (capabilities exchange).
  static int ext_add_cb(SSL* s, unsigned int ext_type, unsigned int context,
                        const unsigned char** out, size_t* outlen, X509* x,
                        size_t chainidx, int* al, void* add_arg);
  static int ext_parse_cb(SSL* s, unsigned int ext_type, unsigned int context,
                          const unsigned char* in, size_t inlen, X509* x,
                          size_t chainidx, int* al, void* parse_arg);

  void fail(const std::string& why);
  void write_pending();
  void recv_ready();
  void send_raw(const uint8_t* quic, size_t len);

  struct TxChunk {
    std::vector<uint8_t> data;
    size_t off = 0;
    bool fin = false;
  };
  std::map<int64_t, std::deque<TxChunk>> txq_; // per-stream outbound queue
  void drainTx();
  void drainStream(int64_t sid);

  Config cfg_;
  bool failed_ = false;
  bool hs_complete_ = false;
  bool hs_confirmed_ = false;
  std::string last_error_;

  int fd_ = -1;
  sockaddr_storage local_sa_{};
  socklen_t local_len_ = 0;
  sockaddr_storage remote_sa_{};
  socklen_t remote_len_ = 0;
  ngtcp2_path path_{};
  RuppHeader rupp_{};

  SSL_CTX* ssl_ctx_ = nullptr;
  SSL* ssl_ = nullptr;
  ngtcp2_crypto_ossl_ctx* ossl_ctx_ = nullptr;
  ngtcp2_crypto_conn_ref conn_ref_{};
  ngtcp2_conn* conn_ = nullptr;

  uint8_t caps_wire_[8]{};
  uint64_t peer_caps_ = 0;
  bool peer_caps_present_ = false;

  uint8_t static_secret_[32]{};

  uint64_t tx_packets_ = 0;
  uint64_t rx_packets_ = 0;
  uint64_t rupp_dropped_ = 0;

  std::map<int64_t, StreamState> streams_;

  std::string tx_dump_dir_;
  int tx_dump_n_ = 0;
};

} // namespace rbx
