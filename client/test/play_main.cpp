// rbxplay — fully local 0x9B solver client (C++ port of py/probe10.py).
//
//   1. join (gamejoin v2 SSE / v1 fallback) or load a saved join
//   2. QUIC connect (native-parity fingerprint: rbx-rtcio, caps ext, RUPP)
//   3. session burst: ctrl hello + openU c3/c5/c6/c11, early-auth, A7,
//      0x90, 0x92, 0x8A, 0x8F on chan1
//   4. detect [9b] challenge on chan1 -> solveMessage() in-process ->
//      send [9b][u2][answer] -> +60ms route declarations on chan11
//   5. keep pumping until interrupted: success = post-answer traffic keeps
//      flowing (peer assignment / replication) instead of a stream reset
//
// Usage (superbuild binary):
//   ./build/client/rbxplay --place-id 1818 --roblosecurity "$(cat run/cookie2.txt)"
//   timeout 45s ./build/client/rbxplay --place-id 1818 --roblosecurity "$(cat run/cookie2.txt)"
//   ./build/client/rbxplay --join-json run/join_full.json   # offline only
//
// Deliberately minimal surface: anything the shell does better stays out.
// A live join needs --place-id (--job-id optionally pins one running
// instance; no silent default place); duration comes from `timeout(1)`
// (or Ctrl-C), the cookie value is passed directly (callers with a file
// use "$(cat file)"), and maintainer-only knobs (message/tx dumps,
// save-join, corrupt-answer control, route toggle, replication capture)
// live behind RBX_* env vars so they never burden the live CLI.
//   RBX_ROBLOSECURITY  cookie fallback (same value as --roblosecurity)
//   RBX_CAP_DIR        capture templates (default: resolved py/captures)
//   RBX_SAVE_JOIN      write {"joinScript","reply"} to this path after join
//   RBX_DUMP_MSGS      write earlyauth/m8a/m92/m90 to this dir and exit
//                      (regenerates the tracked run/cpp_msgs fixtures)
//   RBX_TX_DUMP        dump every outgoing QUIC datagram as <dir>/txNNN.bin
//   RBX_REPL_DUMP      write the full chan1 byte stream to this path at exit
//                      (frozen captures for replication RE; gitignored)
//   RBX_CORRUPT=1      flip answer bits before sending (rejection control)
//   RBX_NO_ROUTES=1    skip chan11 route declarations after the answer
//   RBX_INPUT_PROBE=X  comma-separated upstream sends, first ~30s after
//                      routes then one per 25s (send-path safety ladder,
//                      see send.h; alt account first): heartbeat = A6
//                      count=0, replayN = native post-answer bytes N,
//                      freshH1/H2 = R1 shape with continued stepIds under
//                      the H1 ([4:12]) vs H2 ([5:13]) split hypothesis,
//                      cb0..cb7 = R1 with a bool ControlState slot set,
//                      c10..c17 = same with value 1. First item "hold"
//                      cycles the list every 10s (sustained input).
//                      m96/m99 = handshake messages on chan 1.
//   RBX_TOS=AF41       carrier knob: mark game UDP with that TOS byte
//                      (AF41 = 136 = 0x88; also decimal/hex/DSCP names).
//                      Default unset = TOS 0 (native game parity — the
//                      native game transport never sets TOS; only its
//                      WebRTC voice path does, DSCP<<2|ECN).
//   RBX_MAX_UDP=N      carrier knob: cap QUIC datagrams, [1200,1452]
//                      (default 1452 = native). The 1200 Initial floor
//                      always holds (RFC 9000 s14.1); small-MTU egress
//                      relies on kernel IP fragmentation (DF=0).
#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>


#include "join.h"
#include "live_runtime.h"
#include "view.h"
#include "quic_client.h"
#include "session.h"
#include "util.h"
#include "replicate.h"
#include "joindata.h"
#include "view.h"
#include "instance/class_registry.h"
#include "instance/instance.h"

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
          "usage: rbxplay --place-id N --roblosecurity COOKIE [--job-id UUID]\n"
          "               [--game-id UNIVERSE] [--join-json F] [--headless|--headful] [--shot PATH]\n"
          "--place-id is always required (the server answers job-only joins\n"
          "with status 12 \"Cannot join game without placeId\"); --job-id\n"
          "targets one running instance of that place; session behavior is\n"
          "native-fixed (real A7, full dummy traffic): no behavior flags\n"
          "runs until interrupted (use `timeout Ns` to bound a run)\n"
          "Default is headful: a raylib window opens immediately and renders\n"
          "the live Engine world as replication arrives; --headless disables\n"
          "GL and runs the same Engine runtime without a window; --shot PATH\n"
          "(headful only) saves a screenshot once the world is visible, then\n"
          "exits\n");
}

