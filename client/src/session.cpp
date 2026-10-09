#include "session.h"

#include "blob.h"    // rbxclient::findChallengeFrame (the chan1_rx walk)
#include "solve.h"   // rbxclient::solveMessage / buildResponse
#include "util.h"
#include "xxh32.h"   // rbxclient::xxh32

#include <openssl/rand.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace rbx {

namespace {

int64_t nowMs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string hexs(const uint8_t* p, size_t n, size_t max = 32) {
  static const char* d = "0123456789abcdef";
  std::string s;
  size_t m = n < max ? n : max;
  for (size_t i = 0; i < m; i++) {
    s.push_back(d[p[i] >> 4]);
    s.push_back(d[p[i] & 0xf]);
  }
  if (n > max) s += "...";
  return s;
}

std::string hex32(uint32_t v) {
  char b[16];
  snprintf(b, sizeof b, "%08x", v);
  return b;
}

std::vector<uint8_t> hx(const char* s) {
  std::vector<uint8_t> out;
  auto v = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
  };
  for (size_t i = 0; s[i] && s[i + 1]; i += 2)
    out.push_back(uint8_t((v(s[i]) << 4) | v(s[i + 1])));
  return out;
}

int64_t jsonInt(const nlohmann::json& j, const char* k) {
  auto it = j.find(k);
  if (it == j.end()) return 0;
  if (it->is_number_integer()) return it->get<int64_t>();
  if (it->is_string()) {
    try {
      return std::stoll(it->get<std::string>());
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

std::string jsonStr(const nlohmann::json& j, const char* k) {
  auto it = j.find(k);
  if (it == j.end()) return "";
  if (it->is_string()) return it->get<std::string>();
  return it->dump();
}

std::string jsonStr(const nlohmann::json& j, const std::string& k) {
  return jsonStr(j, k.c_str());
}

// ---------------------------------------------------------------------------
// 0x8A builder (probe5.build_8a — validated against native captures)
// ---------------------------------------------------------------------------

constexpr uint32_t S = 0x71375635; // dword_71941A0 runtime value

inline uint32_t rol32(uint32_t x, int r) {
  return (x << r) | (x >> (32 - r));
}

// v31 = obfuscated(xxh32(ClientTicket, seed=1)) — probe5.ticket_v31.
uint32_t ticketV31(const uint8_t* ticket, size_t n) {
  uint32_t v7 = rbxclient::xxh32(ticket, n, 1);
  uint32_t v8 = uint32_t(0u - uint32_t(17506u * S)) & 0xFFFFu;
  uint32_t v9 = (v8 & 2) ? 7 : 25;
  uint32_t v12 = (v8 & 4) ? uint32_t(0u - S) : 1434170839u;
  uint32_t v13 = rol32(v7 + 1434170839u, int(v9)) + v12;
  uint32_t v14 = (v8 & 8) ? S : 1434170839u;
  uint32_t v15 = (v8 & 0x20) ? S : 1434170839u;
  uint32_t v16 = (v8 & 0x40) ? S : 1434170839u;
  uint32_t v17 = (v8 & 0x4000) ? S : 1434170839u;
  uint32_t v18 = v13 * v14;
  uint32_t v19 = (v8 & 0x10) ? 13 : 19;
  uint32_t v20 =
      rol32(v16 ^ (v15 - rol32(v18, int(v19))), int(2 * ((v8 & 0xFF) >> 7) + 15));
  uint32_t v11 = (v8 & 0x100) ? S : uint32_t(0u - S);
  uint32_t v21 = v11 + 1434170839u;
  uint32_t v22 = v11 - 1434170839u;
  if (v8 & 0x200) v22 = v21;
  uint32_t v23 = v20 + v22;
  uint32_t v24 = (v8 & 0x400) ? 23 : 9;
  uint32_t v25 = rol32(v23, int(v24));
  uint32_t v26 = (v8 & 0x800) ? uint32_t(0u - v25) : v25;
  uint32_t v27 = S + v26;
  uint32_t v28 = (v8 & 0x1000) ? uint32_t(0u - v27) : v27;
  uint32_t v29 = v28 + 1434170839u;
  uint32_t v30 = (v8 & 0x2000) ? 29 : 3;
  return v17 ^ rol32(v29, int(v30));
}

// ---------------------------------------------------------------------------
// 0x90 rebuild (probe7.build_90): real native capture template, patched with
// THIS session's RandomSeed1 / APIsecurityToken / __joinTicket.
// ---------------------------------------------------------------------------

const char* kTemplates[] = {"acct2_90.bin", "msg_0007_a4_c1.bin"};

struct Template90 {
  std::string raw;
  nlohmann::ordered_json tmpl;
  bool ok = false;
};

Template90 loadTemplate90(const std::string& cap_dir, int64_t want) {
  std::vector<std::string> names;
  if (const char* env = getenv("RBX_90_TEMPLATE")) names.push_back(env);
  else for (auto* n : kTemplates) names.push_back(n);
  Template90 fb;
  for (auto& name : names) {
    bool ok = false;
    std::string raw = readFileBytes(cap_dir + "/" + name, &ok);
    if (!ok) continue;
    size_t i = raw.find("{\"UserId\"");
    if (i == std::string::npos || raw.size() < 20 || i >= raw.size() - 20)
      continue;
    auto tmpl = nlohmann::ordered_json::parse(
        raw.substr(i, (raw.size() - 20) - i), nullptr, false);
    if (tmpl.is_discarded()) continue;
    if (!fb.ok) {
      fb.raw = raw;
      fb.tmpl = tmpl;
      fb.ok = true;
    }
    if (jsonInt(tmpl, "UserId") == want) {
      Template90 hit;
      hit.raw = raw;
      hit.tmpl = tmpl;
      hit.ok = true;
      return hit;
    }
  }
  return fb; // fallback: first parseable (probe7 behavior)
}

// ---------------------------------------------------------------------------
// native TX templates (dummycap2)
// ---------------------------------------------------------------------------

// chan1 ping 77B / chan3 sync 25B on the app=6 dummy connection
std::vector<uint8_t> pingTmpl() {
  static std::vector<uint8_t> t = [] {
    auto v = hx("0858fbbded5ddc184000000001");
    v.insert(v.end(), 64, 'P');
    return v;
  }();
  return t;
}
std::vector<uint8_t> syncTmpl() {
  static std::vector<uint8_t> t =
      hx("03fa4715010000000000000000000000000000000000000000");
  return t;
}

// native route declarations on chan11, sent ~60ms after the 9B answer
// (sessioncap s0019-s0022)
const char* kRoutes[] = {
    "a60101000114000000140000000000",
    "a60201000118000000180000000000040400",
    "a6030100011c0000001c0000000000040400040400",
    "a60401000120000000200000000000040400040400040400",
};

} // namespace

namespace msgs {

std::vector<uint8_t> build8A(const nlohmann::json& js) {
  int64_t uid = jsonInt(js, "UserId");
  std::string ticket = jsonStr(js, "ClientTicket");
  std::string session = jsonStr(js, "SessionId");
  uint64_t zz = uint64_t(uid) << 1; // (2*uid) ^ (uid>>63), uid >= 0
  uint32_t v31 = ticketV31(reinterpret_cast<const uint8_t*>(ticket.data()),
                           ticket.size());
  std::vector<uint8_t> out;
  out.push_back(0x8A);
  leb128(out, zz);
  leb128(out, ticket.size());
  out.insert(out.end(), ticket.begin(), ticket.end());
  out.push_back(36); // u32le(36)
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  static const char* CONST74 =
      "2e427f51c4dab762fe9e3471c6cfa1650841723b!6e8e47e92778f00efbb13c7bb151ea88.";
  leb128(out, 74);
  out.insert(out.end(), CONST74, CONST74 + 74);
  static const char* ANDROID = "Android";
  leb128(out, 7);
  out.insert(out.end(), ANDROID, ANDROID + 7);
  leb128(out, 1);
  out.push_back('?');
  leb128(out, v31);
  leb128(out, uint64_t(v31 - 0x0BADF00Du)); // uint32 wrap == Python & M
  out.push_back(0x00);
  leb128(out, session.size());
  out.insert(out.end(), session.begin(), session.end());
  out.push_back(0xfe);
  out.push_back(0xca);
  out.push_back(0x01);
  out.push_back(0xc0);
  return out;
}

std::vector<uint8_t> build90(const nlohmann::json& js, const nlohmann::json& reply,
                             const std::string& cap_dir, std::string* err) {
  auto fail = [&](const std::string& e) -> std::vector<uint8_t> {
    if (err) *err = e;
    return {};
  };
  if (!reply.contains("joinTicket") || !reply["joinTicket"].contains("SerializedClientFields"))
    return fail("0x90 needs reply.joinTicket (live join or --save-join file)");
  if (!js.contains("RandomSeed1") || !js.contains("APIsecurityToken"))
    return fail("0x90 needs joinScript.RandomSeed1 / APIsecurityToken");

  Template90 t = loadTemplate90(cap_dir, jsonInt(js, "UserId"));
  if (!t.ok) return fail("no 0x90 template found in " + cap_dir);

  // prefix = everything before the LEB length of the JSON (probe7 walk)
  size_t i = t.raw.find("{\"UserId\"");
  size_t start = i - 1;
  while (start > 0 && (t.raw[start - 1] & 0x80)) start--;
  std::string prefix = t.raw.substr(0, start);
  std::string tail = t.raw.substr(t.raw.size() - 20);

  nlohmann::ordered_json tmpl = t.tmpl;
  tmpl["RandomSeed1"] = js["RandomSeed1"];
  tmpl["APIsecurityToken"] = js["APIsecurityToken"];
  nlohmann::ordered_json ti;
  ti["SerializedClientFields"] =
      reply["joinTicket"]["SerializedClientFields"];
  ti["EncryptedServerFields"] = reply["joinTicket"]["EncryptedServerFields"];
  tmpl["__joinTicket"] = ti.dump();

  std::string body = tmpl.dump();
  std::vector<uint8_t> out(prefix.begin(), prefix.end());
  leb128(out, body.size());
  out.insert(out.end(), body.begin(), body.end());
  out.insert(out.end(), tail.begin(), tail.end());
  return out;
}

std::vector<uint8_t> build92() {
  uint32_t v8 = 0;
  RAND_bytes(reinterpret_cast<uint8_t*>(&v8), 4);
  constexpr uint64_t V9 = 0x63E25F26ull;
  constexpr uint64_t M64 = ~0ull;
  uint64_t x = (uint64_t(v8) << 32) | (uint64_t(v8) ^ V9);
  uint64_t zz = x << 1;
  if (v8 & 0x80000000u) zz = ~zz; // ^ M64 on the sign branch
  std::vector<uint8_t> out;
  out.push_back(0x92);
  leb128(out, zz);
  return out;
}

std::vector<uint8_t> earlyAuth(const std::string& ct, int version) {
  std::vector<std::string> parts;
  {
    size_t pos = 0;
    while (true) {
      size_t s = ct.find(';', pos);
      if (s == std::string::npos) {
        parts.push_back(ct.substr(pos));
        break;
      }
      parts.push_back(ct.substr(pos, s - pos));
      pos = s + 1;
    }
  }
  std::string pre = parts.size() > 2 ? b64decode(parts[2]) : "";
  std::string auth = parts.size() > 3 ? b64decode(parts[3]) : "";
  std::vector<uint8_t> out;
  out.push_back(0xA8);
  out.push_back(uint8_t(version & 0xFF));
  out.push_back(uint8_t(pre.size()));
  out.insert(out.end(), pre.begin(), pre.end());
  out.push_back(uint8_t(auth.size()));
  out.insert(out.end(), auth.begin(), auth.end());
  return out;
}

} // namespace msgs

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

Session::Session(QuicClient& qc, const nlohmann::json& js,
                 const nlohmann::json& reply, SessionOptions opt, LogFn log)
    : qc_(qc), js_(js), reply_(reply), opt_(std::move(opt)), log_(std::move(log)) {}

int64_t Session::openStream(uint32_t app, uint64_t chan, const char* label) {
  int64_t sid = qc_.open_bidi_stream();
  if (sid < 0) return sid;
  info_[sid] = SInfo{app, chan};
  qc_.send_stream(sid, streamHeader(app, chan), false);
  log_("-> open stream " + std::to_string(sid) + " app=" + std::to_string(app) +
       " chan=" + std::to_string(chan) + (label && *label ? std::string(" ") + label : ""));
  return sid;
}

void Session::sendFramed(int64_t sid, const std::vector<uint8_t>& payload,
                         const char* label) {
  auto f = frame(payload);
  qc_.send_stream(sid, f, false);
  log_(std::string("-> send ") + (label ? label : "payload") + " " +
       std::to_string(payload.size()) + "B");
}

void Session::onConnected() {
  // probe10.on_connected, in native sessioncap order
  ctrl_sid_ = openStream(APP, CTRL, "ctrl");
  if (ctrl_sid_ < 0) return;
  sendFramed(ctrl_sid_, {0x00, 0x00}, "ctrl hello");

  struct {
    uint64_t chan;
    uint64_t wire;
  } opens[] = {{3, 2}, {5, 4}, {6, 6}, {11, 8}};
  for (auto& o : opens) {
    std::vector<uint8_t> b = {2, uint8_t(APP)};
    b.push_back(uint8_t(o.chan >> 24));
    b.push_back(uint8_t(o.chan >> 16));
    b.push_back(uint8_t(o.chan >> 8));
    b.push_back(uint8_t(o.chan));
    b.push_back(uint8_t(o.wire >> 24));
    b.push_back(uint8_t(o.wire >> 16));
    b.push_back(uint8_t(o.wire >> 8));
    b.push_back(uint8_t(o.wire));
    sendFramed(ctrl_sid_, b, ("openU c" + std::to_string(o.chan)).c_str());
  }

  // early auth + burst on chan1
  std::string ct = jsonStr(js_, "ClientTicket");
  int version = 0;
  if (!ct.empty()) {
    size_t s = ct.rfind(';');
    if (s != std::string::npos) {
      try {
        version = std::stoi(ct.substr(s + 1));
      } catch (...) {
        version = 0;
      }
    }
  }
  chan1_sid_ = openStream(APP, 1, "chan1");
  if (chan1_sid_ < 0) return;
  sendFramed(chan1_sid_, msgs::earlyAuth(ct, version), "EARLY-AUTH");

  if (opt_.a7_mode == "real") {
    std::string name;
    if (const char* env = getenv("RBX_A7")) name = env;
    else name = opt_.a7_name.empty() ? "msg_0006_a4_c1.bin" : opt_.a7_name;
    bool ok = false;
    std::string a7 = readFileBytes(opt_.cap_dir + "/" + name, &ok);
    if (!ok) {
      log_("  !! A7 real file missing: " + opt_.cap_dir + "/" + name);
    } else {
      sendFramed(chan1_sid_, std::vector<uint8_t>(a7.begin(), a7.end()),
                 ("A7-real(" + std::to_string(a7.size()) + "B)").c_str());
    }
  } else if (opt_.a7_mode == "empty") {
    sendFramed(chan1_sid_, {0xA7, 0x00, 0x00}, "A7-empty");
  } // "skip": no A7 (native acct2cap does this too)

  std::string err;
  auto m90 = msgs::build90(js_, reply_, opt_.cap_dir, &err);
  if (!err.empty()) {
    log_("  !! " + err);
  } else {
    sendFramed(chan1_sid_, m90, "90");
  }
  sendFramed(chan1_sid_, msgs::build92(), "92");
  sendFramed(chan1_sid_, msgs::build8A(js_), "8A");
  sendFramed(chan1_sid_, {0x8F, 0x00}, "8F");
}

void Session::onStream(int64_t sid, const uint8_t* p, size_t n, bool) {
  auto it = info_.find(sid);
  if (it == info_.end()) {
    // server-opened stream: wait for its 7B header [06 01 app chan]
    auto& hb = hdr_buf_[sid];
    hb.insert(hb.end(), p, p + n);
    if (hb.size() < 7) return;
    if (hb[0] == 6 && hb[1] == 1) {
      SInfo si{hb[2], be32(hb.data() + 3)};
      info_[sid] = si;
      log_("  <<< SERVER STREAM " + std::to_string(sid) +
           " app=" + std::to_string(si.app) + " chan=" + std::to_string(si.chan));
      std::vector<uint8_t> payload(hb.begin() + 7, hb.end());
      hdr_buf_.erase(sid);
      if (!payload.empty()) processPayload(si, payload.data(), payload.size());
    } else {
      hdr_buf_.erase(sid); // probe10: unknown stream without header -> drop
    }
    return;
  }
  if (n) processPayload(it->second, p, n);
}

void Session::processPayload(const SInfo& si, const uint8_t* p, size_t n) {
  if (si.app == APP && si.chan == 1) {
    chan1_rx.insert(chan1_rx.end(), p, p + n);
    log_("      <<< RX chan1 " + std::to_string(n) + "B: " + hexs(p, n));
    if (answered && !peer_assigned) {
      peer_assigned = true;
      log_("  *** POST-ANSWER TRAFFIC on chan1 — server kept the session "
           "(peer assignment / replication)");
    }
    tryChallenge();
  } else if (si.app == APP || si.app == 0) {
    log_("      <<< RX app=" + std::to_string(si.app) +
         " chan=" + std::to_string(si.chan) + " " + std::to_string(n) +
         "B: " + hexs(p, n));
    parseCtrl(p, n);
  } else {
    log_("      <<< RX app=" + std::to_string(si.app) +
         " chan=" + std::to_string(si.chan) + " " + std::to_string(n) +
         "B (unhandled): " + hexs(p, n));
  }
}

void Session::parseCtrl(const uint8_t* p, size_t n) {
  size_t off = 0;
  while (off < n) {
    uint64_t ln = 0;
    size_t k = compactVarintDecode(p + off, n - off, &ln);
    if (!k) break;
    off += k;
    if (ln == 0 || off + ln > n) break;
    const uint8_t* body = p + off;
    off += size_t(ln);
    if (ln >= 10 && body[0] == 2) {
      uint32_t app = body[1];
      uint64_t chan = be32(body + 2);
      uint64_t wire = be32(body + 6);
      log_("     ctrl OpenUnreliable app=" + std::to_string(app) +
           " chan=" + std::to_string(chan) + " wire=" + std::to_string(wire));
    }
  }
}

void Session::tryChallenge() {
  if (challenge_seen) return;
  const auto& d = chan1_rx;
  // The walk itself is a pure function (blob.h) — fuzzed separately; here is
  // only the I/O + solve dispatch around its hit.
  size_t msg_off = 0, msg_len = 0;
  if (!rbxclient::findChallengeFrame(d.data(), d.size(), &msg_off, &msg_len))
    return; // no complete challenge frame yet: wait for more data
  const uint8_t* msg = d.data() + msg_off;
  size_t mlen = msg_len;
  u1_ = le32(msg + 1);
  u2_ = le32(msg + 5);
  challenge_seen = true;
  if (FILE* f = fopen(opt_.chal_path.c_str(), "wb")) {
    fwrite(msg, 1, mlen, f);
    fclose(f);
  }
  log_("  *** CHALLENGE u1=0x" + hex32(u1_) + " u2=0x" + hex32(u2_) +
       " blob=" + std::to_string(le32(msg + 9)) + "B -> " + opt_.chal_path);
  solveNow(msg, mlen);
}

void Session::solveNow(const uint8_t* msg, size_t mlen) {
  int64_t t0 = nowMs();
  std::string job = jsonStr(js_, "GameId");
  std::string err;
  uint32_t ans = 0;
  if (!rbxclient::solveMessage(msg, mlen, job, ans, &err)) {
    solve_failed = true;
    log_("  LOCAL SOLVE FAILED: " + err);
    return;
  }
  solve_ms = nowMs() - t0;
  solved = true;
  uint32_t sent = ans;
  if (opt_.corrupt) {
    sent ^= 0xDEADBEEFu;
    log_("  -> CORRUPTED ANSWER 0x" + hex32(sent) +
         " (control test), sending");
  } else {
    log_("  -> LOCAL ANSWER 0x" + hex32(sent) + " (solved in " +
         std::to_string(solve_ms) + "ms), sending");
  }
  answer = sent;
  sendAnswer(sent);
  answered = true;
  if (opt_.routes) routes_at_ms_ = nowMs() + 60;
}

void Session::sendAnswer(uint32_t sent) {
  auto resp = rbxclient::buildResponse(u2_, sent);
  int64_t sid = ensureChan1();
  if (sid < 0) return;
  sendFramed(sid, resp, "9B-LOCAL");
}

int64_t Session::ensureChan1() {
  if (chan1_sid_ >= 0) {
    bool reset = false;
    for (int64_t r : resets)
      if (r == chan1_sid_) reset = true;
    if (!reset) return chan1_sid_;
  }
  chan1_sid_ = openStream(APP, 1, "(chan1-reopen)");
  log_("  -> re-opened chan1 as stream " + std::to_string(chan1_sid_) +
       " after reset");
  return chan1_sid_;
}

void Session::sendRoutes() {
  int64_t sid = openStream(APP, 11, "routes");
  if (sid < 0) return;
  for (auto* r : kRoutes) {
    auto b = hx(r);
    sendFramed(sid, b, ("route(" + std::to_string(b.size()) + "B)").c_str());
  }
}

void Session::tick(int64_t now_ms) {
  if (routes_at_ms_ >= 0 && now_ms >= routes_at_ms_) {
    routes_at_ms_ = -1;
    sendRoutes();
  }
}

void Session::onReset(int64_t sid, uint64_t code) {
  resets.push_back(sid);
  log_("  <<< StreamReset " + std::to_string(sid) + " err=" +
       std::to_string(code));
}

void Session::onStopped(int64_t sid, uint64_t code) {
  log_("  <<< StopSending " + std::to_string(sid) + " err=" +
       std::to_string(code));
}

// ---------------------------------------------------------------------------
// DummySession (app=6 DummyClient connection)
// ---------------------------------------------------------------------------

namespace {
constexpr uint32_t APP6 = 6;
}

DummySession::DummySession(QuicClient& qc, SessionOptions opt, LogFn log)
    : qc_(qc), opt_(std::move(opt)), log_(std::move(log)) {}

int64_t DummySession::openStream(uint32_t app, uint64_t chan) {
  int64_t sid = qc_.open_bidi_stream();
  if (sid < 0) return sid;
  info_[sid] = SInfo{app, chan};
  qc_.send_stream(sid, streamHeader(app, chan), false);
  log_("  DUMMY -> open stream " + std::to_string(sid) + " app=" +
       std::to_string(app) + " chan=" + std::to_string(chan));
  return sid;
}

void DummySession::sendFramed(int64_t sid, const std::vector<uint8_t>& payload,
                              const char* label) {
  auto f = frame(payload);
  qc_.send_stream(sid, f, false);
  log_(std::string("  DUMMY -> send ") + (label ? label : "payload") + " " +
       std::to_string(payload.size()) + "B");
}

void DummySession::onConnected() {
  ctrl_ = openStream(APP6, CTRL);
  if (ctrl_ < 0) return;
  sendFramed(ctrl_, {0x00, 0x00}, "hello");
  auto openU = [&](uint64_t chan, uint64_t wire) {
    std::vector<uint8_t> b = {2, uint8_t(APP6)};
    b.push_back(uint8_t(chan >> 24));
    b.push_back(uint8_t(chan >> 16));
    b.push_back(uint8_t(chan >> 8));
    b.push_back(uint8_t(chan));
    b.push_back(uint8_t(wire >> 24));
    b.push_back(uint8_t(wire >> 16));
    b.push_back(uint8_t(wire >> 8));
    b.push_back(uint8_t(wire));
    sendFramed(ctrl_, b, ("openU c" + std::to_string(chan)).c_str());
  };
  openU(2, 2);
  openU(3, 4);
  c1_ = openStream(APP6, 1);
  c3_ = openStream(APP6, 3);
  sendFramed(c3_, syncTmpl(), "sync#0");
  sendFramed(c1_, pingTmpl(), "ping#0");
  next_ping_ms_ = nowMs() + 1000;
  next_sync_ms_ = nowMs() + 5000;
}

void DummySession::onStream(int64_t sid, const uint8_t* p, size_t n, bool) {
  auto it = info_.find(sid);
  if (it == info_.end()) {
    auto& hb = hdr_buf_[sid];
    hb.insert(hb.end(), p, p + n);
    if (hb.size() < 7) return;
    if (hb[0] == 6 && hb[1] == 1) {
      SInfo si{hb[2], be32(hb.data() + 3)};
      info_[sid] = si;
      log_("  DUMMY <<< SERVER STREAM " + std::to_string(sid) + " app=" +
           std::to_string(si.app) + " chan=" + std::to_string(si.chan));
      rx += hb.size() - 7;
      hdr_buf_.erase(sid);
    } else {
      hdr_buf_.erase(sid);
    }
    return;
  }
  if (n) {
    rx += n;
    log_("  DUMMY <<< RX app=" + std::to_string(it->second.app) +
         " chan=" + std::to_string(it->second.chan) + " " + std::to_string(n) +
         "B: " + hexs(p, n));
  }
}

void DummySession::tick(int64_t now_ms) {
  if (c1_ < 0) return;
  if (next_ping_ms_ >= 0 && now_ms >= next_ping_ms_) {
    next_ping_ms_ += 1000;
    sendFramed(c1_, pingTmpl(), "ping");
  }
  if (next_sync_ms_ >= 0 && now_ms >= next_sync_ms_) {
    next_sync_ms_ += 5000;
    sendFramed(c3_, syncTmpl(), "sync");
  }
}

} // namespace rbx
