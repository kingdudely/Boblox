// config.cpp — see config.h.
#include "quic/config.h"

#include "join/target.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rbx {
namespace quic {
namespace {

bool intEnv(const char* name, int* out, std::string* err) {
  const char* e = getenv(name);
  if (!e) return true;
  try {
    *out = std::stoi(e);
  } catch (...) {
    if (err) *err = std::string("bad $") + name + " " + e;
    return false;
  }
  return true;
}

} // namespace

bool configFromScript(const nlohmann::json& js, QuicClient::Config* cfg,
                      std::string* err) {
  rbx::JoinTarget t;
  if (!rbx::readJoinTarget(js, &t, err)) return false;
  if (!t.has_token) {
    if (err) *err = "join script has no usable NetStackTokenValue";
    return false;
  }
  cfg->alpn = "rbx-rtcio";  // the native game conn ALPN, always
  cfg->udmux_ip = t.udmux_host;
  cfg->rcc_ip = t.rcc_host;
  // Native RbxTransport uses NetStackPort for both the UDP destination and
  // RUPP; the Udmux port is a RakNet-only distinction.
  cfg->port = t.netstack_port;
  std::memcpy(cfg->token, t.token, sizeof cfg->token);
  return true;
}

bool configFromEnv(QuicClient::Config* cfg, std::string* err) {
  if (const char* e = getenv("RBX_TOS")) {
    const int tos = QuicClient::parseTos(e);
    if (tos == -2 || tos < 0 || tos > 255) {
      if (err)
        *err = std::string("bad $RBX_TOS ") + e +
               " (want 0-255, 0x.., or AF41/EF/CSn)";
      return false;
    }
    cfg->tos = tos;
  }
  if (const char* e = getenv("RBX_MAX_UDP")) {
    try {
      cfg->max_tx_payload = std::stoi(e);
    } catch (...) {
      if (err)
        *err = std::string("bad $RBX_MAX_UDP ") + e + " (want 1200-1452)";
      return false;
    }
  }
  // RUPP capability word (8 bytes LE in the header). The native game
  // connection advertises 0x002dd3c750b70892; the web client advertises
  // 0x002dc3c750b30892. The bits select the join path, so keep it testable.
  if (const char* e = getenv("RBX_CAPS")) {
    char* end = nullptr;
    const unsigned long long v = strtoull(e, &end, 16);
    if (!end || *end || (end - e) != 16) {
      if (err) *err = std::string("bad $RBX_CAPS ") + e + " (want 16 hex chars)";
      return false;
    }
    cfg->local_caps = v;
  }
  int port = 0;
  if (getenv("RBX_DEST_PORT")) {
    if (!intEnv("RBX_DEST_PORT", &port, err)) return false;
    cfg->port = uint16_t(port);
  }
  if (getenv("RBX_RUPP_PORT")) {
    if (!intEnv("RBX_RUPP_PORT", &port, err)) return false;
    cfg->rcc_port = uint16_t(port);
  }
  return true;
}

} // namespace quic
} // namespace rbx
