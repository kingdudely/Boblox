// config.h — joinScript / environment -> QuicClient::Config.
//
// play_main and hs_main each used to carry their own JSON-to-config mapping
// ("same fields hs1818 uses") plus their own copy of the RBX_TOS /
// RBX_MAX_UDP parsing. One mapping here: a join script read by
// join/target.cpp, then the carrier knobs that are transport diagnostics.
#pragma once

#include <nlohmann/json.hpp>

#include <string>

#include "quic/quic_client.h"

namespace rbx {
namespace quic {

// Native ALPN ("rbx-rtcio", always), the Udmux edge, NetStackPort as the UDP
// destination and the RUPP token. False with *err naming the first missing or
// malformed field.
bool configFromScript(const nlohmann::json& js, QuicClient::Config* cfg,
                      std::string* err);

// Carrier knobs, all optional (unset = native parity):
//   RBX_TOS=AF41|136|0x88   mark game UDP with that TOS byte
//   RBX_MAX_UDP=N           cap QUIC datagrams, clamped to [1200,1452]
//   RBX_DEST_PORT=N         override the UDP destination port
//   RBX_RUPP_PORT=N         override the RUPP (TLV2) port
// False with *err on a malformed value.
bool configFromEnv(QuicClient::Config* cfg, std::string* err);

} // namespace quic
} // namespace rbx
