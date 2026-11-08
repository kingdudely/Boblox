// join_flow.h — the native post-handshake RakNet sequence.
//
// Kept out of the CLI so raknet_main.cpp is only argument parsing and config
// wiring, and so the sequence (the part that actually has to match the
// native byte for byte) has one home.
#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

#include "join/join.h"

namespace rbxclient {

class RaknetClient;

// Template/default inputs, relative to the repo root (all optional except the
// 0x90 body template; a missing one is reported and that step is skipped).
struct JoinFlowPaths {
  std::string join90 = "run/raknet90_template.bin";
  std::string ticket8a = "run/raknet8a_template.bin";
  std::string pre_join = "run/raknet_pre_join.bin";
  // 0xA7 script-hash queue. Default "skip": the native's live RakNet stream
  // carries no 0xA7 at all (decrypted 1818 capture, /tmp/nat_cmp), and sending
  // it at ordering slot 1 pushes 0x90 to slot 2, after which the RCC never
  // sends its 0x93 settings push. It is a RbxTransport-burst message; keep
  // --a7 FILE for A/B runs.
  std::string a7 = "skip";
  // Platform name the 0x8a body declares ("Android", "Windows", …). Empty
  // keeps the captured template's value. The app fills it from the same
  // identity the session was minted with (rbx::joinPlatform()), so the body
  // and the User-Agent can never disagree.
  std::string platform;
};

// Sends the pre-join pads, 0xA7, 0x90 join body, 0x92, 0x8a ClientTicket and
// 0x8f in the order the native sends them. Progress lines go to `say` when set
// (default: stdout).
void sendJoinFlow(RaknetClient& client, const nlohmann::json& js,
                  const rbx::JoinResult& jr, long long place,
                  const JoinFlowPaths& paths,
                  const std::function<void(const std::string&)>& say = {});

} // namespace rbxclient
