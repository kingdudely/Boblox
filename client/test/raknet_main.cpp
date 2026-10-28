// raknet_main.cpp — live RakNet handshake smoke test (raknet1818).
//
//   raknet1818 --join-json run/join.json [--seconds 10]
//   raknet1818 --place-id 1818 --roblosecurity <cookie> [--seconds 10]
//
// Joins (or loads) the game, then speaks the native RakNet handshake to the
// Udmux endpoint: Req1 -> Reply1 -> Req2 (SessionCrypto: X25519 + BLAKE2b-512
// + ChaCha20-Poly1305, ticket blobs from the join script) -> Reply2, whose
// tag verifying against our derived rx key proves the server accepted the
// ticket and shares the session keys. --req2 sends the plain experimental
// Req2; --no-crypto disables the encrypted path.
#include "join.h"
#include "join_msg.h"
#include "raknet.h"
#include "session_crypto.h"
#include "util.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using nlohmann::json;

// joinScript NetStackTokenValue is base64 of the 16-byte RUPP TLV1 token.
static bool b64To16(const std::string& in, uint8_t out[16]) {
  static const char* kAlphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int vals[256];
  for (int i = 0; i < 256; ++i) vals[i] = -1;
  for (int i = 0; i < 64; ++i) vals[int((unsigned char)kAlphabet[i])] = i;
  std::vector<uint8_t> bytes;
  int acc = 0, bits = 0;
  for (char c : in) {
    if (c == '=' || c == '\n' || c == '\r') continue;
    const int v = vals[int((unsigned char)c)];
    if (v < 0) return false;
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      bytes.push_back(uint8_t((acc >> bits) & 0xff));
    }
  }
  if (bytes.size() != 16) return false;
  std::memcpy(out, bytes.data(), 16);
  return true;
}

static bool hexTo16(const std::string& in, uint8_t out[16]) {
  std::string s;
  for (char c : in) {
    if (c != ' ' && c != ':') s.push_back(c);
  }
  if (s.size() != 32) return false;
  for (int i = 0; i < 16; ++i) {
    unsigned v = 0;
    if (sscanf(s.c_str() + i * 2, "%2x", &v) != 1) return false;
    out[i] = uint8_t(v);
  }
  return true;
}

static bool hexToBytes(const std::string& in, uint8_t* out, size_t want) {
  std::string s;
  for (char c : in) {
    if (c != ' ' && c != ':') s.push_back(c);
  }
  if (s.size() != want * 2) return false;
  for (size_t i = 0; i < want; ++i) {
    unsigned v = 0;
    if (sscanf(s.c_str() + i * 2, "%2x", &v) != 1) return false;
    out[i] = uint8_t(v);
  }
  return true;
}

static bool b64ToBytes(const std::string& in, size_t want,
                       std::vector<uint8_t>* out) {
  const std::string raw = rbx::b64decode(in);
  if (raw.size() != want) return false;
  out->assign(raw.begin(), raw.end());
  return true;
}

// joinScript.ClientTicket is "<iso>;<b64 258B>;<b64 pre>;<b64 auth>;<2>";
// parts 2/3 are the native client's early-auth blobs (33B and 66B here).
//
// The key fields come in two shapes: a bare base64 key, or a keyring document
// *string* ({"applications":{"<app>":{"versions":[{"id":n,"value":"<b64>"}]}}}),
// which is what ClientPublicKeyData always is. Take the first 32-byte value.
static bool keyFromField(const nlohmann::json& js, const char* name, uint8_t out[32],
                         std::string* which) {
  if (!js.contains(name) || !js[name].is_string()) return false;
  // Join fields are percent-encoded ("…%3D" for "…="), sometimes a whole
  // keyring document — decode once, then look for a 32-byte b64 value.
  const std::string s = rbx::percentDecode(js[name].get<std::string>());
  std::vector<std::pair<std::string, std::string>> cands;
  if (!s.empty() && s[0] == '{') {
    const nlohmann::json doc = nlohmann::json::parse(s, nullptr, false);
    if (doc.is_discarded() || !doc.contains("applications")) return false;
    for (auto it = doc["applications"].begin(); it != doc["applications"].end(); ++it) {
      const nlohmann::json& body = it.value();
      if (!body.is_object() || !body.contains("versions")) continue;
      for (const nlohmann::json& v : body["versions"]) {
        if (v.is_object() && v.contains("value") && v["value"].is_string())
          cands.emplace_back(it.key(), v["value"].get<std::string>());
      }
    }
  } else {
    cands.emplace_back("raw", s);
  }
  for (const auto& [app, b64] : cands) {
    const std::string raw = rbx::b64decode(b64);
    if (raw.size() == 32) {
      std::memcpy(out, raw.data(), 32);
      if (which) *which = app;
      return true;
    }
  }
  if (which) *which = s.substr(0, 120);
  return false;
}

