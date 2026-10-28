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
  // 0xA7 script-hash queue, opt-in via --a7 FILE (empty = not sent).
  // It is the trigger on RbxTransport, but the captured native *RakNet*
  // session never sends one (FINDINGS "RakNet inner protocol": 0x13, 0x90,
  // 0x92, 0x8a, 0x8f, 0x9b are the only ordered messages before replication),
  // and adding it consumes an ordering index the native has already assigned
  // to 0x90/0x92/0x8a/0x8f. So it is a knob, not the default sequence.
  std::string a7;
};

// Sends the pre-join pads, 0x90 join body, 0x92, 0x8a ClientTicket and 0x8f in
// the order the native sends them (plus an optional 0xA7 queue when --a7 is
// given). Progress lines go to `say` when set
// (default: stdout).
void sendJoinFlow(RaknetClient& client, const nlohmann::json& js,
                  const rbx::JoinResult& jr, long long place,
                  const JoinFlowPaths& paths,
                  const std::function<void(const std::string&)>& say = {});

} // namespace rbxclient
