// join_flow.cpp — see join_flow.h.
#include "raknet/join_flow.h"

#include "common/util.h"
#include "raknet/join_msg.h"
#include "raknet/raknet.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace rbxclient {

void sendJoinFlow(RaknetClient& client, const nlohmann::json& js,
                  const rbx::JoinResult& jr, long long place,
                  const JoinFlowPaths& paths,
                  const std::function<void(const std::string&)>& say) {
  const auto log = say ? say : [](const std::string& m) { printf("%s\n", m.c_str()); };
  std::string err;

  const std::vector<uint8_t> pre = rbx::readFileVec(paths.pre_join);
  if (!pre.empty()) {
    const int n = client.sendJoinMessage(pre.data(), pre.size(), /*ordered=*/false);
    log("raknet: pre-join pad sent " + std::to_string(pre.size()) + "B in " +
        std::to_string(n) + " datagrams");
  }

  const std::vector<uint8_t> tmpl = rbx::readFileVec(paths.join90);
  if (tmpl.size() < 64) {
    log("raknet: 0x90 template " + paths.join90 + " missing — join body held back");
    return;
  }
  rbxmsg::Join90 j90;
  if (!rbxmsg::build90(tmpl, js, jr.reply, &j90, &err)) {
    log("raknet: build90 failed: " + err);
    return;
  }
  {
    const std::vector<uint8_t> msg =
        rbxmsg::sealedMessage(0x90, j90.body, rbxmsg::kKey90, rbxmsg::kState90);
    const int n = client.sendJoinMessage(msg.data(), msg.size());
    log("raknet: 0x90 join body sent (body " + std::to_string(j90.body.size()) +
        "B, json " + std::to_string(j90.json.size()) + "B, " + std::to_string(n) +
        " datagrams)");
  }

  // The 0x92's second input is a build constant that also lives in the 0x90
  // template's tail (sub_664CCD4 = tail[3] ^ (uint16(tail[2]) | 0x22640000)).
  const uint32_t t2 = rbx::le32(tmpl.data() + tmpl.size() - 16);
  const uint32_t t3 = rbx::le32(tmpl.data() + tmpl.size() - 12);
  const uint32_t v9 = t3 ^ ((t2 & 0xFFFFu) | 0x22640000u);
  const uint32_t rng = uint32_t(
      std::chrono::steady_clock::now().time_since_epoch().count() ^ 0x9E3779B9u);
  {
    const std::vector<uint8_t> msg = rbxmsg::build92(rng, v9);
    const int n = client.sendJoinMessage(msg.data(), msg.size());
    log("raknet: 0x92 sent (" + std::to_string(n) + " datagram)");
  }

  const uint32_t uid = js.value("UserId", 0u);
  const std::string ticket = js.value("ClientTicket", std::string());
  const std::vector<uint8_t> t8a = rbx::readFileVec(paths.ticket8a);
  if (uid == 0 || ticket.empty()) {
    log("raknet: 0x8a skipped (join script has no UserId/ClientTicket)");
  } else {
    std::vector<uint8_t> body8a;
    if (!rbxmsg::build8a(uid, ticket, t8a, js, &body8a, &err)) {
      log("raknet: build8a failed: " + err);
      return;
    }
    const uint32_t place_id = uint32_t(place ? place : js.value("PlaceId", 0));
    const std::string key = rbxmsg::key8a(place_id, t2);
    const std::vector<uint8_t> msg = rbxmsg::sealedMessage(
        0x8A, body8a, reinterpret_cast<const uint8_t*>(key.data()), rbxmsg::kState8a);
    const int n = client.sendJoinMessage(msg.data(), msg.size());
    log("raknet: 0x8a ticket submitted (body " + std::to_string(body8a.size()) +
        "B, key " + key + ", " + std::to_string(n) + " datagrams)");
  }

  {
    const std::vector<uint8_t> msg = rbxmsg::build8f("");
    const int n = client.sendJoinMessage(msg.data(), msg.size());
    log("raknet: 0x8f sent (" + std::to_string(n) + " datagram)");
  }
}

} // namespace rbxclient
