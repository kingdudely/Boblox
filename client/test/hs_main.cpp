// hs1818 — QUIC handshake smoke test against a real Roblox game server.
//
//   1. fetch a join reply:
//        RBX_COOKIE_FILE=run/cookie2.txt python3 tools/dump_join.py 1818 run/join_fresh.json
//   2. run:
//        ./client/build/hs1818 run/join_fresh.json --seconds 12 \
//            --tx-dump /home/john/RobloxInBrowser/run/cpp_tx
//
// Success = handshake completes (native-parity ClientHello), server caps ext
// received, connection survives --seconds without IDLE_CLOSE / reset, and any
// server stream data is printed.
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "quic_client.h"

using json = nlohmann::json;
using rbx::QuicClient;

namespace {

std::string b64decode(const std::string& in) {
  static const std::string alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int val = 0, valb = -8;
  std::string out;
  for (char ch : in) {
    if (ch == '=' || ch == '\n' || ch == '\r') break;
    size_t p = alphabet.find(ch);
    if (p == std::string::npos) continue;
    val = (val << 6) + int(p);
    valb += 6;
    if (valb >= 0) {
      out.push_back(char((val >> valb) & 0xff));
      valb -= 8;
    }
  }
  return out;
}

std::string hex_prefix(const uint8_t* p, size_t n, size_t max = 32) {
  char buf[4 * 64];
  size_t m = n < max ? n : max;
  size_t k = 0;
  for (size_t i = 0; i + 1 < 2 * m && k + 3 < sizeof buf; i += 2) {
    static const char* d = "0123456789abcdef";
    buf[k++] = d[p[i] >> 4];
    buf[k++] = d[p[i] & 0xf];
  }
  buf[k] = 0;
  return std::string(buf, k) + (n > max ? "..." : "");
}

void usage() {
  fprintf(stderr,
          "usage: hs1818 <join.json> [--seconds N] [--alpn NAME] "
          "[--tx-dump DIR]\n");
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  std::string join_path = argv[1];
  int seconds = 12;
  std::string alpn = "rbx-rtcio";
  std::string tx_dump;
  for (int i = 2; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--seconds" && i + 1 < argc) seconds = atoi(argv[++i]);
    else if (a == "--alpn" && i + 1 < argc) alpn = argv[++i];
    else if (a == "--tx-dump" && i + 1 < argc) tx_dump = argv[++i];
    else {
      usage();
      return 2;
    }
  }

  std::ifstream f(join_path);
  if (!f) {
    fprintf(stderr, "cannot open %s\n", join_path.c_str());
    return 2;
  }
  json js;
  try {
    f >> js;
  } catch (const std::exception& e) {
    fprintf(stderr, "bad json: %s\n", e.what());
    return 2;
  }
  const json& j = js.contains("joinScript") ? js["joinScript"] : js;

  QuicClient::Config cfg;
  cfg.alpn = alpn;
  try {
    cfg.udmux_ip = j.at("UdmuxEndpoints").at(0).at("Address").get<std::string>();
    cfg.rcc_ip = j.at("ServerConnections").at(0).at("Address").get<std::string>();
    const json& p = j.at("NetStackPort");
    cfg.port = uint16_t(p.is_number_integer() ? p.get<int>()
                                              : std::stoi(p.get<std::string>()));
    std::string tok = b64decode(j.at("NetStackTokenValue").get<std::string>());
    if (tok.size() != 16) {
      fprintf(stderr, "bad token length %zu (want 16)\n", tok.size());
      return 2;
    }
    memcpy(cfg.token, tok.data(), 16);
  } catch (const std::exception& e) {
    fprintf(stderr, "join json parse: %s\n", e.what());
    return 2;
  }

  auto jstr = [](const json& o, const char* k) -> std::string {
    auto it = o.find(k);
    if (it == o.end()) return "?";
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
  };
  printf("== join: place=%s job=%s udp=%s:%u rcc=%s alpn=%s caps=%016llx ==\n",
         jstr(j, "PlaceId").c_str(), jstr(j, "GameId").c_str(),
         cfg.udmux_ip.c_str(),
         unsigned(cfg.port), cfg.rcc_ip.c_str(), alpn.c_str(),
         (unsigned long long)cfg.local_caps);

  QuicClient qc;
  if (!tx_dump.empty()) qc.set_tx_dump_dir(tx_dump);

  uint64_t bytes = 0, msgs = 0;
  qc.on_stream_data = [&](int64_t sid, const uint8_t* data, size_t len,
                          bool fin) {
    if (len) {
      bytes += len;
      printf("[stream %lld] +%zu bytes fin=%d: %s\n", (long long)sid, len,
             int(fin), hex_prefix(data, len).c_str());
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
  printf("handshake=%s alpn=%s cipher=%s peer_caps=%016llx present=%d "
         "tx=%llu rx=%llu dropped=%llu err=%s\n",
         ok ? "OK" : "FAIL", qc.alpn_selected().c_str(),
         qc.cipher_name().c_str(), (unsigned long long)qc.peer_caps(),
         int(qc.peer_caps_present()), (unsigned long long)qc.tx_packets(),
         (unsigned long long)qc.rx_packets(),
         (unsigned long long)qc.rupp_dropped(),
         qc.last_error().c_str());
  if (!ok) return 1;

  // Keep the connection alive to observe server behavior (57B peer
  // assignment, N0 reset, kick...).
  for (int i = 0; i < seconds * 2 && !qc.failed(); i++) qc.pump(500);

  printf("after %ds: confirmed=%d failed=%d streams=%zu stream_bytes=%llu "
         "msgs=%llu tx=%llu rx=%llu err=%s\n",
         seconds, int(qc.hs_confirmed()), int(qc.failed()),
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