static std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                              std::istreambuf_iterator<char>());
}

static uint32_t le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}

// The native post-handshake sequence (live capture, place 1818): 0x13
// (ID_NEW_INCOMING_CONNECTION, sent by the caller), an un-ordered pre-join pad,
// the 0x90 join body, 0x92 (+ 0x8a and 0x8f from its handler) — all on the
// reliable-ordered channel, the big ones fragmented into 1061-byte pieces.
static void sendJoinFlow(rbxclient::RaknetClient& client, const nlohmann::json& js,
                         const rbx::JoinResult& jr, long long place,
                         const std::string& t90_path, const std::string& t8a_path,
                         const std::string& pre_path) {
  auto say = [](const std::string& m) { printf("%s\n", m.c_str()); };
  std::string err;

  const std::vector<uint8_t> pre = readFile(pre_path);
  if (!pre.empty()) {
    const int n = client.sendJoinMessage(pre.data(), pre.size(), /*ordered=*/false);
    say("raknet: pre-join pad sent " + std::to_string(pre.size()) + "B in " +
        std::to_string(n) + " datagrams");
  }

  const std::vector<uint8_t> tmpl = readFile(t90_path);
  if (tmpl.size() < 64) {
    say("raknet: 0x90 template " + t90_path + " missing — join body held back");
    return;
  }
  rbxmsg::Join90 j90;
  if (!rbxmsg::build90(tmpl, js, jr.reply, &j90, &err)) {
    say("raknet: build90 failed: " + err);
    return;
  }
  {
    const std::vector<uint8_t> msg =
        rbxmsg::sealedMessage(0x90, j90.body, rbxmsg::kKey90, rbxmsg::kState90);
    const int n = client.sendJoinMessage(msg.data(), msg.size());
    say("raknet: 0x90 join body sent (body " + std::to_string(j90.body.size()) +
        "B, json " + std::to_string(j90.json.size()) + "B, " + std::to_string(n) +
        " datagrams)");
  }

  // The 0x92's second input is a build constant that also lives in the 0x90
  // template's tail (sub_664CCD4 = tail[3] ^ (uint16(tail[2]) | 0x22640000)).
  const uint32_t t2 = le32(tmpl.data() + tmpl.size() - 16);
  const uint32_t t3 = le32(tmpl.data() + tmpl.size() - 12);
  const uint32_t v9 = t3 ^ ((t2 & 0xFFFFu) | 0x22640000u);
  const uint32_t rng = uint32_t(
      std::chrono::steady_clock::now().time_since_epoch().count() ^ 0x9E3779B9u);
  {
    const std::vector<uint8_t> msg = rbxmsg::build92(rng, v9);
    const int n = client.sendJoinMessage(msg.data(), msg.size());
    say("raknet: 0x92 sent (" + std::to_string(n) + " datagram)");
  }

  const uint32_t uid = js.value("UserId", 0u);
  const std::string ticket = js.value("ClientTicket", std::string());
  const std::vector<uint8_t> t8a = readFile(t8a_path);
  if (uid == 0 || ticket.empty()) {
    say("raknet: 0x8a skipped (join script has no UserId/ClientTicket)");
  } else {
    std::vector<uint8_t> body8a;
    if (!rbxmsg::build8a(uid, ticket, t8a, js, &body8a, &err)) {
      say("raknet: build8a failed: " + err);
      return;
    }
    const uint32_t place_id = uint32_t(place ? place : js.value("PlaceId", 0));
    const std::string key = rbxmsg::key8a(place_id, t2);
    const std::vector<uint8_t> msg = rbxmsg::sealedMessage(
        0x8A, body8a, reinterpret_cast<const uint8_t*>(key.data()), rbxmsg::kState8a);
    const int n = client.sendJoinMessage(msg.data(), msg.size());
    say("raknet: 0x8a ticket submitted (body " + std::to_string(body8a.size()) +
        "B, key " + key + ", " + std::to_string(n) + " datagrams)");
  }

  {
    const std::vector<uint8_t> msg = rbxmsg::build8f("");
    const int n = client.sendJoinMessage(msg.data(), msg.size());
    say("raknet: 0x8f sent (" + std::to_string(n) + " datagram)");
  }
}

