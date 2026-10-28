// hs_main.cpp — QUIC handshake smoke test against a real Roblox game server.
//
//   1. fetch a join reply:
//        RBX_COOKIE_FILE=run/cookie2.txt python3 tools/dump_join.py 1818 run/join_fresh.json
//   2. run:
//        timeout 12s ./build/client/hs1818 run/join_fresh.json
//
// Runs until interrupted; with RBX_TX_DUMP set, every outgoing QUIC
// datagram lands in <dir>/txNNN.bin for tools/quic_initial_parse.py.
//
// Success = handshake completes (native-parity ClientHello), server caps ext
// received, connection survives without IDLE_CLOSE / reset, and any
// server stream data is printed.
//
// The join script -> QuicClient::Config mapping is shared with the full client
// (quic/config.cpp), so this test exercises the same transport setup.
#include <nlohmann/json.hpp>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "common/hex.h"
#include "join/join.h"
#include "quic/config.h"
#include "quic/quic_client.h"

using json = nlohmann::json;
using rbx::QuicClient;

namespace {

std::string jstr(const json& o, const char* k) {
  auto it = o.find(k);
  if (it == o.end()) return "?";
  if (it->is_string()) return it->get<std::string>();
  return it->dump();
}

void usage() {
  fprintf(stderr, "usage: hs1818 <join.json>   (runs until interrupted)\n");
}

volatile std::sig_atomic_t g_stop = 0;
void on_stop(int) { g_stop = 1; }

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    usage();
    return 2;
  }
  rbx::JoinResult jr;
  std::string err;
  if (!rbx::loadJoin(argv[1], jr, &err)) {
    fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }
  const json& j = jr.join_script;

  QuicClient::Config cfg;
  if (!rbx::quic::configFromScript(j, &cfg, &err)) {
    fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }
  if (!rbx::quic::configFromEnv(&cfg, &err)) {
    fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }

  std::string tx_dump;
  if (const char* e = getenv("RBX_TX_DUMP")) tx_dump = e;

  printf("== join: place=%s job=%s udp=%s:%u rcc=%s alpn=%s caps=%s ==\n",
         jstr(j, "PlaceId").c_str(), jstr(j, "GameId").c_str(),
         cfg.udmux_ip.c_str(), unsigned(cfg.port), cfg.rcc_ip.c_str(),
         cfg.alpn.c_str(), rbx::hexU64(cfg.local_caps).c_str());

  QuicClient qc;
  if (!tx_dump.empty()) qc.set_tx_dump_dir(tx_dump);

  uint64_t bytes = 0, msgs = 0;
  qc.on_stream_data = [&](int64_t sid, const uint8_t* data, size_t len,
                          bool fin) {
    if (len) {
      bytes += len;
      printf("[stream %lld] +%zu bytes fin=%d: %s\n", (long long)sid, len,
             int(fin), rbx::hexPrefix(data, len).c_str());
    } else if (fin) {
      printf("[stream %lld] FIN\n", (long long)sid);
    }
    msgs++;
  };
  qc.on_stream_reset = [](int64_t sid, uint64_t code) {
    printf("[stream %lld] RESET code=%llu\n", (long long)sid,
           (unsigned long long)code);
  };
  qc.on_stop_sending = [](int64_t sid, uint64_t code) {
    printf("[stream %lld] STOP_SENDING code=%llu\n", (long long)sid,
           (unsigned long long)code);
  };

  if (!qc.init(cfg)) {
    printf("INIT FAILED: %s\n", qc.last_error().c_str());
    return 1;
  }

  bool ok = qc.run_until_handshake(10500);
  printf("handshake=%s alpn=%s cipher=%s peer_caps=%s present=%d "
         "tx=%llu rx=%llu dropped=%llu err=%s\n",
         ok ? "OK" : "FAIL", qc.alpn_selected().c_str(),
         qc.cipher_name().c_str(), rbx::hexU64(qc.peer_caps()).c_str(),
         int(qc.peer_caps_present()), (unsigned long long)qc.tx_packets(),
         (unsigned long long)qc.rx_packets(),
         (unsigned long long)qc.rupp_dropped(), qc.last_error().c_str());
  if (!ok) return 1;

  // Keep the connection alive to observe server behavior (57B peer
  // assignment, N0 reset, kick...) until interrupted.
  std::signal(SIGINT, on_stop);
  std::signal(SIGTERM, on_stop);
  int half_seconds = 0;
  while (!g_stop && !qc.failed()) {
    qc.pump(500);
    half_seconds++;
  }

  printf("after %ds: confirmed=%d failed=%d streams=%zu stream_bytes=%llu "
         "msgs=%llu tx=%llu rx=%llu err=%s\n",
         half_seconds / 2, int(qc.hs_confirmed()), int(qc.failed()),
         qc.streams().size(), (unsigned long long)bytes,
         (unsigned long long)msgs, (unsigned long long)qc.tx_packets(),
         (unsigned long long)qc.rx_packets(), qc.last_error().c_str());
  for (auto& kv : qc.streams()) {
    printf("  stream %lld: rx=%zu fin=%d reset=%d code=%llu\n",
           (long long)kv.first, kv.second.buf.size(), int(kv.second.fin),
           int(kv.second.reset), (unsigned long long)kv.second.reset_code);
  }
  return qc.failed() ? 1 : 0;
}
