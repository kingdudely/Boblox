// join_flow.cpp — see join_flow.h.
#include "raknet/join_flow.h"

#include "common/hex.h"
#include "common/util.h"
#include "raknet/join_msg.h"
#include "raknet/raknet.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace rbxclient {

void sendJoinFlow(RaknetClient& client, const nlohmann::json& js,
                  const rbx::JoinResult& jr, long long place,
                  const JoinFlowPaths& paths,
                  const std::function<void(const std::string&)>& say) {
  const auto log = say ? say : [](const std::string& m) { printf("%s\n", m.c_str()); };
  std::string err;

  // The native's MTU-pad exchange before the join: the client sends the
  // 1160-byte 0x00 pad twice and the 0x03 pad twice, interleaved with the
  // server's pads and pings (captured TX: pad 0x00 sid0, pad 0x00 sid1,
  // 0x00 9B, pings, pad 0x03 sid2, pad 0x03 sid3, then 0x90). The RCC treats
  // this exchange as the connection's capability/attestation signal — a
  // single 0x00 pad (what this client used to send) leaves it half-done and
  // the session never reaches the app-level challenge.
  // OFF by default. This exchange used to look mandatory (it appears in the
  // native's TX stream), but sending it is what kept the session from ever
  // reaching the challenge: with the four pads in front of the join the RCC
  // answered 0x93 and then only padded at us, and the 0x9B never came. Without
  // them the challenge arrives 1-2 datagrams after 0x93, the solver answers it,
  // 0x97 lands and the world builds (measured 2026-10-28). RBX_PRE_PAD=1 sends
  // the old exchange for A/B runs.
  const std::vector<uint8_t> pre = rbx::readFileVec(paths.pre_join);
  const char* want_pre = getenv("RBX_PRE_PAD");
  if (!pre.empty() && want_pre && *want_pre && *want_pre != '0') {
    static const uint8_t kPadIds[4] = {0x00, 0x00, 0x03, 0x03};
    for (uint8_t id : kPadIds) {
      std::vector<uint8_t> pad = pre;
      pad[0] = id;
      const int n = client.sendJoinMessage(pad.data(), pad.size(), /*ordered=*/false);
      char b[4];
      snprintf(b, sizeof b, "%02x", id);
      log(std::string("raknet: MTU pad 0x") + b + " sent " +
          std::to_string(pad.size()) + "B in " + std::to_string(n) + " datagrams");
    }
  }

  // 0xA7 script-hash queue: `[a7][count u16le][count x 16B]`. On RbxTransport
  // a non-empty queue is what makes the RCC issue the 0x9B challenge; on RakNet
  // the native sends no A7 at all, so this only runs when --a7 is given (it
  // shifts the ordering indexes the native's sequence depends on).
  if (!paths.a7.empty()) {
    const std::vector<uint8_t> a7 = rbx::readFileVec(paths.a7);
    if (a7.size() >= 3 && a7[0] == 0xA7) {
      const size_t count = size_t(a7[1] | (a7[2] << 8));
      if (a7.size() != 3 + count * 16) {
        log("raknet: 0xA7 template " + paths.a7 + " is 3+" +
            std::to_string(count * 16) + "B expected, got " +
            std::to_string(a7.size()) + "B — not sent");
      } else if (count == 0) {
        log("raknet: 0xA7 template has count=0 — the server will not challenge");
      } else {
        const int n = client.sendJoinMessage(a7.data(), a7.size());
        log("raknet: 0xA7 script hashes sent (" + std::to_string(a7.size()) +
            "B, count " + std::to_string(count) + ", " + std::to_string(n) +
            " datagrams)");
      }
    } else {
      log("raknet: 0xA7 template " + paths.a7 + " missing or malformed — not sent");
    }
  }

  const std::vector<uint8_t> tmpl = rbx::readFileVec(paths.join90);
  if (tmpl.size() < 64) {
    log("raknet: 0x90 template " + paths.join90 + " missing — join body held back");
    return;
  }
  // RBX_JOIN90_RAW=<file>: send a captured 0x90 message (id + sealed body)
  // verbatim instead of building one. The envelope key is a build constant, so
  // a native capture's message is still well-formed for the RCC; this isolates
  // a body difference from a transport difference in one run.
  if (const char* raw = getenv("RBX_JOIN90_RAW")) {
    const std::vector<uint8_t> msg = rbx::readFileVec(raw);
    if (msg.size() < 32 || msg[0] != 0x90) {
      log("raknet: RBX_JOIN90_RAW is not a 0x90 message — not sent");
    } else {
      const int n = client.sendJoinMessage(msg.data(), msg.size());
      log("raknet: 0x90 REPLAYED verbatim (" + std::to_string(msg.size()) +
          "B, " + std::to_string(n) + " datagrams) from " + raw);
    }
  } else {
  rbxmsg::Join90 j90;
  if (!rbxmsg::build90(tmpl, js, jr.reply, &j90, &err)) {
    log("raknet: build90 failed: " + err);
    return;
  }
  if (const char* dump = getenv("RBX_DUMP_JOIN90")) {
    // Field-level diffing against the native body needs our plaintext JSON;
    // the sealed body is opaque and the native's unseals with tools/raknet_seal.py.
    if (FILE* f = fopen((std::string(dump) + ".json").c_str(), "wb"))
      fwrite(j90.json.data(), 1, j90.json.size(), f), fclose(f);
    if (FILE* f = fopen((std::string(dump) + ".body").c_str(), "wb"))
      fwrite(j90.body.data(), 1, j90.body.size(), f), fclose(f);
  }
  {
    const std::vector<uint8_t> msg =
        rbxmsg::sealedMessage(0x90, j90.body, rbxmsg::kKey90, rbxmsg::kState90);
    const int n = client.sendJoinMessage(msg.data(), msg.size());
    log("raknet: 0x90 join body sent (body " + std::to_string(j90.body.size()) +
        "B, json " + std::to_string(j90.json.size()) + "B, " + std::to_string(n) +
        " datagrams)");
  }
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
    if (const char* dump = getenv("RBX_DUMP_JOIN8A"))
      if (FILE* f = fopen(dump, "wb"))
        fwrite(body8a.data(), 1, body8a.size(), f), fclose(f);
    const uint32_t place_id = uint32_t(place ? place : js.value("PlaceId", 0));
    const std::string key = rbxmsg::key8a(place_id, t2);
    const std::vector<uint8_t> msg = rbxmsg::sealedMessage(
        0x8A, body8a, reinterpret_cast<const uint8_t*>(key.data()), rbxmsg::kState8a);
    const int n = client.sendJoinMessage(msg.data(), msg.size());
    log("raknet: 0x8a ticket submitted (body " + std::to_string(body8a.size()) +
        "B, key " + key + ", " + std::to_string(n) + " datagrams)");
  }

  // The native sends 0x8f as its own datagram once the ticket has landed, not
  // in the same burst as 0x8a; pumping in between also lets an early challenge
  // arrive before the join is declared finished.
  client.pump(250);
  client.sendAcks();

  {
    const std::vector<uint8_t> msg = rbxmsg::build8f("");
    const int n = client.sendJoinMessage(msg.data(), msg.size());
    log("raknet: 0x8f sent (" + std::to_string(n) + " datagram)");
  }

  // The native's ordered stream continues with a 9-byte 0x9b at ord 5, in the
  // same breath as 0x8f — before the RCC has said anything but pads and pings
  // (captured: 9b f1a6f265 2a33dc31, then 9b 037e65d8 59bf666f in another
  // session). So it is NOT the answer to a server challenge (that form also
  // carries the challenge's u2, and a challenge would have to arrive first);
  // the native never sends a second 0x9b in a whole session that replicates
  // fine. It is a join step: two client-generated u32s that seed the RCC's
  // challenge/attestation for this connection.
  //
  // RBX_RAKNET_9B=<hex> replays a captured message verbatim (for diffing).
  {
    std::vector<uint8_t> msg;
    if (const char* hex = getenv("RBX_RAKNET_9B")) {
      msg = rbx::hexDecode(hex);
      if (msg.empty()) log("raknet: RBX_RAKNET_9B is not valid hex — not sent");
    }
    if (msg.empty()) {
      // splitmix64: independent u32s per join, like the native's PCG draws.
      uint64_t x = uint64_t(rng) * 0x9E3779B97F4A7C15ull +
                   uint64_t(std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count());
      const auto next = [&x]() {
        x += 0x9E3779B97F4A7C15ull;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return uint32_t(z >> 32);
      };
      const uint32_t a = next(), b = next();
      msg.resize(9);
      msg[0] = 0x9B;
      for (int i = 0; i < 4; ++i) msg[1 + i] = uint8_t(a >> (8 * i));
      for (int i = 0; i < 4; ++i) msg[5 + i] = uint8_t(b >> (8 * i));
    }
    if (!msg.empty()) {
      const int n = client.sendJoinMessage(msg.data(), msg.size());
      log("raknet: 0x9B join step sent (" + std::to_string(msg.size()) +
          "B, " + std::to_string(n) + " datagram) " +
          rbx::hexEncode(msg.data(), msg.size()));
    }
  }
}

} // namespace rbxclient
