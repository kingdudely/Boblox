// session.h — RbxTransport game session over the C++ QUIC transport.
// Byte-level port of py/probe10.py (P10 + Dummy) message flow:
//
//   on handshake:
//     ctrl stream (app=4, chan="ctrl") : 00 00 hello,
//                                        OpenUnreliable c3/c5/c6/c11
//     chan1 stream (app=4, chan=1)     : early-auth, A7, 0x90, 0x92, 0x8A,
//                                        0x8F
//   on chan1 rx:
//     frame-walk -> [9b][u1][u2][len][blob] challenge -> solveMessage()
//     (in-process) -> [9b][u2][answer] -> chan1; +60ms route declarations
//     on chan11 (native sessioncap s0019-s0022)
//
// Success signal: post-answer traffic keeps flowing on chan1 (peer
// assignment / replication) instead of a 0x10000000106 stream reset.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "quic_client.h"

namespace rbx {

constexpr uint32_t APP = 4;
constexpr uint64_t CTRL = 0x6374726C; // "ctrl"

struct SessionOptions {
  std::string a7_name;           // default msg_0006_a4_c1.bin; env RBX_A7 wins
  bool routes = true;            // chan11 route declarations after the answer
  bool corrupt = false;          // control: flip answer bits before sending
  // Input probe (send-path safety ladder, see send.h): "" = off,
  // "heartbeat" = A6 count=0 once ~30s after routes, "replay3" = native
  // post-answer bytes verbatim. Set from $RBX_INPUT_PROBE (alt account
  // first). Empty by default: zero behavior change.
  std::string input_probe;
  std::string cap_dir = "py/captures";
  // received 0x9B challenges are saved here for debugging. Must NOT point at
  // a tracked fixture (run/oracle_challenge.bin feeds tests/vectors) — the
  // default lives under run/, which is ignored by default.
  std::string chal_path = "run/challenge_live.bin";
};

class Session {
public:
  using LogFn = std::function<void(const std::string&)>;

  Session(QuicClient& qc, const nlohmann::json& js, const nlohmann::json& reply,
          SessionOptions opt, LogFn log);

  void onConnected();
  void onStream(int64_t sid, const uint8_t* p, size_t n, bool fin);
  void onReset(int64_t sid, uint64_t code);
  void onStopped(int64_t sid, uint64_t code);
  void tick(int64_t now_ms); // delayed route declarations

  // probe10 summary parity
  bool challenge_seen = false;
  bool answered = false;
  bool solved = false;
  bool solve_failed = false;
  bool peer_assigned = false;
  bool marker_seen = false;  // server 0x84 snapshot-done marker received
  uint32_t marker_id = 0;    // its id (echoed back in our 0x04 ack)
  int marker_acks = 0;       // 0x04 acks sent
  uint32_t answer = 0;
  int64_t solve_ms = 0;
  std::vector<uint8_t> chan1_rx;
  std::vector<int64_t> resets;

private:
  struct SInfo {
    uint32_t app = 0;
    uint64_t chan = 0;
  };
  int64_t openStream(uint32_t app, uint64_t chan, const char* label);
  void sendFramed(int64_t sid, const std::vector<uint8_t>& payload,
                  const char* label);
  void processPayload(const SInfo& si, const uint8_t* p, size_t n);
  void parseCtrl(const uint8_t* p, size_t n);
  void tryChallenge();
  void solveNow(const uint8_t* msg, size_t mlen);
  void sendAnswer(uint32_t sent);
  void sendRoutes();
  // Scan chan1_rx for newly completed 0x84 frames; ack each with 0x04.
  void tryMarker();
  int64_t ensureChan1();

  // Gameplay upstream: opens a stream + sends one framed payload
  // (send-path probes; see send.h). chan defaults to 11 (input); pass 1
  // for chan-1 handshake messages (0x96/0x99). Logs via log_.
  void sendInput(const std::vector<uint8_t>& payload, const char* label,
                 uint64_t chan = 11);
  // One named probe send (RBX_INPUT_PROBE item grammar: heartbeat,
  // replayN, freshH1/H2, cb0..cb7).
  void fireProbe(const std::string& name);

