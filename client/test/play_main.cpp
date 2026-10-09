// rbxplay — fully local 0x9B solver client (C++ port of py/probe10.py).
//
//   1. join (gamejoin v2 SSE / v1 fallback) or load a saved join
//   2. QUIC connect (native-parity fingerprint: rbx-rtcio, caps ext, RUPP)
//   3. session burst: ctrl hello + openU c3/c5/c6/c11, early-auth, A7,
//      0x90, 0x92, 0x8A, 0x8F on chan1
//   4. detect [9b] challenge on chan1 -> solveMessage() in-process ->
//      send [9b][u2][answer] -> +60ms route declarations on chan11
//   5. keep pumping: success = post-answer traffic keeps flowing (peer
//      assignment / replication) instead of a stream reset
//
// Usage:
//   RBX_COOKIE_FILE=run/cookie2.txt ./client/build/rbxplay --seconds 90
//   ./client/build/rbxplay --join-json run/join_full.json --seconds 60
//   ./client/build/rbxplay --join-json ... --dump-msgs run/cpp_msgs   # offline
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "join.h"
#include "quic_client.h"
#include "session.h"
#include "util.h"

using json = nlohmann::json;
using rbx::DummySession;
using rbx::QuicClient;
using rbx::Session;
using rbx::SessionOptions;

namespace {

int64_t ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

int64_t g_start = 0;

void logLine(const std::string& s) {
  printf("[%8.3f] %s\n", (ms() - g_start) / 1000.0, s.c_str());
  fflush(stdout);
}

void usage() {
  fprintf(stderr,
          "usage: rbxplay [--place N] [--seconds N] [--cookie-file F]\n"
          "               [--join-json F] [--save-join F] [--job UUID]\n"
          "               [--a7 empty|real|skip] [--corrupt] [--no-routes]\n"
          "               [--dummy skip|full] [--alpn NAME] [--tx-dump DIR]\n"
          "               [--dump-msgs DIR] [--cap-dir DIR]\n");
}

// Extract QuicClient::Config from a joinScript (same fields hs1818 uses).
bool configFromScript(const json& j, const std::string& alpn,
                      QuicClient::Config& cfg, std::string* err) {
  cfg.alpn = alpn;
  try {
    cfg.udmux_ip = j.at("UdmuxEndpoints").at(0).at("Address").get<std::string>();
    cfg.rcc_ip = j.at("ServerConnections").at(0).at("Address").get<std::string>();
    const json& p = j.at("NetStackPort");
    cfg.port = uint16_t(p.is_number_integer() ? p.get<int>()
                                              : std::stoi(p.get<std::string>()));
    std::string tok = rbx::b64decode(j.at("NetStackTokenValue").get<std::string>());
    if (tok.size() != 16) {
      if (err) *err = "bad NetStackTokenValue length (want 16)";
      return false;
    }
    memcpy(cfg.token, tok.data(), 16);
  } catch (const std::exception& e) {
    if (err) *err = std::string("join json: ") + e.what();
    return false;
  }
  return true;
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& b) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  if (!b.empty()) fwrite(b.data(), 1, b.size(), f);
  fclose(f);
  return true;
}

std::string jstr(const json& o, const char* k) {
  auto it = o.find(k);
  if (it == o.end()) return "?";
  if (it->is_string()) return it->get<std::string>();
  return it->dump();
}

std::string hex64(uint64_t v) {
  char b[32];
  snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
  return b;
}

} // namespace

