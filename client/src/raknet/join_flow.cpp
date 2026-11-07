// join_flow.cpp — see join_flow.h.
#include "raknet/join_flow.h"

#include "common/hex.h"
#include "common/util.h"
#include "raknet/join_msg.h"
#include "raknet/raknet.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace rbxclient {

void sendJoinFlow(RaknetClient& client, const nlohmann::json& js,
                  const rbx::JoinResult& jr, long long place,
                  const JoinFlowPaths& paths,
                  const std::function<void(const std::string&)>& say) {
  const auto log = say ? say : [](const std::string& m) { printf("%s\n", m.c_str()); };
  std::string err;

  // Templates are repo-relative (run/...). Resolve them so launching from
  // build/client works too: a missing 0x90 template silently skips the join,
  // and a server that never sees a join never issues the 0x9B challenge.
  JoinFlowPaths p = paths;
  p.join90 = rbx::resolveDataPath(p.join90);
  p.ticket8a = rbx::resolveDataPath(p.ticket8a);
  p.pre_join = rbx::resolveDataPath(p.pre_join);
  if (!p.a7.empty() && p.a7 != "skip") p.a7 = rbx::resolveDataPath(p.a7);

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
  // The native's live 1818 stream (2026-10-29) has exactly ONE pad before the
  // join body: a 1160-byte 0x00 split (2 fragments, sequenced, split id 0),
  // straight after 0x13 and before 0x90. The 0x03 pads come later, as answers
  // to the RCC's own pads. The old four-pad burst was the thing that stopped
  // the challenge, not the exchange itself.
  const std::vector<uint8_t> pre = rbx::readFileVec(p.pre_join);
  const char* want_pre = getenv("RBX_PRE_PAD");
  // Off by default: with the pad in front of the join the RCC answers 0x93 and
  // then only pads at us, so the challenge never comes (measured). Native sends
  // one, so this stays available for A/B runs (RBX_PRE_PAD=1).
  const bool send_pre = pre.size() >= 1160 && want_pre && *want_pre &&
                        *want_pre != '0';
  if (send_pre) {
    static const uint8_t kPadIds[1] = {0x00};
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

  // 0xA7 script-hash queue: `[a7][count u16le][count x 16B]`.
  //
  // Already built, but NOT part of the native's RakNet join ladder. The
  // decrypted live capture (/tmp/nat_cmp, 2026-10-30) has no 0xA7 anywhere in
  // the client stream, and sending it at ordering slot 1 moves 0x90 to slot 2:
  // the RCC then answers the join with pads only and never sends 0x93. With
  // the queue omitted, 0x90 sits at slot 1 exactly like the native, 0x93
  // arrives one round trip later, and the 0x9B step below gets challenged.
  // --a7 FILE restores a captured queue for A/B runs.
  std::vector<uint8_t> a7;
  if (p.a7 == "skip" || p.a7.empty()) {
    log("raknet: 0xA7 skipped (native RakNet join carries none)");
  } else if (!p.a7.empty()) {
    a7 = rbx::readFileVec(p.a7);
  } else {
    // Byte-identical to the queue the native submitted to 1818 (35B, count 2).
    static const uint8_t kNativeA7[35] = {
        0xA7, 0x02, 0x00, 0x87, 0x72, 0x4E, 0x96, 0xF7, 0x4F, 0xC3, 0x71, 0x45,
        0x0B, 0x71, 0x0A, 0x97, 0xFE, 0x1F, 0x10, 0x50, 0xFE, 0x51, 0x44, 0xC4,
        0x99, 0x8A, 0x9B, 0xDD, 0xC2, 0x00, 0x86, 0x69, 0x95, 0x60, 0x66};
    a7.assign(kNativeA7, kNativeA7 + sizeof kNativeA7);
  }
  if (!a7.empty()) {
    p.a7 = p.a7.empty() ? "(native capture)" : p.a7;
    if (a7.size() >= 3 && a7[0] == 0xA7) {
      const size_t count = size_t(a7[1] | (a7[2] << 8));
      if (a7.size() != 3 + count * 16) {
        log("raknet: 0xA7 template " + p.a7 + " is 3+" +
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
      log("raknet: 0xA7 template " + p.a7 + " missing or malformed — not sent");
    }
  }

  // The native's pre-join probe, byte-for-byte from its decrypted 1818 TX
  // stream (/tmp/nat_diff2, 2026-10-30):
  //
  //   0x00  1160 B   1061 + 99 split, all zero except a u16 BE counter at [7:9]
  //   0x53    9 B    id + the same u16 BE counter
  //   0x52    9 B
  //   0x51    9 B
  //
  // An earlier form used 1146 B and only 0x51; the two extra pings and the
  // 14-byte-longer pad are what the native actually writes.
  //   0x90  join body
  //
  // The RCC answers a join that arrives *after* this probe; without it the
  // same body gets 0x93 and then nothing at all (8/8 runs at one job id, while
  // the native got its 0x9B from that same session). The pad used to come from
  // run/pre_join.bin, which is not in the tree, so this exchange never ran.
  {
    const uint16_t seq = uint16_t(std::chrono::duration_cast<
                                      std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now()
                                          .time_since_epoch())
                                      .count() &
                                  0xFFFFu);
    // RBX_PAD_OLD=1: the pre-diff shape (1146 B pad, single 0x51) for A/B runs.
    const char* pad_old = getenv("RBX_PAD_OLD");
    const bool old_pad = pad_old && *pad_old && *pad_old != '0';
    // The native runs the MTU ladder TWICE before the join body (decrypted TX,
    // /tmp/nat_cmp dg2-6): pad 0x00 ctr C + pings 53/52/51 ctr C, then the same
    // pad with ctr C+9 and its pings. One pad leaves the RCC acknowledging only
    // the first eight datagrams and never reading the 0x90 behind them.
    const int rounds = old_pad ? 1 : 2;
    for (int r = 0; r < rounds; ++r) {
      const uint16_t ctr = uint16_t(seq + (r ? 9 : 0));
      std::vector<uint8_t> pad(old_pad ? 1146 : 1160, 0);
      pad[0] = 0x00;
      pad[7] = uint8_t(ctr >> 8);
      pad[8] = uint8_t(ctr);
      const int n = client.sendJoinMessage(pad.data(), pad.size(), /*ordered=*/false);
      log("raknet: pre-join pad 0x00 sent (" + std::to_string(pad.size()) +
          "B, seq " + std::to_string(ctr) + ", " + std::to_string(n) +
          " datagrams)");
      // 0x53, 0x52, 0x51 in the native's order, all carrying the same counter.
      // They are UNRELIABLE on the wire (native TX kind 0, server's own pings
      // arrive rel 0x00); sending them reliable gives them message numbers the
      // RCC's ping path never sees, so its MTU ladder never closes.
      for (uint8_t id : {0x53, 0x52, 0x51}) {
        if (old_pad && id != 0x51) continue;
        const std::vector<uint8_t> ping = {id, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                           uint8_t(ctr >> 8), uint8_t(ctr)};
        client.sendUnreliable(ping.data(), ping.size());
      }
      log("raknet: pre-join pings sent (" + std::string(old_pad ? "0x51 only" :
                                                                 "0x53/0x52/0x51") +
          ", 9B each)");
    }
  }

  const std::vector<uint8_t> tmpl = rbx::readFileVec(p.join90);
  if (tmpl.size() < 64) {
    log("raknet: 0x90 template " + p.join90 +
        " missing (searched CWD, binary dir, $BOBLOX_DATA_DIR) — join body held back");
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
  // RBX_JOIN_EXTRA=1 restores the 0x92/0x8f/0x9b messages this flow used to
  // send after the join body. The native's RakNet stream has none of them:
  // its ordered channel is 0x13 (ord 0), 0x90 (ord 1), 0x8a (its ticket), and
  // then only 0x00/0x03 MTU pads (sequenced). 0x92/0x8f are RbxTransport
  // (QUIC) join-burst messages carried over here.
  //
  // Measured 2026-10-29 against place 1818, six runs each: without the burst
  // the RCC never issues the 0x9B challenge (0/6), with it the challenge
  // arrives every run (6/6). The native-shaped ordered stream alone is not
  // enough for *our* join body, so the burst is the default; RBX_JOIN_EXTRA=0
  // restores the native-shaped stream for A/B runs.
  // RBX_JOIN_EXTRA also accepts a subset ("92", "8f", "92,8f") so a place
  // that answers the two-message burst differently can be bisected without a
  // rebuild. "0" disables both.
  const char* extra = getenv("RBX_JOIN_EXTRA");
  const bool extra_all = !(extra && *extra == '0');
  const bool extra_92 = extra_all && (!extra || !*extra || std::strstr(extra, "92"));
  const bool extra_8f = extra_all && (!extra || !*extra || std::strstr(extra, "8f"));
  const char* skip9b = getenv("RBX_JOIN_STEP_9B");
  if (extra_92) {
    const std::vector<uint8_t> msg = rbxmsg::build92(rng, v9);
    const int n = client.sendJoinMessage(msg.data(), msg.size());
    log("raknet: 0x92 sent (" + std::to_string(n) + " datagram)");
  } else {
    // Native leaves ordering index 2 empty and puts the ticket at 3.
    client.reserveOrdering();
  }

  const uint64_t uid = js.value("UserId", 0ULL);
  const std::string ticket = js.value("ClientTicket", std::string());
  const std::vector<uint8_t> t8a = rbx::readFileVec(p.ticket8a);
  // RBX_NO_8A=1 drops the ticket submission entirely: it isolates a rejected
  // ticket from a rejected join body (the RCC's reset arrives on the join step,
  // after both have been sent).
  const char* no8a = getenv("RBX_NO_8A");
  if (no8a && *no8a && *no8a != '0') {
    log("raknet: 0x8a skipped (RBX_NO_8A)");
  } else if (uid == 0 || ticket.empty()) {
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
    std::string key = rbxmsg::key8a(place_id, t2);
    // RBX_KEY8A_STR=<16 chars>: override the ticket seal key. The derivation is
    // a 16-char truncation of (%d placeId, constant, tail2, constant); this
    // probe exists so a place that resets (0x15/0x0106) can be tested against a
    // candidate key without a rebuild.
    if (const char* ko = getenv("RBX_KEY8A_STR")) {
      if (std::strlen(ko) == 16) {
        key.assign(ko, 16);
        log("raknet: 0x8a key overridden (RBX_KEY8A_STR)");
      } else {
        log("raknet: RBX_KEY8A_STR must be 16 chars — ignored");
      }
    }
    // RBX_JOIN8A_RAW=<file>: submit a captured 0x8a message (id + sealed body)
    // verbatim, so a rejected ticket can be told apart from a rejected join.
    std::vector<uint8_t> msg;
    if (const char* raw8a = getenv("RBX_JOIN8A_RAW")) {
      msg = rbx::readFileVec(raw8a);
      if (msg.size() < 32 || msg[0] != 0x8A) {
        log("raknet: RBX_JOIN8A_RAW is not a 0x8a message — not sent");
        msg.clear();
      }
    }
    if (msg.empty())
      msg = rbxmsg::sealedMessage(0x8A, body8a,
                                  reinterpret_cast<const uint8_t*>(key.data()),
                                  rbxmsg::kState8a);
    const int n = client.sendJoinMessage(msg.data(), msg.size());
    log("raknet: 0x8a ticket submitted (body " + std::to_string(body8a.size()) +
        "B, key " + key + ", " + std::to_string(n) + " datagrams)");
  }

  // Native sequencing (live 1818 capture): 0x90 (ord 1), 0x92 (ord 2), 0x8a
  // (ord 3), 0x8f (ord 4) and the 0x9b join step (ord 5) leave in one burst,
  // back-to-back, with no pause and no server reply in between. The old
  // 250 ms pump here pushed 0x8f/0x9b a quarter second behind the ticket.
  client.sendAcks();

  if (extra_8f) {
    const std::vector<uint8_t> msg = rbxmsg::build8f("");
    const int n = client.sendJoinMessage(msg.data(), msg.size());
    log("raknet: 0x8f sent (" + std::to_string(n) + " datagram)");
  } else {
    // Native leaves ordering index 4 empty; its next ordered message is the
    // 9-byte 0x9b at index 5.
    client.reserveOrdering();
  }

  // An optional 9-byte 0x9B. OFF by default.
  //
  // The native does send one at ordering slot 5 (tx r268: 9b df7f58dd 90890f0c),
  // but its answer to a real challenge has the same 9-byte shape, so sending it
  // speculatively looks like a wrong answer: every run with it enabled got the
  // 0x93 settings push and then silence, while the runs that never sent it
  // (the last known-good joins, /tmp/ab_obj_2.log) received the 1941-byte 0x9B
  // challenge one round trip after 0x93. RBX_JOIN_STEP_9B=1 restores it for
  // A/B runs; RBX_RAKNET_9B=<hex> replays a captured message verbatim.
  const bool send_step_9b = skip9b && *skip9b && *skip9b != '0';
  if (send_step_9b) {
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
