// raknet_main.cpp — live RakNet handshake smoke test (raknet1818).
//
//   raknet1818 --join-json run/join.json --token d886… [--seconds 10]
//   raknet1818 --place-id 1818 --roblosecurity <cookie> --token d886…
//
// Joins (or loads) the game, then speaks the native RakNet offline
// handshake to the Udmux endpoint: Req1 -> Reply1, using the RUPP header
// the official client uses. --req2 sends the (experimental) Req2.
#include "join.h"
#include "raknet.h"
#include "util.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

int main(int argc, char** argv) {
  long long place = 0;
  std::string cookie, join_path, job, token_hex, host_override, port_override;
  int seconds = 10;
  bool req2 = false;
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
    else if (a == "--job-id") job = next("--job-id");
    else if (a == "--token") token_hex = next("--token");
    else if (a == "--host") host_override = next("--host");
    else if (a == "--port") port_override = next("--port");
    else if (a == "--seconds") seconds = atoi(next("--seconds").c_str());
    else if (a == "--req2") req2 = true;
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
    if (!rbx::joinGame(place, cookie, job, 0, 0, jr, &err)) {
      fprintf(stderr, "joinGame: %s\n", err.c_str());
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

  const auto log = [](const std::string& m) { printf("%s\n", m.c_str()); };
  rbxclient::RaknetClient client(log);
  if (!client.connect(cfg, &err)) {
    fprintf(stderr, "raknet connect: %s\n", err.c_str());
    return 1;
  }
  for (int i = 0; i < seconds && client.stage() < 2; ++i)
    client.pump(1000);
  printf("raknet: result stage=%d mtu=%u guid=%llu session_token=%s pkts=%llu "
         "payloads=%llu bytes=%llu\n",
         client.stage(), unsigned(client.mtu()),
         (unsigned long long)client.server_guid(),
         client.has_session_token() ? "yes" : "no",
         (unsigned long long)client.pkts_rx(),
         (unsigned long long)client.payloads_rx(),
         (unsigned long long)client.bytes_rx());
  return client.connected() ? 0 : 1;
}