int main(int argc, char** argv) {
  g_start = ms();

  int place = 1818;
  int seconds = 120;
  std::string cookie_file;
  std::string join_json;
  std::string save_join;
  std::string job;
  std::string alpn = "rbx-rtcio";
  std::string tx_dump;
  std::string dump_msgs;
  std::string cap_dir = "py/captures";
  std::string dummy_mode = "skip";
  bool corrupt = false;
  bool routes = true;
  SessionOptions sopt;

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        fprintf(stderr, "%s needs a value\n", what);
        exit(2);
      }
      return argv[++i];
    };
    if (a == "--place") place = atoi(next("--place").c_str());
    else if (a == "--seconds") seconds = atoi(next("--seconds").c_str());
    else if (a == "--cookie-file") cookie_file = next("--cookie-file");
    else if (a == "--join-json") join_json = next("--join-json");
    else if (a == "--save-join") save_join = next("--save-join");
    else if (a == "--job") job = next("--job");
    else if (a == "--alpn") alpn = next("--alpn");
    else if (a == "--tx-dump") tx_dump = next("--tx-dump");
    else if (a == "--dump-msgs") dump_msgs = next("--dump-msgs");
    else if (a == "--cap-dir") cap_dir = next("--cap-dir");
    else if (a == "--dummy") dummy_mode = next("--dummy");
    else if (a == "--corrupt") corrupt = true;
    else if (a == "--no-routes") routes = false;
    else if (a == "--a7") sopt.a7_mode = next("--a7");
    else {
      usage();
      return 2;
    }
  }

  sopt.cap_dir = cap_dir;
  sopt.routes = routes;
  sopt.corrupt = corrupt;

  // ---- 1. join (live or saved) ------------------------------------------
  rbx::JoinResult jr;
  std::string err;
  if (!join_json.empty()) {
    if (!rbx::loadJoin(join_json, jr, &err)) {
      fprintf(stderr, "%s\n", err.c_str());
      return 2;
    }
    logLine("join: loaded " + join_json);
  } else {
    if (cookie_file.empty()) {
      if (const char* e = getenv("RBX_COOKIE_FILE")) cookie_file = e;
    }
    if (cookie_file.empty()) {
      fprintf(stderr, "need --join-json or --cookie-file / $RBX_COOKIE_FILE\n");
      return 2;
    }
    std::string cookie = rbx::loadCookieFile(cookie_file, &err);
    if (cookie.empty()) {
      fprintf(stderr, "%s\n", err.c_str());
      return 2;
    }
    logLine("join: place=" + std::to_string(place) + " ...");
    if (!rbx::joinGame(place, cookie, job, 0, jr, &err)) {
      fprintf(stderr, "JOIN FAILED: %s\n", err.c_str());
      return 1;
    }
    logLine("join: OK job=" + jstr(jr.join_script, "GameId"));
  }
  if (!save_join.empty()) {
    if (!rbx::saveJoin(save_join, jr, &err)) {
      fprintf(stderr, "%s\n", err.c_str());
      return 2;
    }
    logLine("join: saved -> " + save_join);
  }

  const json& js = jr.join_script;

  // ---- 2. offline message dump (parity tests vs probe5/probe7) ----------
  if (!dump_msgs.empty()) {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> msgs;
    std::string ct;
    if (auto it = js.find("ClientTicket"); it != js.end() && it->is_string())
      ct = it->get<std::string>();
    int version = 0;
    if (!ct.empty()) {
      size_t s = ct.rfind(';');
      if (s != std::string::npos) {
        try {
          version = std::stoi(ct.substr(s + 1));
        } catch (...) {
        }
      }
    }
    msgs.push_back({"earlyauth.bin", rbx::msgs::earlyAuth(ct, version)});
    msgs.push_back({"m8a.bin", rbx::msgs::build8A(js)});
    msgs.push_back({"m92.bin", rbx::msgs::build92()});
    std::string e90;
    auto m90 = rbx::msgs::build90(js, jr.reply, cap_dir, &e90);
    if (!e90.empty()) fprintf(stderr, "build90: %s\n", e90.c_str());
    else msgs.push_back({"m90.bin", m90});
    for (auto& kv : msgs) {
      std::string p = dump_msgs + "/" + kv.first;
      printf("%s: %zuB\n", p.c_str(), kv.second.size());
      if (!writeFile(p, kv.second)) {
        fprintf(stderr, "cannot write %s\n", p.c_str());
        return 2;
      }
    }
    return 0;
  }

  // ---- 3. transport config ---------------------------------------------
  QuicClient::Config cfg;
  if (!configFromScript(js, alpn, cfg, &err)) {
    fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }
  logLine("== join: place=" + jstr(js, "PlaceId") + " job=" + jstr(js, "GameId") +
          " udp=" + cfg.udmux_ip + ":" + std::to_string(cfg.port) +
          " rcc=" + cfg.rcc_ip + " alpn=" + cfg.alpn +
          " caps=" + hex64(cfg.local_caps) + " ==");

  // ---- 4. main connection + session -------------------------------------
  QuicClient qc;
  if (!tx_dump.empty()) qc.set_tx_dump_dir(tx_dump);
  Session sess(qc, js, jr.reply, sopt, logLine);
  qc.on_handshake_complete = [&] { sess.onConnected(); };
  qc.on_stream_data = [&](int64_t sid, const uint8_t* d, size_t n, bool fin) {
    sess.onStream(sid, d, n, fin);
  };
  qc.on_stream_reset = [&](int64_t sid, uint64_t code) { sess.onReset(sid, code); };
  qc.on_stop_sending = [&](int64_t sid, uint64_t code) { sess.onStopped(sid, code); };

  if (!qc.init(cfg)) {
    logLine("INIT FAILED: " + qc.last_error());
    return 1;
  }
  bool ok = qc.run_until_handshake(10500);
  logLine(std::string("handshake=") + (ok ? "OK " : "FAIL ") +
          "alpn=" + qc.alpn_selected() + " cipher=" + qc.cipher_name() +
          " peer_caps=" + hex64(qc.peer_caps()) +
          " present=" + std::to_string(int(qc.peer_caps_present())) +
          (ok ? "" : (" err=" + qc.last_error())));
  if (!ok) return 1;

  // ---- 5. optional dummy (app=6, same RUPP token) ------------------------
  bool want_dummy = dummy_mode == "full";
  QuicClient dqc;
  std::unique_ptr<DummySession> dsess;
  if (want_dummy) {
    if (!dqc.init(cfg)) {
      logLine("DUMMY INIT FAILED: " + dqc.last_error());
      want_dummy = false;
    } else {
      dsess = std::make_unique<DummySession>(dqc, sopt, logLine);
      dqc.on_handshake_complete = [&] { dsess->onConnected(); };
      dqc.on_stream_data = [&](int64_t sid, const uint8_t* d, size_t n, bool fin) {
        dsess->onStream(sid, d, n, fin);
      };
      dqc.run_until_handshake(10500);
      logLine(std::string("dummy handshake=") +
              (dqc.hs_complete() ? "OK" : "FAIL") +
              (dqc.failed() ? (" err=" + dqc.last_error()) : ""));
    }
  }

  // ---- 6. run ------------------------------------------------------------
  int64_t deadline = ms() + int64_t(seconds) * 1000;
  int tick_ms = want_dummy ? 50 : 250;
  while (ms() < deadline) {
    if (qc.failed()) break;
    qc.pump(tick_ms);
    sess.tick(ms());
    if (want_dummy && !dqc.failed()) {
      dqc.pump(tick_ms);
      dsess->tick(ms());
    }
  }

  // ---- 7. summary (probe10 parity) ---------------------------------------
  char answer[16];
  snprintf(answer, sizeof answer, "%08x", sess.answer);
  logLine("== summary == challenge=" + std::string(sess.challenge_seen ? "yes" : "no") +
          " answered=" + (sess.answered ? "yes" : "no") +
          " peer=" + (sess.peer_assigned ? "yes" : "no") +
          " resets=" + std::to_string(sess.resets.size()) +
          " chan1_rx=" + std::to_string(sess.chan1_rx.size()) +
          " dummy_rx=" + std::to_string(dsess ? dsess->rx : 0));
  logLine(std::string("== solved=") + (sess.solved ? "yes" : "no") +
          (sess.solve_failed ? " (SOLVE FAILED)" : "") +
          " answer=0x" + std::string(answer) +
          " solve_ms=" + std::to_string(sess.solve_ms) +
          " tx=" + std::to_string(qc.tx_packets()) +
          " rx=" + std::to_string(qc.rx_packets()) +
          " dropped=" + std::to_string(qc.rupp_dropped()) +
          " err=" + qc.last_error());
  for (auto& kv : qc.streams()) {
    logLine("  stream " + std::to_string(kv.first) + ": rx=" +
            std::to_string(kv.second.buf.size()) +
            " fin=" + std::to_string(int(kv.second.fin)) +
            " reset=" + std::to_string(int(kv.second.reset)) +
            " code=" + std::to_string(kv.second.reset_code));
  }
  return (qc.failed() || sess.solve_failed) ? 1 : 0;
}
