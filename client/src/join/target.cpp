// target.cpp — see target.h.
#include "join/target.h"

#include "common/util.h"

#include <cstdio>
#include <cstring>
#include <utility>

namespace rbx {
namespace {

using nlohmann::json;

bool arrayField(const json& js, const char* name) {
  return js.contains(name) && js[name].is_array() && !js[name].empty();
}

std::string addressOf(const json& endpoint) {
  return endpoint.is_object() ? endpoint.value("Address", std::string()) : "";
}

// NetStackPort arrives as a number in v1 joins and as a string in some
// legacy replies (both were accepted before this was shared).
uint16_t portOf(const json& v) {
  if (v.is_number_integer()) return uint16_t(v.get<int>());
  if (v.is_string()) {
    try {
      return uint16_t(std::stoi(v.get<std::string>()));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

// The key fields come in two shapes: a bare base64 key, or a keyring
// document *string* ({"applications":{"<app>":{"versions":[{"id":n,
// "value":"<b64>"}]}}}), which is what ClientPublicKeyData always is. Take
// the first 32-byte value.
bool keyFromField(const json& js, const char* name, uint8_t out[32],
                  std::string* which) {
  if (!js.contains(name) || !js[name].is_string()) return false;
  // Join fields are percent-encoded ("…%3D" for "…="), sometimes a whole
  // keyring document — decode once, then look for a 32-byte b64 value.
  const std::string s = percentDecode(js[name].get<std::string>());
  std::vector<std::pair<std::string, std::string>> cands;
  if (!s.empty() && s[0] == '{') {
    const json doc = json::parse(s, nullptr, false);
    if (doc.is_discarded() || !doc.contains("applications")) return false;
    for (auto it = doc["applications"].begin(); it != doc["applications"].end();
         ++it) {
      const json& body = it.value();
      if (!body.is_object() || !body.contains("versions")) continue;
      for (const json& v : body["versions"]) {
        if (v.is_object() && v.contains("value") && v["value"].is_string())
          cands.emplace_back(it.key(), v["value"].get<std::string>());
      }
    }
  } else {
    cands.emplace_back("raw", s);
  }
  for (const auto& [app, b64] : cands) {
    const std::string raw = b64decode(b64);
    if (raw.size() == 32) {
      std::memcpy(out, raw.data(), 32);
      if (which) *which = app;
      return true;
    }
  }
  if (which) *which = s.substr(0, 120);
  return false;
}

} // namespace

bool readIpv4(const std::string& s, uint8_t out[4]) {
  unsigned b[4] = {0, 0, 0, 0};
  if (sscanf(s.c_str(), "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) != 4)
    return false;
  for (int i = 0; i < 4; ++i) out[i] = uint8_t(b[i]);
  return true;
}

bool readJoinTarget(const json& js, JoinTarget* out, std::string* err) {
  const auto fail = [err](const std::string& m) {
    if (err) *err = m;
    return false;
  };

  if (!js.contains("NetStackPort")) return fail("joinScript lacks NetStackPort");
  out->netstack_port = portOf(js["NetStackPort"]);
  if (out->netstack_port == 0) return fail("joinScript has no usable NetStackPort");

  if (!arrayField(js, "UdmuxEndpoints"))
    return fail("join script has no UdmuxEndpoints[]");
  out->udmux_host = addressOf(js["UdmuxEndpoints"][0]);
  if (out->udmux_host.empty())
    return fail("join script has no UdmuxEndpoints[0].Address");
  out->udmux_port = js["UdmuxEndpoints"][0].contains("Port")
                        ? portOf(js["UdmuxEndpoints"][0]["Port"])
                        : out->netstack_port;
  if (out->udmux_port == 0) out->udmux_port = out->netstack_port;

  if (!arrayField(js, "ServerConnections"))
    return fail("joinScript lacks ServerConnections[]");
  out->rcc_host = addressOf(js["ServerConnections"][0]);
  if (out->rcc_host.empty())
    return fail("joinScript lacks ServerConnections[0].Address");
  // RUPP TLV2 on the handshake (subtype 1) carries rcc_ip:NetStackPort: the
  // native's rupp_tx001 and the edge's validation both want that value, and a
  // Req1 with the ServerConnections port is ignored outright (measured on a
  // live job, same account). The ServerConnections port is still kept — it is
  // what the *established* session (subtype 2) advertises on the same TLV.
  out->rcc_port = out->netstack_port;
  if (js["ServerConnections"][0].contains("Port"))
    out->server_port = portOf(js["ServerConnections"][0]["Port"]);

  // The token is parsed but not required: a caller may supply one out of
  // band (--token), and policy belongs to whoever needs it.
  if (js.contains("NetStackTokenValue") && js["NetStackTokenValue"].is_string()) {
    const std::string tok = b64decode(js["NetStackTokenValue"].get<std::string>());
    if (tok.size() == 16) {
      std::memcpy(out->token, tok.data(), 16);
      out->has_token = true;
    }
  }

  if (js.contains("ClientTicket") && js["ClientTicket"].is_string())
    out->client_ticket = js["ClientTicket"].get<std::string>();
  return true;
}

bool readSessionCrypto(const json& js, JoinTarget* out, std::string* err) {
  const auto fail = [err](const std::string& m) {
    if (err) *err = m;
    return false;
  };
  if (!js.contains("EphemeralEarlyPubKey"))
    return fail("join script has no EphemeralEarlyPubKey");
  std::string which;
  if (!keyFromField(js, "EphemeralEarlyPubKey", out->peer_pub, &which))
    return fail("EphemeralEarlyPubKey has no 32-byte key: " + which);
  out->peer_key_from = which;

  std::vector<std::string> parts;
  std::string cur;
  for (char c : out->client_ticket) {
    if (c == ';') {
      parts.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  parts.push_back(cur);
  out->ticket_parts = parts.size();
  if (parts.size() < 4) return true;  // no blobs: has_crypto stays false

  const std::string pre_raw = b64decode(parts[2]);
  const std::string auth_raw = b64decode(parts[3]);
  out->ticket_pre.assign(pre_raw.begin(), pre_raw.end());
  out->ticket_auth.assign(auth_raw.begin(), auth_raw.end());
  out->has_crypto = out->ticket_pre.size() >= 16 && out->ticket_auth.size() >= 32;
  return true;
}

} // namespace rbx