int main(int argc, char** argv) {
  long long place = 0;
  std::string cookie, join_path, job, token_hex, host_override, port_override;
  std::string cap_hex, sid_hex, save_path;
  std::string t90_path = "run/raknet90_template.bin";
  std::string t8a_path = "run/raknet8a_template.bin";
  std::string pre_path = "run/raknet_pre_join.bin";
  long universe = 0;
  int seconds = 10, tok_sub = 0;
  bool req2 = false, no_crypto = false, skip_join = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* flag) -> std::string {
      if (i + 1 >= argc) {
        fprintf(stderr, "%s needs a value\n", flag);
        exit(2);
      }
      return argv[++i];
    };
    if (a == "--place-id") place = atoll(next("--place-id").c_str());
    else if (a == "--roblosecurity") cookie = next("--roblosecurity");
    else if (a == "--join-json") join_path = next("--join-json");
    else if (a == "--save-join") save_path = next("--save-join");
    else if (a == "--job-id") job = next("--job-id");
    else if (a == "--universe-id") universe = atol(next("--universe-id").c_str());
    else if (a == "--token") token_hex = next("--token");
    else if (a == "--cap-word") cap_hex = next("--cap-word");
    else if (a == "--session-id") sid_hex = next("--session-id");
    else if (a == "--host") host_override = next("--host");
    else if (a == "--port") port_override = next("--port");
    else if (a == "--seconds") seconds = atoi(next("--seconds").c_str());
    else if (a == "--90-template") t90_path = next("--90-template");
    else if (a == "--8a-template") t8a_path = next("--8a-template");
    else if (a == "--pre-join") pre_path = next("--pre-join");
    else if (a == "--skip-join") skip_join = true;
    else if (a == "--token-sub") tok_sub = atoi(next("--token-sub").c_str());
    else if (a == "--req2") req2 = true;
    else if (a == "--no-crypto") no_crypto = true;
    else {
      fprintf(stderr, "unknown arg %s\n", a.c_str());
      return 2;
    }
  }
  if (token_hex.empty()) {
    if (const char* env = getenv("RBX_RUPP_TOKEN")) token_hex = env;
  }

  rbx::JoinResult jr;
  std::string err;
  if (!join_path.empty()) {
    if (!rbx::loadJoin(join_path, jr, &err)) {
      fprintf(stderr, "loadJoin: %s\n", err.c_str());
      return 2;
    }
  } else {
    if (place == 0 || cookie.empty()) {
      fprintf(stderr, "--place-id and --roblosecurity (or --join-json) are "
                      "required\n");
      return 2;
    }
    if (universe == 0 && !rbx::resolveUniverse(place, &universe, &err)) {
      fprintf(stderr, "resolveUniverse: %s\n", err.c_str());
      return 2;
    }
    printf("join: place=%lld universe(gameId)=%ld\n", place, universe);
    if (!rbx::joinGame(place, cookie, job, 0, universe, jr, &err)) {
      fprintf(stderr, "joinGame: %s\n", err.c_str());
      return 2;
    }
    if (!save_path.empty() && !rbx::saveJoin(save_path, jr, &err)) {
      fprintf(stderr, "saveJoin: %s\n", err.c_str());
      return 2;
    }
  }
  const json& js = jr.join_script;

  rbxclient::RaknetConfig cfg;
  if (!host_override.empty()) {
    cfg.host = host_override;
  } else if (js.contains("UdmuxEndpoints") && js["UdmuxEndpoints"].is_array() &&
             !js["UdmuxEndpoints"].empty()) {
    cfg.host = js["UdmuxEndpoints"][0].value("Address", "");
  }
  if (cfg.host.empty()) {
    fprintf(stderr, "join script has no UdmuxEndpoints[0].Address\n");
    return 2;
  }
  if (!port_override.empty()) {
    cfg.port = uint16_t(atoi(port_override.c_str()));
  } else if (js.contains("UdmuxEndpoints") && js["UdmuxEndpoints"].is_array() &&
             !js["UdmuxEndpoints"].empty() &&
             js["UdmuxEndpoints"][0].contains("Port")) {
    cfg.port = uint16_t(js["UdmuxEndpoints"][0]["Port"].get<int>());
  } else if (js.contains("NetStackPort")) {
    cfg.port = uint16_t(js["NetStackPort"].get<int>());
  }
  if (js.contains("ClientTicket") && js["ClientTicket"].is_string()) {
    cfg.client_ticket = js["ClientTicket"].get<std::string>();
    printf("ticket: %zu chars\n", cfg.client_ticket.size());
  }
  if (js.contains("ServerConnections") && js["ServerConnections"].is_array() &&
      !js["ServerConnections"].empty()) {
    const std::string rcc = js["ServerConnections"][0].value("Address", "");
    unsigned b[4] = {0, 0, 0, 0};
    if (sscanf(rcc.c_str(), "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) == 4) {
      for (int i = 0; i < 4; ++i) cfg.rcc_ip[i] = uint8_t(b[i]);
    }
  }
  if (js.contains("NetStackPort")) {
    cfg.rcc_port = uint16_t(js["NetStackPort"].get<int>());
  }
  if (!token_hex.empty()) {
    if (!hexTo16(token_hex, cfg.join_token)) {
      fprintf(stderr, "--token must be exactly 32 hex chars\n");
      return 2;
    }
  } else if (js.contains("NetStackTokenValue") &&
             b64To16(js["NetStackTokenValue"].get<std::string>(),
                     cfg.join_token)) {
    // token straight from the join script (normal path)
  } else {
    fprintf(stderr, "no RUPP token: join script has no usable "
                    "NetStackTokenValue and --token/RBX_RUPP_TOKEN unset\n");
    return 2;
  }
  cfg.send_req2 = req2;
  cfg.force_token_sub = uint8_t(tok_sub);

  // Session crypto inputs straight from the join script: the per-session
  // server key plus the two ticket blobs the native sends as early-auth.
  if (!no_crypto && js.contains("EphemeralEarlyPubKey")) {
    std::string which;
    if (keyFromField(js, "EphemeralEarlyPubKey", cfg.peer_pub, &which))
      printf("crypto: peer key from EphemeralEarlyPubKey[%s]\n", which.c_str());
    else
      fprintf(stderr, "EphemeralEarlyPubKey has no 32-byte key: %s\n",
              which.c_str());
    if (js.contains("ClientTicket") && js["ClientTicket"].is_string()) {
      std::vector<std::string> parts;
      std::string cur;
      for (char c : js["ClientTicket"].get<std::string>()) {
        if (c == ';') {
          parts.push_back(cur);
          cur.clear();
        } else {
          cur.push_back(c);
        }
      }
      parts.push_back(cur);
      if (parts.size() >= 4) {
        const std::string pre_raw = rbx::b64decode(parts[2]);
        const std::string auth_raw = rbx::b64decode(parts[3]);
        cfg.ticket_pre.assign(pre_raw.begin(), pre_raw.end());
        cfg.ticket_auth.assign(auth_raw.begin(), auth_raw.end());
        cfg.have_crypto = cfg.ticket_pre.size() >= 16 && cfg.ticket_auth.size() >= 32;
        printf("crypto: peer=%s ticket parts=%zu pre=%zu auth=%zu\n",
               cfg.have_crypto ? "ok" : "missing", parts.size(),
               cfg.ticket_pre.size(), cfg.ticket_auth.size());
      } else {
        fprintf(stderr, "ClientTicket has %zu parts (want 5)\n", parts.size());
      }
    }
  }
  if (!cap_hex.empty() && !hexToBytes(cap_hex, cfg.capability_word, 8)) {
    fprintf(stderr, "--cap-word must be 16 hex chars\n");
    return 2;
  }
  if (!sid_hex.empty() && !hexToBytes(sid_hex, cfg.session_id, 8)) {
    fprintf(stderr, "--session-id must be 16 hex chars\n");
    return 2;
  }

  const auto log = [](const std::string& m) { printf("%s\n", m.c_str()); };
  rbxclient::RaknetClient client(log);
  if (!client.connect(cfg, &err)) {
    fprintf(stderr, "raknet connect: %s\n", err.c_str());
    return 1;
  }
  for (int i = 0; i < seconds && !client.reply2_ok(); ++i) client.pump(1000);

  // Once the datagram keys are installed, open the RakNet session: the first
  // encrypted datagram is ID_CONNECTION_REQUEST (0x09), exactly like the
  // native's first post-handshake frame.
  if (client.reply2_ok() && client.data_ready()) {
    client.sendConnectionRequest();
    client.pump(600);
    // ACK the ACCEPTED so the server stops retransmitting, then open the
    // session like the native does (ID_NEW_INCOMING_CONNECTION).
    client.sendAcks();
    client.sendNewIncoming();
    if (skip_join) {
      printf("raknet: --skip-join: join body/ticket not sent\n");
    } else {
      sendJoinFlow(client, js, jr, place, t90_path, t8a_path, pre_path);
    }
    for (int i = 0; i < seconds; ++i) {
      client.pump(1000);
      client.sendAcks();
    }
    if (const uint8_t* dg = client.last_datagram()) {
      std::string hex;
      for (size_t i = 0; i < client.last_datagram_len() && i < 48; ++i) {
        char b[4];
        snprintf(b, sizeof b, "%02x", dg[i]);
        hex += b;
      }
      printf("raknet: last server datagram %zuB: %s\n",
             client.last_datagram_len(), hex.c_str());
    }
  }
  printf("raknet: result stage=%d mtu=%u guid=%llu session_token=%s "
         "reply2=%s data=%s datagrams=%llu/%llu acks=%llu srv_acks=%llu dups=%llu "
         "pkts=%llu payloads=%llu bytes=%llu\n",
         client.stage(), unsigned(client.mtu()),
         (unsigned long long)client.server_guid(),
         client.has_session_token() ? "yes" : "no",
         client.reply2_ok() ? "verified" : "no",
         client.data_ready() ? "armed" : "no",
         (unsigned long long)client.data_datagrams_rx(),
         (unsigned long long)client.data_frames_rx(),
         (unsigned long long)client.acks_sent(),
         (unsigned long long)client.server_acks(),
         (unsigned long long)client.dups_dropped(),
         (unsigned long long)client.pkts_rx(),
         (unsigned long long)client.payloads_rx(),
         (unsigned long long)client.bytes_rx());
  return client.reply2_ok() ? 0 : 1;
}