  QuicClient& qc_;
  nlohmann::json js_;
  nlohmann::json reply_;
  SessionOptions opt_;
  LogFn log_;
  std::map<int64_t, SInfo> info_;
  std::map<int64_t, std::vector<uint8_t>> hdr_buf_;
  int64_t ctrl_sid_ = -1;
  int64_t chan1_sid_ = -1;
  // chan1_rx bytes already scanned for 0x84 markers (tryMarker).
  size_t marker_off_ = 0;
  int64_t routes_sid_ = -1; // chan-11 routes stream (sendRoutes); probes
                            // prefer it so gameplay upstream rides ONE
                            // chan-11 stream, like the native framer queue.
  int64_t input_sid_ = -1;  // fallback when routes are disabled (see above)
  int64_t routes_at_ms_ = -1;
  int64_t input_at_ms_ = -1; // input probe schedule (send.h ladder)
  size_t input_idx_ = 0;     // next probe in the comma list
  uint32_t u1_ = 0;
  uint32_t u2_ = 0;
};

// app=6 DummyClient connection (probe10.Dummy) — the native opens a second
// QUIC conn with the SAME RUPP token right after joining: ctrl hello,
// openU c2/c3, chan1 ping 77B/1s, chan3 sync 25B/5s (dummycap2 order).
class DummySession {
public:
  using LogFn = std::function<void(const std::string&)>;

  DummySession(QuicClient& qc, LogFn log);
  void onConnected();
  void onStream(int64_t sid, const uint8_t* p, size_t n, bool fin);
  void tick(int64_t now_ms);

  uint64_t rx = 0;

private:
  struct SInfo {
    uint32_t app = 0;
    uint64_t chan = 0;
  };
  int64_t openStream(uint32_t app, uint64_t chan);
  void sendFramed(int64_t sid, const std::vector<uint8_t>& payload,
                  const char* label);

  QuicClient& qc_;
  LogFn log_;
  std::map<int64_t, SInfo> info_;
  std::map<int64_t, std::vector<uint8_t>> hdr_buf_;
  int64_t ctrl_ = -1;
  int64_t c1_ = -1;
  int64_t c3_ = -1;
  int64_t next_ping_ms_ = -1;
  int64_t next_sync_ms_ = -1;
};

// Message builders (exported for offline parity tests vs probe5/probe7).
namespace msgs {
std::vector<uint8_t> build8A(const nlohmann::json& js);
// err set when no 0x90 template or reply.joinTicket is missing.
std::vector<uint8_t> build90(const nlohmann::json& js, const nlohmann::json& reply,
                             const std::string& cap_dir, std::string* err);
std::vector<uint8_t> build92();
std::vector<uint8_t> earlyAuth(const std::string& client_ticket, int version);
// 0x96 streaming handshake, idle minimal (32BE27C): [96][00]. Post-connect
// pre-spawn hypothesized; client chan 1. a2≠0 form ([96][01][u32 8/9])
// needs a feature flag we don't have — idle form only.
std::vector<uint8_t> build96();
// 0x99 one-shot caps (32BE4E6): [99][02 00 00 00][u32][u32][u8], zeros
// when no profiler overrides (the only observed config). Client chan 1,
// sent once (native sets +18384 after).
std::vector<uint8_t> build99();
// 0x04 marker ack (reply to the server's 0x84 snapshot-done SendMarker):
// [04][u32 marker LE, echoed]. The server gates join progress (our
// character) on DeserializedMarkerItem::process of this ack.
std::vector<uint8_t> buildMarkerAck(uint32_t marker);
// Native post-answer chan-11 bytes, verbatim (kRoutes sessioncap
// s0019-s0022): idx 0..3, sizes 14/18/22/26B. Replay probe material.
std::vector<uint8_t> routeSample(int idx);
} // namespace msgs

} // namespace rbx