// Capture templates (0x90 rebuild, A7-real) live in py/captures. That dir
// is resolved once, in order: $RBX_CAP_DIR, ./py/captures (repo-root CWD),
// <exe>/../../py/captures (both build layouts: build/client/rbxplay and
// standalone client/build/rbxplay sit two levels below the root).
// Hardcoding ./py/captures broke every run from another CWD (missing
// A7/90 -> instant server-side ERR_DRAINING).
std::string resolve_cap_dir(const char* argv0) {
  if (const char* e = getenv("RBX_CAP_DIR")) return e;
  namespace fs = std::filesystem;
  std::error_code ec;
  if (fs::is_directory("py/captures", ec)) return "py/captures";
  // Portable exe-relative lookup via argv[0] (no /proc/self/exe): both
  // build layouts (build/client/rbxplay, client/build/rbxplay) sit two
  // levels below the repo root.
  if (argv0 && argv0[0]) {
    const fs::path exe = fs::absolute(fs::path(argv0));
    const fs::path p =
        exe.parent_path() / ".." / ".." / "py" / "captures";
    ec.clear();
    if (fs::is_directory(p, ec)) return p.lexically_normal().string();
  }
  return "py/captures"; // last resort: original failure, but loud in the log
}

// The loop below runs until SIGINT/SIGTERM so `timeout` (which sends
// SIGTERM) ends the session cleanly with the summary still printed.
volatile std::sig_atomic_t g_stop = 0;
void on_stop(int) { g_stop = 1; }

// Extract QuicClient::Config from a joinScript (same fields hs1818 uses).
// ALPN is hardcoded: the native game conn uses "rbx-rtcio", always.
bool configFromScript(const json& j, QuicClient::Config& cfg,
                      std::string* err) {
  cfg.alpn = "rbx-rtcio";
  // Explicit per-field errors: some join paths (e.g. v1/join-game-instance)
  // return status 2 with a script missing auth/network fields. Failing
  // here with the field name beats a QUIC session that can never start
  // (no ClientTicket/NetStackTokenValue -> no RUPP TLV, see rupp.h).
  auto need = [&](const char* k) -> bool {
    if (j.contains(k)) return true;
    if (err) *err = std::string("joinScript lacks ") + k;
    return false;
  };
  if (!need("UdmuxEndpoints") || !need("ServerConnections") ||
      !need("NetStackPort") || !need("NetStackTokenValue") ||
      !need("ClientTicket"))
    return false;
  try {
    cfg.udmux_ip = j.at("UdmuxEndpoints").at(0).at("Address").get<std::string>();
    cfg.rcc_ip = j.at("ServerConnections").at(0).at("Address").get<std::string>();
    const json& p = j.at("NetStackPort");
    cfg.port = uint16_t(p.is_number_integer() ? p.get<int>()
                                              : std::stoi(p.get<std::string>()));
    // RUPP TLV2 carries rcc_ip:NetStackPort (native rupp_tx001). The RCC
    // port in ServerConnections is a different number and is NOT the TLV
    // value — sending it makes the edge drop the handshake.
    const json& sc = j.at("ServerConnections").at(0);
    (void)sc;
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

// One live attempt (join + session + run loop). Heap-held so a stalled
// attempt can be torn down and retried with a fresh join (new instance):
// members destroy in reverse order (session before its QuicClient before
// the join reply it references), so reassigning Attempt is always safe.
struct Attempt {
  rbx::JoinResult jr;
  QuicClient::Config cfg;
  std::unique_ptr<QuicClient> qc;
  std::unique_ptr<Session> sess;
  std::unique_ptr<QuicClient> dqc;
  std::unique_ptr<DummySession> dsess;
  std::unique_ptr<rbxclient::LiveRuntime> live;
  bool have_dummy = false;
};

// Stall signature: joined + handshake OK, then the server goes silent
// before any challenge (idle close with only the 1811B join reply +
// refactor received). Our TX is byte-identical in stall vs success runs,
// so this is server-side gating (full/wedged instance), not our bytes:
// re-join (fresh job -> new instance) instead of waiting it out.
bool isStall(const Attempt& a) {
  return a.sess && !a.sess->challenge_seen && a.qc && a.qc->failed();
}

// Interruptible sleep (SIGINT/SIGTERM sets g_stop; timeout(1) bounds runs).
void sleepIntr(int secs) {
  for (int s = 0; s < secs && !g_stop; ++s)
    std::this_thread::sleep_for(std::chrono::seconds(1));
}

} // namespace


int main(int argc, char** argv) {
  g_start = ms();

  long long place = 0;
  bool have_place = false;
  long universe = 0; // --game-id (universeId; some games reject joins without it)
  std::string cookie;
  std::string join_json;
  std::string job;
  bool headless = false; // headful is the default; --headless opts out of GL
  std::string shot;
  SessionOptions sopt; // native-fixed: real A7 + full dummy, no overrides

  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        fprintf(stderr, "%s needs a value\n", what);
        exit(2);
      }
      return argv[++i];
    };
    if (a == "--place-id") {
      place = atoll(next("--place-id").c_str());
      have_place = true;
    }
    else if (a == "--roblosecurity") cookie = next("--roblosecurity");
    else if (a == "--join-json") join_json = next("--join-json");
    else if (a == "--job-id") job = next("--job-id");
    else if (a == "--game-id") universe = atol(next("--game-id").c_str());
    else if (a == "--headful") headless = false;
    else if (a == "--headless") headless = true;
    else if (a == "--shot") shot = next("--shot");
    else {
      usage();
      return 2;
    }
  }
  // Offline replication replay (maintainer knob): apply a captured chan1
  // stream through the exact live path, then exit. No join/QUIC/GL.
  if (const char* replay = getenv("RBX_REPLAY")) {
    FILE* f = fopen(replay, "rb");
    if (!f) {
      fprintf(stderr, "cannot open $RBX_REPLAY %s\n", replay);
      return 2;
    }
    std::vector<uint8_t> bytes;
    uint8_t buf[65536];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
      bytes.insert(bytes.end(), buf, buf + n);
    fclose(f);
    // The capture has no join script, so identity comes from the caller when
    // it wants LocalPlayer on a replay. 0 leaves Players.LocalPlayer nil,
    // which is the honest offline result.
    if (const char* e = getenv("RBX_USER_ID")) {
      rbx::set_local_user_id(std::strtoll(e, nullptr, 10));
      logLine(std::string("replay user id=") + e);
    }
    rbxclient::LiveRuntime live("replay",
                                [](const std::string& s) { logLine("live: " + s); });
    live.feed(bytes.data(), bytes.size());
    for (int i = 0; i < 10; ++i) live.pump(1.0 / 60.0);
    logLine("== replay == bytes=" + std::to_string(bytes.size()) +
            " world=" + std::string(live.has_world() ? "yes" : "no") +
            " records=" + std::to_string(live.snapshot_records()) +
            " msgs=" + std::to_string(live.live_messages()) +
            " scripts=" + std::to_string(live.scripts_run()) +
            " script_fail=" + std::to_string(live.scripts_failed()) +
            " " + live.stats() + " classes=" + live.class_stats() +
            (live.failed() ? " error=" + live.error() : ""));
    if (!shot.empty()) {
      // Offline renderer review: the same collectBoxes/view path as live,
      // but with no network. Fails cleanly without a display.
      if (!live.has_world()) {
        fprintf(stderr, "--shot replay: no world applied\n");
        return 2;
      }
      const auto boxes = rbxclient::collectBoxes(live.root());
      const rbxclient::CameraFocus f = rbxclient::focusCamera(boxes);
      logLine("replay view: boxes=" + std::to_string(boxes.size()) +
              " focus=(" + std::to_string(f.cx) + "," +
              std::to_string(f.cy) + "," + std::to_string(f.cz) +
              ") r=" + std::to_string(f.radius));
      std::string verr;
      const int vrc = rbxclient::runViewer(boxes, shot, &verr);
      if (vrc != 0) {
        fprintf(stderr, "replay view: %s\n", verr.c_str());
        return vrc;
      }
    }
    return live.failed() ? 1 : 0;
  }

  // No silent default place. The server rejects job-only joins outright
  // (status 12 "Cannot join game without placeId"), so --place-id is
  // mandatory; --job-id only selects one running instance of that place.
  if (join_json.empty() && !have_place) {
    fprintf(stderr, "need --place-id (and --roblosecurity) or --join-json\n");
    usage();
    return 2;
  }
  if (!shot.empty() && headless) {
    fprintf(stderr, "--shot needs --headful (omit --headless)\n");
    usage();
    return 2;
  }

  // Maintainer-only knobs (never flags): message/tx dumps regenerate the
  // tracked run/cpp_msgs + run/cpp_tx fixtures; corrupt/no-routes are the
  // rejection-control test. The 0x90/A7 templates always come from
  // py/captures (same as the unit tests).
  sopt.cap_dir = resolve_cap_dir(argv[0]);
  if (const char* e = getenv("RBX_CORRUPT")) sopt.corrupt = std::string(e) == "1";
  if (const char* e = getenv("RBX_NO_ROUTES")) sopt.routes = std::string(e) != "1";
  std::string save_join;
  if (const char* e = getenv("RBX_SAVE_JOIN")) save_join = e;
  std::string dump_msgs;
  if (const char* e = getenv("RBX_DUMP_MSGS")) dump_msgs = e;
  std::string tx_dump;
  if (const char* e = getenv("RBX_TX_DUMP")) tx_dump = e;
  std::string repl_dump;
  if (const char* e = getenv("RBX_REPL_DUMP")) repl_dump = e;
  if (const char* e = getenv("RBX_INPUT_PROBE")) sopt.input_probe = e;
  // Bounded stall retries: re-join (fresh job -> new instance) when the
  // server goes silent before any challenge. Default 3 (up to 4 joins).
  int join_retries = 3;
  if (const char* e = getenv("RBX_JOIN_RETRIES")) {
    try {
      join_retries = std::stoi(e);
    } catch (...) {
    }
    if (join_retries < 0)
      join_retries = 0;
    if (join_retries > 10)
      join_retries = 10;
  }

  // ---- 1. join (live or saved) ------------------------------------------
  const bool live = join_json.empty();
  if (live && cookie.empty()) {
    if (const char* e = getenv("RBX_ROBLOSECURITY")) cookie = e;
  }
  if (live && cookie.empty()) {
    fprintf(stderr, "need --join-json or --roblosecurity / $RBX_ROBLOSECURITY\n");
    return 2;
  }
  // Live attempts (join -> session -> run). A stalled attempt (server
  // silent before any challenge) retries with a FRESH join (new job ->
  // new instance); anything else (offline, interrupt, success, join/config
  // failure) runs exactly once. Saved joins never retry (no re-join).
  Attempt cur;
  int attempt = 0;
  std::signal(SIGINT, on_stop);
  std::signal(SIGTERM, on_stop);
  for (;; ++attempt) {
    cur = Attempt{};
    // ---- 1. join (live or saved) ------------------------------------------
    std::string err;
    if (!live) {
      if (attempt > 0)
        break;
      if (!rbx::loadJoin(join_json, cur.jr, &err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 2;
      }
      logLine("join: loaded " + join_json);
    } else {
      logLine("join: place=" + std::to_string(place) +
              (attempt > 0
                   ? " (attempt " + std::to_string(attempt + 1) + " of " +
                         std::to_string(join_retries + 1) + ")"
                   : "") +
              " ...");
      if (!rbx::joinGame(place, cookie, job, 0, universe, cur.jr, &err)) {
        fprintf(stderr, "JOIN FAILED: %s\n", err.c_str());
        return 1;
      }
      logLine("join: OK job=" + jstr(cur.jr.join_script, "GameId"));
    }
    if (!save_join.empty()) {
      if (!rbx::saveJoin(save_join, cur.jr, &err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 2;
      }
      logLine("join: saved -> " + save_join);
    }

    const json& js = cur.jr.join_script;

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
    auto m90 = rbx::msgs::build90(js, cur.jr.reply, sopt.cap_dir, &e90);
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
  if (!configFromScript(js, cur.cfg, &err)) {
    fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }
  // Carrier knobs (transport layer only; session behavior stays
  // native-fixed). Defaults = native parity (TOS 0, 1452B cap).
  //   RBX_TOS=AF41 (or 136 / 0x88): mark game UDP with that TOS byte.
  //   RBX_MAX_UDP=N: cap QUIC datagrams (clamped to [1200,1452]; the
  //     1200 Initial floor always holds — sub-1200 Initials are
  //     blackholed — with kernel IP fragmentation covering small-MTU
  //     egress, since our sockets leave DF=0).
  if (const char* e = getenv("RBX_TOS")) {
    int tos = QuicClient::parseTos(e);
    if (tos == -2 || tos < 0 || tos > 255) {
      fprintf(stderr, "bad $RBX_TOS %s (want 0-255, 0x.., or AF41/EF/CSn)\n",
              e);
      return 2;
    }
    cur.cfg.tos = tos;
  }
  // Destination/RUPP-port override (transport RE knob). The join carries
  // both the Udmux port and NetStackPort; native RbxTransport uses
  // NetStackPort for both, but a deployment can expose game app=4 only on
  // the Udmux port, so make them independently testable.
  if (const char* e = getenv("RBX_DEST_PORT")) {
    try {
      cur.cfg.port = uint16_t(std::stoi(e));
    } catch (...) {
      fprintf(stderr, "bad $RBX_DEST_PORT %s\n", e);
      return 2;
    }
  }
  if (const char* e = getenv("RBX_RUPP_PORT")) {
    try {
      cur.cfg.rcc_port = uint16_t(std::stoi(e));
    } catch (...) {
      fprintf(stderr, "bad $RBX_RUPP_PORT %s\n", e);
      return 2;
    }
  }
  if (const char* e = getenv("RBX_MAX_UDP")) {
    try {
      cur.cfg.max_tx_payload = std::stoi(e);
    } catch (...) {
      fprintf(stderr, "bad $RBX_MAX_UDP %s (want 1200-1452)\n", e);
      return 2;
    }
  }
  logLine("== join: place=" + jstr(js, "PlaceId") + " job=" + jstr(js, "GameId") +
          " udp=" + cur.cfg.udmux_ip + ":" + std::to_string(cur.cfg.port) +
          " rcc=" + cur.cfg.rcc_ip + ":" +
          std::to_string(cur.cfg.rcc_port ? cur.cfg.rcc_port : cur.cfg.port) +
          " alpn=" + cur.cfg.alpn +
          " caps=" + hex64(cur.cfg.local_caps) + " ==");

  // ---- 4. main connection + session -------------------------------------
  cur.qc = std::make_unique<QuicClient>();
  if (!tx_dump.empty()) cur.qc->set_tx_dump_dir(tx_dump);
  cur.sess = std::make_unique<Session>(*cur.qc, js, cur.jr.reply, sopt, logLine);
  // LocalPlayer is keyed by the authenticated account, exactly like the
  // native client. Install it before any snapshot applies so Players API
  // lookups work from the first tick.
  const int64_t self_id = js.value("UserId", int64_t(0));
  rbx::set_local_user_id(self_id);
  if (self_id > 0)
    logLine("local user id=" + std::to_string(self_id));
  cur.live = std::make_unique<rbxclient::LiveRuntime>(
      jstr(js, "GameId"), [](const std::string& s) { logLine("live: " + s); });
  cur.sess->on_chan1_data = [&cur](const uint8_t* d, size_t n) {
    if (cur.live) cur.live->feed(d, n);
  };
  cur.qc->on_handshake_complete = [&] { cur.sess->onConnected(); };
  cur.qc->on_stream_data = [&](int64_t sid, const uint8_t* d, size_t n, bool fin) {
    cur.sess->onStream(sid, d, n, fin);
  };
  cur.qc->on_stream_reset = [&](int64_t sid, uint64_t code) { cur.sess->onReset(sid, code); };
  cur.qc->on_stop_sending = [&](int64_t sid, uint64_t code) { cur.sess->onStopped(sid, code); };

  if (!cur.qc->init(cur.cfg)) {
    logLine("INIT FAILED: " + cur.qc->last_error());
    return 1;
  }
  bool ok = cur.qc->run_until_handshake(10500);
  logLine(std::string("handshake=") + (ok ? "OK " : "FAIL ") +
          "alpn=" + cur.qc->alpn_selected() + " cipher=" + cur.qc->cipher_name() +
          " peer_caps=" + hex64(cur.qc->peer_caps()) +
          " present=" + std::to_string(int(cur.qc->peer_caps_present())) +
          (ok ? "" : (" err=" + cur.qc->last_error())));
  if (!ok) {
    // Handshake never completed: same stall family (server silent during
    // setup) — retry like a pre-challenge stall when attempts remain.
    if (live && !g_stop && attempt < join_retries) {
      logLine("stall: handshake failed (" + cur.qc->last_error() +
              "); re-joining in 5s (attempt " + std::to_string(attempt + 2) +
              " of " + std::to_string(join_retries + 1) + ")" +
              (job.empty() ? "" : " [pinned to --job-id " + job + "]"));
      sleepIntr(5);
      if (g_stop)
        break;
      continue;
    }
    return 1;
  }

  // ---- 5. dummy (app=6, same RUPP token) --------------------------------
  // Native always opens it immediately after joining: no flag, no skip.
  // RBX_NO_DUMMY=1 is a transport diagnostic only: the dummy uses a second
  // UDP 4-tuple for the same RUPP token, and some edges route one of the two
  // connections badly while the handshake still succeeds.
  const bool no_dummy = getenv("RBX_NO_DUMMY") &&
                        std::string(getenv("RBX_NO_DUMMY")) == "1";
  cur.dqc = std::make_unique<QuicClient>();
  cur.have_dummy = !no_dummy;
  if (no_dummy) {
    logLine("dummy skipped (RBX_NO_DUMMY=1)");
  } else if (!cur.dqc->init(cur.cfg)) {
    logLine("DUMMY INIT FAILED: " + cur.dqc->last_error());
    cur.have_dummy = false;
  } else {
    cur.dsess = std::make_unique<DummySession>(*cur.dqc, logLine);
    cur.dqc->on_handshake_complete = [&] { cur.dsess->onConnected(); };
    cur.dqc->on_stream_data = [&](int64_t sid, const uint8_t* d, size_t n, bool fin) {
      cur.dsess->onStream(sid, d, n, fin);
    };
    cur.dqc->run_until_handshake(10500);
    logLine(std::string("dummy handshake=") +
            (cur.dqc->hs_complete() ? "OK" : "FAIL") +
            (cur.dqc->failed() ? (" err=" + cur.dqc->last_error()) : ""));
  }

  // ---- 6. run until the window closes / interrupted --------------------
  // The live Engine world is the same in both modes; only presentation
  // differs. Headful pumps transport + scheduler from the render loop, so
  // the window is useful before the first snapshot (waiting state) and
  // updates as 0x83 deltas arrive. --headless is the explicit no-GL path.
  if (headless) {
    int64_t last_pump = ms();
    while (!g_stop) {
      if (cur.qc->failed()) break;
      cur.qc->pump(50);
      cur.sess->tick(ms());
      if (cur.have_dummy && !cur.dqc->failed()) {
        cur.dqc->pump(50);
        cur.dsess->tick(ms());
      }
      const int64_t now = ms();
      if (cur.live) cur.live->pump(double(now - last_pump) / 1000.0);
      last_pump = now;
    }
  } else {
    auto boxes = [&]() -> std::vector<rbxclient::ViewBox> {
      if (cur.live && cur.live->has_world())
        return rbxclient::collectBoxes(cur.live->root());
      return {};
    };
    auto tick = [&](double dt) -> bool {
      if (g_stop || cur.qc->failed()) return false;
      cur.qc->pump(0);
      cur.sess->tick(ms());
      if (cur.have_dummy && !cur.dqc->failed()) {
        cur.dqc->pump(0);
        cur.dsess->tick(ms());
      }
      if (cur.live) cur.live->pump(dt);
      return !g_stop && !cur.qc->failed();
    };
    std::string verr;
    const int vrc = rbxclient::runViewerLive(boxes, tick, shot, &verr);
    if (vrc != 0) {
      logLine("view: " + verr);
      return vrc;
    }
    g_stop = 1;
  }

  // ---- 6b. stall retry (live only) --------------------------------------
  // Pre-challenge server silence with a failed transport (and no user
  // interrupt) means the instance never set the session up: re-join
  // fresh (new job -> new instance) a bounded number of times.
  if (live && isStall(cur) && !g_stop && attempt < join_retries &&
      getenv("RBX_CORRUPT") == nullptr && getenv("RBX_NO_ROUTES") == nullptr) {
    const int wait_s = 5 * (attempt + 1);
    logLine("stall: no challenge before " + cur.qc->last_error() +
            " (job=" + jstr(cur.jr.join_script, "GameId") + " udp=" +
            cur.cfg.udmux_ip + ":" + std::to_string(cur.cfg.port) +
            "); re-joining in " + std::to_string(wait_s) + "s (attempt " +
            std::to_string(attempt + 2) + " of " +
            std::to_string(join_retries + 1) + ")" +
            (job.empty() ? "" : " [pinned to --job-id " + job + "]"));
    sleepIntr(wait_s);
    if (g_stop)
      break;
    continue;
  }
  break;
  } // end live attempts

  // ---- 7. summary (probe10 parity) ---------------------------------------
  char answer[16];
  snprintf(answer, sizeof answer, "%08x", cur.sess->answer);
  logLine("== summary == challenge=" + std::string(cur.sess->challenge_seen ? "yes" : "no") +
          " answered=" + (cur.sess->answered ? "yes" : "no") +
          " peer=" + (cur.sess->peer_assigned ? "yes" : "no") +
          " resets=" + std::to_string(cur.sess->resets.size()) +
          " chan1_rx=" + std::to_string(cur.sess->chan1_rx.size()) +
          " dummy_rx=" + std::to_string(cur.dsess ? cur.dsess->rx : 0));
  if (cur.live) {
    logLine("== live == world=" +
            std::string(cur.live->has_world() ? "yes" : "no") +
            " records=" + std::to_string(cur.live->snapshot_records()) +
            " msgs=" + std::to_string(cur.live->live_messages()) +
            " scripts=" + std::to_string(cur.live->scripts_run()) +
            " script_fail=" + std::to_string(cur.live->scripts_failed()) +
            " " + cur.live->stats() + " classes=" + cur.live->class_stats() +
            (cur.live->failed() ? " error=" + cur.live->error() : ""));
  }
  logLine(std::string("== solved=") + (cur.sess->solved ? "yes" : "no") +
          (cur.sess->solve_failed ? " (SOLVE FAILED)" : "") +
          " answer=0x" + std::string(answer) +
          " solve_ms=" + std::to_string(cur.sess->solve_ms) +
          " tx=" + std::to_string(cur.qc->tx_packets()) +
          " rx=" + std::to_string(cur.qc->rx_packets()) +
          " sent=" + std::to_string(cur.qc->sent_bytes()) +
          " acked=" + std::to_string(cur.qc->acked_bytes()) +
          " inflight=" + std::to_string(cur.qc->bytes_in_flight()) +
          " rxwin=" + std::to_string(cur.qc->max_data_left()) +
          " s15win=" + std::to_string(cur.qc->max_stream_data_left(15)) +
          " dropped=" + std::to_string(cur.qc->rupp_dropped()) +
          " err=" + cur.qc->last_error());
  for (auto& kv : cur.qc->streams()) {
    logLine("  stream " + std::to_string(kv.first) + ": rx=" +
            std::to_string(kv.second.buf.size()) +
            " fin=" + std::to_string(int(kv.second.fin)) +
            " reset=" + std::to_string(int(kv.second.reset)) +
            " code=" + std::to_string(kv.second.reset_code));
  }
  // Replication capture: the full chan1 byte stream (varint-framed 0x93,
  // 0x9B, snapshot + deltas) for offline RE. Pinned by size against the
  // chan1_rx the summary already prints.
  if (!repl_dump.empty()) {
    if (writeFile(repl_dump, cur.sess->chan1_rx))
      logLine("repl: wrote " + std::to_string(cur.sess->chan1_rx.size()) +
              "B chan1 -> " + repl_dump);
    else
      logLine("repl: CANNOT WRITE " + repl_dump);
  }
  return (cur.qc->failed() || cur.sess->solve_failed) ? 1 : 0;
}
