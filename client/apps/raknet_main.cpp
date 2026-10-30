// raknet_main.cpp — RakNet transport, run as ./roblox --connection-type=RakNet.
//
//   roblox --connection-type=RakNet --join-json run/join.json [--seconds N]
//   roblox --connection-type=RakNet --place-id 1818 --roblosecurity <cookie>
//
// The run stays connected until Ctrl+C (SIGINT/SIGTERM) by default;
// --seconds N bounds it to N seconds for scripted/soak runs.
//
// Also the fallback the automatic selector uses when RbxTransport cannot
// prove a session (see roblox_main.cpp).
//
// Joins (or loads) the game, then speaks the native RakNet handshake to the
// Udmux endpoint: Req1 -> Reply1 -> Req2 (SessionCrypto: X25519 + BLAKE2b-512
// + ChaCha20-Poly1305, ticket blobs from the join script) -> Reply2, whose
// tag verifying against our derived rx key proves the server accepted the
// ticket and shares the session keys. --req2 sends the plain experimental
// Req2; --no-crypto disables the encrypted path.
//
// This file is CLI + config wiring only: the join script is read by
// join/target.cpp and the post-handshake bytes are sent by raknet/join_flow.cpp.
#include "entry.h"

#include "common/cli.h"
#include "common/hex.h"
#include "common/util.h"
#include "engine/live_runtime.h"
#include "join/join.h"
#include "join/target.h"
#include "raknet/challenge.h"
#include "raknet/join_flow.h"
#include "raknet/join_msg.h"
#include "raknet/raknet.h"
#include "view/view.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>
#include <vector>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#ifndef RBX_HAVE_RAKNET
#define RBX_HAVE_RAKNET 0
#endif

using nlohmann::json;

namespace {

// The live loop runs until SIGINT/SIGTERM (default) so `timeout` and Ctrl+C
// still print the summary; --seconds N bounds it instead.
volatile std::sig_atomic_t g_stop = 0;
void on_stop(int) { g_stop = 1; }

const rbx::cli::Spec& spec() {
  static const rbx::cli::Spec s = rbx::cli::Spec()
                                      .value("--place-id")
                                      .value("--roblosecurity")
                                      .value("--join-json")
                                      .value("--save-join")
                                      .value("--job-id")
                                      .value("--universe-id")
                                      .value("--token")
                                      .value("--cap-word")
                                      .value("--session-id")
                                      .value("--host")
                                      .value("--port")
                                      .value("--seconds")
                                      .value("--90-template")
                                      .value("--8a-template")
                                      .value("--pre-join")
                                      .value("--a7")
                                      .value("--token-sub")
                                      .flag("--skip-join")
                                      .flag("--req2")
                                      .flag("--no-crypto")
                                      // Display flags belong to RbxTransport.
                                      // RakNet has no renderer, so the unified
                                      // selector hands the same command line
                                      // to either transport.
                                      .flag("--headless")
                                      .flag("--headful")
                                      .value("--shot");
  return s;
}

} // namespace

namespace rbxclient {

int raknetMain(int argc, char** argv) {
  // Live runs are piped through timestamps/tee: without line buffering the
  // whole log lands in 4 KB chunks at exit and the timeline is lost.
  setvbuf(stdout, nullptr, _IOLBF, 0);
  const rbx::cli::Args args(argc, argv, spec());
  if (!args.ok()) {
    fprintf(stderr, "raknet: %s\n", args.error().c_str());
    return 2;
  }
  // Presentation is shared with RbxTransport: headful is the default (the
  // same collectBoxes/runViewerLive path), --headless runs the identical
  // Engine world with no GL, and --shot saves one frame then exits.
  const bool headless = args.has("--headless");
  const std::string shot = args.get("--shot");
  if (!shot.empty() && headless) {
    fprintf(stderr, "--shot needs --headful (omit --headless)\n");
    return 2;
  }

  const long long place = args.number("--place-id");
  const std::string cookie = args.get("--roblosecurity");
  const std::string join_path = args.get("--join-json");
  const std::string save_path = args.get("--save-join");
  const std::string job = args.get("--job-id");
  long universe = long(args.number("--universe-id"));
  std::string token_hex = args.get("--token");
  // 0 = stay connected until interrupted (the default, matching RbxTransport).
  const int seconds = int(args.number("--seconds", 0));
  // The handshake needs its own bound: with --seconds 0 the loop below must
  // still wait for Reply2, so an unbounded window falls back to 10s there.
  const int handshake_seconds = seconds > 0 ? seconds : 10;
  const int tok_sub = int(args.number("--token-sub"));
  const bool req2 = args.has("--req2");
  const bool no_crypto = args.has("--no-crypto");
  const bool skip_join = args.has("--skip-join");

  rbxclient::JoinFlowPaths flow;
  flow.join90 = args.get("--90-template", flow.join90);
  flow.ticket8a = args.get("--8a-template", flow.ticket8a);
  flow.pre_join = args.get("--pre-join", flow.pre_join);
  flow.a7 = args.get("--a7", flow.a7);

  if (token_hex.empty())
    if (const char* env = getenv("RBX_RUPP_TOKEN")) token_hex = env;

  rbx::JoinResult jr;
  std::string err;
  if (!join_path.empty()) {
    if (!rbx::loadJoin(join_path, jr, &err)) {
      fprintf(stderr, "loadJoin: %s\n", err.c_str());
      return 2;
    }
  } else {
    if (place == 0 || cookie.empty()) {
      fprintf(stderr,
              "--place-id and --roblosecurity (or --join-json) are required\n");
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
    if (jr.reply.is_object() && jr.reply.contains("jobId"))
      printf("join: job=%s (pin with --job-id)\n",
             jr.reply["jobId"].get<std::string>().c_str());
    // The 0x8a body carries the join script's SessionId *document*; the RCC
    // reads GameId out of it, so a stale or missing GameId is a silent join
    // refusal. Print what we are about to submit against the job we were
    // assigned.
    if (jr.join_script.is_object() && jr.join_script.contains("SessionId") &&
        jr.join_script["SessionId"].is_string()) {
      nlohmann::json sd = nlohmann::json::parse(
          jr.join_script["SessionId"].get<std::string>(), nullptr, false);
      if (!sd.is_discarded() && sd.is_object()) {
        const std::string gid = sd.value("GameId", std::string("(none)"));
        const std::string job = jr.reply.is_object()
                                    ? jr.reply.value("jobId", std::string())
                                    : std::string();
        printf("join: session-doc GameId=%s %s\n", gid.c_str(),
               (job.empty() || gid == job) ? "(matches job)"
                                           : "(MISMATCH: RCC will refuse)");
      } else {
        printf("join: session-doc SessionId is not a JSON object\n");
      }
    } else {
      printf("join: join script has no SessionId document (0x8a falls back "
             "to the template)\n");
    }
    if (!save_path.empty() && !rbx::saveJoin(save_path, jr, &err)) {
      fprintf(stderr, "saveJoin: %s\n", err.c_str());
      return 2;
    }
  }
  const json& js = jr.join_script;

  // The 0x90 document carries the account's AgeBracket / Gender / Platform /
  // Os / IsOriginalUser / OriginalAccountCreationTimestampMs. The join script
  // has none of them, so without this they keep the captured template's
  // values — another account's. The native gets them from the player
  // hydration service; fetch the same payload and merge it into the join
  // script so build90 picks them up.
  if (js.is_object() && js.contains("UserId")) {
    nlohmann::json hyd;
    std::string herr;
    if (rbx::fetchPlayerHydration(js["UserId"].get<long long>(), cookie, &hyd, &herr)) {
      for (const auto& kv : hyd.items()) jr.join_script[kv.key()] = kv.value();
      printf("raknet: player hydration: %zu fields (age %s, gender %s)\n",
             hyd.size(), hyd.value("AgeBracket", "?").c_str(),
             hyd.value("Gender", "?").c_str());
    } else {
      printf("raknet: player hydration unavailable (%s) — identity fields stay "
             "from the template\n", herr.c_str());
    }
  }

  rbx::JoinTarget target;
  if (!rbx::readJoinTarget(js, &target, &err)) {
    fprintf(stderr, "raknet: %s\n", err.c_str());
    return 2;
  }

  rbxclient::RaknetConfig cfg;
  cfg.host = args.get("--host", target.udmux_host);
  // Dial UdmuxEndpoints[0].Port (measured to equal the join script's
  // ServerConnections[0].Port, which is the port the native's RakNet client
  // connects to). RBX_DIAL_TLV=1 dials the NetStackPort that TLV2 advertises
  // instead — measured unreachable directly (no Reply1), so it stays off.
  {
    const char* dt = getenv("RBX_DIAL_TLV");
    const bool dial_tlv = dt && *dt && *dt != '0' && target.rcc_port != 0;
    cfg.port = uint16_t(args.number("--port", dial_tlv ? target.rcc_port
                                                       : target.udmux_port));
  }
  cfg.rcc_port = target.rcc_port;  // TLV2 on the handshake (NetStackPort)
  // TLV2 on the established session (subtype 2) is ServerConnections[0].Port,
  // the port the session really sends to; the RCC silently drops subtype-2
  // datagrams that advertise NetStackPort there.
  cfg.rcc_data_port = target.server_port ? target.server_port
                                         : target.udmux_port;
  if (!rbx::readIpv4(target.rcc_host, cfg.rcc_ip))
    fprintf(stderr, "raknet: ServerConnections[0].Address '%s' is not IPv4\n",
            target.rcc_host.c_str());
  cfg.client_ticket = target.client_ticket;
  if (!cfg.client_ticket.empty())
    printf("ticket: %zu chars\n", cfg.client_ticket.size());

  if (!token_hex.empty()) {
    if (!rbx::hexDecodeInto(token_hex, cfg.join_token, sizeof cfg.join_token)) {
      fprintf(stderr, "--token must be exactly 32 hex chars\n");
      return 2;
    }
  } else if (target.has_token) {
    std::memcpy(cfg.join_token, target.token, sizeof cfg.join_token);
  } else {
    fprintf(stderr,
            "no RUPP token: join script has no usable NetStackTokenValue and "
            "--token/RBX_RUPP_TOKEN unset\n");
    return 2;
  }
  cfg.send_req2 = req2;
  cfg.force_token_sub = uint8_t(tok_sub);

  // Session crypto inputs straight from the join script: the per-session
  // server key plus the two ticket blobs the native sends as early-auth.
  if (!no_crypto && js.contains("EphemeralEarlyPubKey")) {
    if (rbx::readSessionCrypto(js, &target, &err)) {
      std::memcpy(cfg.peer_pub, target.peer_pub, sizeof cfg.peer_pub);
      cfg.ticket_pre = target.ticket_pre;
      cfg.ticket_auth = target.ticket_auth;
      cfg.have_crypto = target.has_crypto;
      printf("crypto: peer key from EphemeralEarlyPubKey[%s]\n",
             target.peer_key_from.c_str());
      if (target.ticket_parts >= 4)
        printf("crypto: peer=%s ticket parts=%zu pre=%zu auth=%zu\n",
               cfg.have_crypto ? "ok" : "missing", target.ticket_parts,
               cfg.ticket_pre.size(), cfg.ticket_auth.size());
      else
        fprintf(stderr, "ClientTicket has %zu parts (want 5)\n",
                target.ticket_parts);
    } else {
      fprintf(stderr, "%s\n", err.c_str());
    }
  }

  if (args.has("--cap-word") &&
      !rbx::hexDecodeInto(args.get("--cap-word"), cfg.capability_word,
                          sizeof cfg.capability_word)) {
    fprintf(stderr, "--cap-word must be 16 hex chars\n");
    return 2;
  }
  if (args.has("--session-id") &&
      !rbx::hexDecodeInto(args.get("--session-id"), cfg.session_id,
                          sizeof cfg.session_id)) {
    fprintf(stderr, "--session-id must be 16 hex chars\n");
    return 2;
  }

  const auto log = [](const std::string& m) { printf("%s\n", m.c_str()); };
  rbxclient::RaknetClient client(log);

  // The replication parser is transport-independent: RakNet hands it the
  // same application messages (0x83 chains, 0x97 join data) that the
  // RbxTransport path feeds from its channel stream. LiveRuntime adds the
  // varint framing the stream transport supplies on its own.
  std::string job_id = job;
  if (job_id.empty() && js.is_object() && js.contains("JobId"))
    job_id = js.value("JobId", std::string());
  // The 0x9B challenge is solved against the session's GameId (the RCC job
  // id), exactly like the RbxTransport session; --job-id only overrides it.
  // Reaching it at all requires the 0xA7 queue that sendJoinFlow emits.
  std::string challenge_job = job_id;
  if (js.is_object() && js.contains("GameId"))
    challenge_job = js.value("GameId", challenge_job);
  std::unique_ptr<rbxclient::LiveRuntime> live;
  // Set once the RCC challenges us: the join is only "placed" after that, so
  // the retry loop below uses it as the success signal.
  bool challenge_seen = false;
  // The join sequence (0x90..0x8f..0x9b step) must finish before the challenge
  // answer goes out: the answer is itself ordered, so answering a challenge
  // that lands in the join flow's mid-point pump pushes 0x8f/0x9b to later
  // ordering indices. Measured once in 20: the server accepted that answer
  // but the initial 0x97 arrived without its record batches and the world
  // stayed empty. Hold the challenge until the sequence is complete, exactly
  // like the native's ordering (0x8f/0x9b first, answer last).
  bool join_flow_open = false;
  std::vector<uint8_t> deferred_challenge;
  if (!skip_join) {
    live = std::make_unique<rbxclient::LiveRuntime>(job_id, log);
    client.setMessageSink([&client, &live, &challenge_job, &challenge_seen,
                           &join_flow_open, &deferred_challenge,
                           log](const uint8_t* p, size_t n) {
      // 0x9B comes in both directions: the server form carries the challenge
      // blob, the 9-byte client form is our answer. Anything else is
      // replication and goes to the shared parser.
      if (n >= 9 && p[0] == 0x9B && rbxclient::isChallenge(p, n)) {
        challenge_seen = true;
        if (join_flow_open) {
          deferred_challenge.assign(p, p + n);
          log("raknet: 0x9B challenge during the join sequence — answering "
              "after the 0x9b join step");
          return;
        }
        rbxclient::answerChallenge(client, p, n, challenge_job, log);
        return;
      }
      // 0x84 snapshot-done SendMarker -> 0x04 ack (same marker id). The RCC's
      // DeserializedMarkerItem::process gates join progress on this ack; the
      // RbxTransport session has always answered it, the RakNet path never
      // did, so the server kept our Player out of the replicated world.
      if (n == 5 && p[0] == 0x84) {
        const uint32_t marker = uint32_t(p[1]) | (uint32_t(p[2]) << 8) |
                                (uint32_t(p[3]) << 16) |
                                (uint32_t(p[4]) << 24);
        const uint8_t ack[5] = {0x04, uint8_t(marker),
                                uint8_t(marker >> 8), uint8_t(marker >> 16),
                                uint8_t(marker >> 24)};
        if (client.sendJoinMessage(ack, sizeof ack, /*ordered=*/true))
          printf("raknet: 0x84 marker %u acked (0x04)\n", marker);
      }
      // 0x83/0x05 is the ID_DATA data-channel PING (sub 0x05 = PING, 0x06 =
      // PING_BACK per the RakNet reversal tables). The native answers a server
      // ping with 0x06; this client never did, and the RCC goes silent on the
      // data channel ~20 s in. Echo the body with the sub-id flipped.
      if (n >= 3 && p[0] == 0x83 && p[1] == 0x05) {
        std::vector<uint8_t> pong(p, p + n);
        pong[1] = 0x06;
        if (client.sendJoinMessage(pong.data(), pong.size()))
          printf("raknet: 0x83/0x05 PING answered with 0x83/0x06 (%zuB)\n",
                 pong.size());
      }
      // The 0x93 join reply is the server's verdict on the join: log its head
      // so a silent gate can be told apart from an accepted-but-idle session.
      if (n >= 2 && p[0] == 0x93)
        fprintf(stderr, "raknet: 0x93 head %s\n", rbx::hexPrefix(p, n, 48).c_str());
      if (live) live->feedMessage(p, n);
    });
  }
  // A placed join needs the RCC to issue its 0x9B challenge, and on this path
  // roughly one session in four never issues one — the RCC stops ACKing part
  // way through the 0x90 burst and the ordered stream stays blocked (re-sending
  // the join in the *same* session cannot help: everything queues behind the
  // same hole, measured). A fresh session gets a fresh RCC session, so after a
  // miss we drop the connection and redo the handshake; three attempts take a
  // ~75% per-attempt rate to ~99%.
  // The native's post-join stream, byte-for-byte from its live 1818 capture
  // (2026-10-30, /tmp/nat_live, TX decrypted with tools/raknet_rx_decode.py
  // --tx): 0xA6 route frames 01/02/03 once, then 04 forever, both stepIds
  // advancing by 4 per message (0x14c, 0x150, 0x154, 0x158, ...). Count=1 is
  // the route opener and has the short `01 <step>` form; count>=2 carry the
  // build's 11-byte tag (`10 03 00 64 d0 b7 e6 d7 ff 03 e0`). The old count=2
  // template was a stale capture with no tag and only 5 header bytes, so every
  // second message of a session was 10 bytes shorter than the native's.
  struct A6Frame {
    const char* hex;
    size_t step_off;
  };
  static const A6Frame kA6[4] = {
      {"a6010100014c0100004c0100000000", 5},
      {"a602010010030064d0b7e6d7ff03e050010000500100000000040400", 15},
      {"a603010010030064d0b7e6d7ff03e054010000540100000000040400040400", 15},
      {"a604010010030064d0b7e6d7ff03e058010000580100000000040400040400040400", 15},
  };
  auto a6Frame = [](int idx, uint32_t step) {
    std::vector<uint8_t> v = rbx::hexDecode(kA6[idx].hex);
    const size_t o = kA6[idx].step_off;
    if (v.size() >= o + 8) {
      for (int k = 0; k < 4; ++k) {
        v[o + k] = uint8_t((step >> (8 * k)) & 0xFF);
        v[o + 4 + k] = uint8_t((step >> (8 * k)) & 0xFF);
      }
    }
    return v;
  };
  bool session_up = false;
  for (int attempt = 0; attempt < 3 && !session_up && !g_stop; ++attempt) {
  if (attempt > 0) {
    printf("raknet: rejoining with a fresh session (attempt %d of 3)\n", attempt + 1);
    client.resetSession();
    challenge_seen = false;
    // Reply2 is bound to the join ticket: replaying the previous one gets
    // Reply1 and then silence (measured). Fetch a new join script and refresh
    // every session-scoped input from it; the endpoint does not change.
    if (join_path.empty()) {
      rbx::JoinResult renewed;
      if (!rbx::joinGame(place, cookie, job, 0, universe, renewed, &err)) {
        fprintf(stderr, "joinGame (retry): %s\n", err.c_str());
        return 2;
      }
      jr = std::move(renewed);
      // A fresh join can land on a different RCC job: re-derive the ids the
      // 0x9B solver uses. A stale GameId silently computes the wrong answer
      // for the new session (replication then never starts).
      job_id = job;
      if (job_id.empty() && js.is_object() && js.contains("JobId"))
        job_id = js.value("JobId", std::string());
      challenge_job = job_id;
      if (js.is_object() && js.contains("GameId"))
        challenge_job = js.value("GameId", challenge_job);
      rbx::JoinTarget fresh;
      if (rbx::readJoinTarget(js, &fresh, &err)) {
        cfg.client_ticket = fresh.client_ticket;
        if (token_hex.empty() && fresh.has_token)
          std::memcpy(cfg.join_token, fresh.token, sizeof cfg.join_token);
        if (!no_crypto && js.contains("EphemeralEarlyPubKey") &&
            rbx::readSessionCrypto(js, &fresh, &err)) {
          std::memcpy(cfg.peer_pub, fresh.peer_pub, sizeof cfg.peer_pub);
          cfg.ticket_pre = fresh.ticket_pre;
          cfg.ticket_auth = fresh.ticket_auth;
          cfg.have_crypto = fresh.has_crypto;
        }
      } else {
        fprintf(stderr, "readJoinTarget (retry): %s\n", err.c_str());
      }
    }
  }
  if (!client.connect(cfg, &err)) {
    fprintf(stderr, "raknet connect: %s\n", err.c_str());
    return 1;
  }
  std::signal(SIGINT, on_stop);
  std::signal(SIGTERM, on_stop);
  for (int i = 0; i < handshake_seconds && !g_stop && !client.reply2_ok(); ++i)
    client.pump(1000);

  // Once the datagram keys are installed, open the RakNet session: the first
  // encrypted datagram is ID_CONNECTION_REQUEST (0x09), exactly like the
  // native's first post-handshake frame.
  if (client.reply2_ok() && client.data_ready()) {
    client.sendConnectionRequest();
    // RBX_FAST_JOIN=1: go straight to the join ladder without the 600 ms
    // window the RCC's 0x10 usually lands in. The native writes 0x13 and the
    // 0x90 join body back-to-back with 0x09 (its 0x10 is read long after the
    // join is already in flight), so a pause here is not native behaviour.
    {
      const char* fj = getenv("RBX_FAST_JOIN");
      client.pump(fj && *fj && *fj != '0' ? 30 : 600);
    }
    // ACK the ACCEPTED so the server stops retransmitting, then open the
    // session like the native does (ID_NEW_INCOMING_CONNECTION).
    client.sendAcks();
    client.sendNewIncoming();
    // Ordering (native RX capture, /tmp/joinrx rkc.log): the RCC answers the
    // client's 0x90 with its 0x93 network-settings push and the 0x9B challenge
    // within two datagrams, and the native has its 0x90 in flight *before* the
    // 0x93 lands. So do not gate the join on 0x93 — send the sequencer straight
    // away, exactly like the native. RBX_WAIT_93=1 restores the old wait for
    // A/B runs.
    if (const char* w = getenv("RBX_WAIT_93")) {
      if (*w && *w != '0') {
        for (int i = 0; i < 50 && !client.netSettingsSeen(); ++i) {
          client.pump(50);
          client.sendAcks();
        }
      }
    }
    printf("raknet: %s before join\n",
           client.netSettingsSeen() ? "network settings (0x93) received"
                                    : "0x93 not seen, joining anyway");
    if (skip_join) {
      printf("raknet: --skip-join: join body/ticket not sent\n");
      session_up = true;
    } else {
      join_flow_open = true;
      sendJoinFlow(client, js, jr, place, flow, log);
      // The native's 19-byte 0x83/0x07 control-stream opener goes out right
      // after the join ladder, *before* the RCC issues its 0x9B challenge
      // (decrypted TX, /tmp/nat_cmp: it sits between the 0x8f join message and
      // the first 0xA6 input frames). Ours only sent it once `live->has_world()`
      // was true, which needs the 0x97 join data — and the join data needs the
      // challenge first, so the opener was never sent on a fresh session.
      // RBX_NO_OPENER=1 skips it for A/B runs.
      {
        const char* no_op = getenv("RBX_NO_OPENER");
        if (!(no_op && *no_op && *no_op != '0')) {
          const auto ev = rbx::hexDecode("830701b612050013100000e04023b401000000");
          if (!ev.empty()) {
            const int n = client.sendJoinMessage(ev.data(), ev.size());
            printf("raknet: 0x83/0x07 control opener sent (%zuB, %d datagrams)\n",
                   ev.size(), n);
          }
        }
      }
      // Native ordering at the same job (decrypted TX, /tmp/nat_diff2):
      //   0x13@0 0x90@1 0x92@2 0x8a@3 0x8f@4 0x9b@5(answer) 0x83/04@6
      // Ours used to push the 318B player-attributes message at 5, so the
      // answer landed at 6 while the native's sits at 5. The RCC's challenge
      // window is the whole join burst, so hold the flow open just long enough
      // for the 0x9B to land, answer it at 5, then push hydration at 6.
      // RBX_ACK_93=1: answer the RCC's 0x93 settings push with the 0x8f
      // settings ack once it lands (instead of the single 0x8f the join burst
      // sends before 0x93). The 0x8f is only 2 bytes and looks like an ack of
      // the settings document; if it is, the pre-0x93 copy leaves the RCC
      // waiting and it never issues the 0x9B.
      if (const char* a93 = getenv("RBX_ACK_93")) {
        if (*a93 && *a93 != '0') {
          for (int i = 0; i < 40 && !client.netSettingsSeen() && !g_stop; ++i) {
            client.pump(25);
            client.sendAcks();
          }
          if (client.netSettingsSeen()) {
            const std::vector<uint8_t> ack = rbxmsg::build8f("");
            client.sendJoinMessage(ack.data(), ack.size());
            client.sendAcks();
            printf("raknet: 0x8f settings ack resent after 0x93 (%zuB)\n",
                   ack.size());
          }
        }
      }
      const char* jw = getenv("RBX_JOIN_WAIT");
      const bool wait_for_9b = !(jw && *jw == '0');
      if (wait_for_9b)
        for (int i = 0; i < 40 && deferred_challenge.empty() && !g_stop; ++i) {
          client.pump(25);
          client.sendAcks();
        }
      if (!deferred_challenge.empty()) {
        rbxclient::answerChallenge(client, deferred_challenge.data(),
                                   deferred_challenge.size(), challenge_job,
                                   log);
        deferred_challenge.clear();
      }
      join_flow_open = false;
      // The native pushes its signed player attributes (0x83/0x04) right after
      // the join: lastPerformed/userId/ageBracket/... JSON plus the hydration
      // service's base64 signature. This used to live in the input tick, where
      // its env gate never fired (no run ever logged it); sending it on the
      // join path is both what the native does and observable.
      if (js.is_object() && js.contains("UserId")) {
        std::string hj, hs, herr;
        if (rbx::fetchPlayerHydrationRaw(js["UserId"].get<long long>(), cookie,
                                        &hj, &hs, &herr)) {
          const auto msg = rbxmsg::buildHydration(hj, hs);
          const int n = client.sendJoinMessage(msg.data(), msg.size());
          printf("raknet: 0x83/0x04 player attributes sent (json %zuB, "
                 "sig %zuB, %d datagrams)\n", hj.size(), hs.size(), n);
        } else {
          printf("raknet: player hydration push skipped (%s)\n", herr.c_str());
        }
      }
      // Go live immediately, exactly like the native: no wait for a 0x9B
      // challenge (place 1818 never sends one to a RakNet join) and no wait
      // for 0x97. The message sink keeps answering a challenge if one lands
      // mid-session, and LiveRuntime builds the world when the data arrives.
      session_up = !g_stop;
      if (!challenge_seen && !(live && live->has_world()))
        printf("raknet: no 0x9B challenge yet — running the session (native "
               "does not wait for one)\n");
    }
  }
  } // attempt loop

  if (session_up) {
    const auto run_start = std::chrono::steady_clock::now();
    // Native input stream, replayed in the native's order and cadence: 01,
    // 02, 03 once, then 04 at ~30 Hz, stepIds +4 per message. A one-shot
    // burst was not enough (the RCC stops receiving route ACKs and never
    // starts replicating); the native's live capture keeps this running for
    // the whole session.
    auto next_input = std::chrono::steady_clock::now();
    uint32_t a6_step = 0x14c;
    int a6_sent = 0;
    int a6_stage = 0;
    auto next_clock_tick = std::chrono::steady_clock::now();
    auto next_ack83 = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(2500);
    auto next_ack83_pair = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(5000);
    uint32_t clock_step = 0x270; // native's first 0x83/23 value, +8 per tick
    bool hb_started = false;
    // Captured native 0x83/0x05 ack pair (dgram 67, 1818, 2026-10-30).
    static const char* kAck83Rel =
        "830516dc10000000000000dc10000000000000b4f331429861"
        "2842000000000000000000db100000a03a8344230c02000000";
    static const char* kAck83Unrel =
        "830516dc10000000000000dc10000000000000b4f3314"
        "298612842000000000000000001db100000a03a834400";
    // Liveness: the RCC never sends a disconnect when it ends a session, so a
    // silent socket must be reported instead of looking like a quiet session.
    long long quiet_announced = 0;
    bool char_note = false;
    // Input cadence, measured from the native's decrypted 1818 TX stream
    // (/tmp/nat_live, 34.4 s window): 457 0xA6 messages (13.3/s), 47 0xA1
    // camera frames (1.4/s), 203 0x83/23 input clocks (5.9/s), 14 45-byte +
    // 7 50-byte 0x83/05 physics acks (0.4/s + 0.2/s). This client used to send
    // one 0xA6 *and* one 0xA1 per 33 ms tick — a 20x flood of both — which is
    // the remaining difference from the native in the steady state.
    int input_ms = 75;
    if (const char* im = getenv("RBX_INPUT_MS")) input_ms = std::max(10, atoi(im));
    auto next_camera = std::chrono::steady_clock::now();
    auto pumpInput = [&]() {
      const auto now = std::chrono::steady_clock::now();
      if (now < next_input) return;
      next_input += std::chrono::milliseconds(input_ms);
      if (now > next_input + std::chrono::milliseconds(500))
        next_input = now + std::chrono::milliseconds(input_ms);
      const int idx = a6_stage < 3 ? a6_stage : 3;
      const auto frame = a6Frame(idx, a6_step);
      if (!frame.empty() && client.sendUnreliable(frame.data(), frame.size()) &&
          ++a6_sent == 1)
        printf("raknet: 0xA6 input stream started (native route frames)\n");
      // Native 0xA1 camera frame: `a1 <u32le clock>` + a 39-byte body. In the
      // 34 s capture every frame carries the same body (only the clock
      // changes), so the session-relative fields are taken from that stream
      // verbatim. The native sends one per ~730 ms, not one per input tick.
      {
        static const uint8_t kA1Tail[39] = {
            0x01, 0x00, 0x00, 0x00, 0x43, 0x62, 0xf9, 0xff, 0x01, 0x00,
            0x00, 0x00, 0x01, 0xb6, 0x12, 0x05, 0x00, 0x7e, 0x01, 0x09,
            0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08,
            0x00, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x80, 0x3f};
        if (now >= next_camera) {
          next_camera = now + std::chrono::milliseconds(730);
          uint8_t a1[44];
          a1[0] = 0xA1;
          for (int k = 0; k < 4; ++k)
            a1[1 + k] = uint8_t((a6_step >> (8 * k)) & 0xff);
          std::memcpy(a1 + 5, kA1Tail, sizeof kA1Tail);
          client.sendUnreliable(a1, sizeof a1);
        }
      }
      // The native pushes its signed player attributes (0x83/0x04) on the
      // join path (above), not per input tick.
      if (live && live->has_world()) {
        // The native's post-join control stream, decrypted from /tmp/nat_live
        // (2026-10-30): a 19-byte 0x83/0x07 opener, 7-byte 0x83/0x23 input
        // clocks at ~6/s, and a 0x83/0x05 physics ack in two forms (45 B
        // unreliable every ~2.5 s, plus a 50 B reliable copy every ~5 s).
        if (!hb_started) {
          hb_started = true;
          const auto ev = rbx::hexDecode(
              "830701b612050013100000e04023b401000000");
          client.sendJoinMessage(ev.data(), ev.size());
        } else {
          if (now >= next_clock_tick) {
            next_clock_tick = now + std::chrono::milliseconds(170);
            clock_step += 8; // native's clock advances +8 per tick
            uint8_t hb[7] = {0x83, 0x23,
                             uint8_t(clock_step & 0xff),
                             uint8_t((clock_step >> 8) & 0xff),
                             uint8_t((clock_step >> 16) & 0xff), 0, 0};
            client.sendJoinMessage(hb, sizeof hb);
          }
          if (now >= next_ack83) {
            const bool pair = now >= next_ack83_pair;
            next_ack83 = now + std::chrono::milliseconds(2500);
            if (pair) next_ack83_pair = now + std::chrono::milliseconds(5000);
            const auto unrel = rbx::hexDecode(kAck83Unrel);
            if (!unrel.empty())
              client.sendUnreliable(unrel.data(), unrel.size());
            if (pair) {
              const auto rel = rbx::hexDecode(kAck83Rel);
              if (!rel.empty())
                client.sendJoinMessage(rel.data(), rel.size(), /*ordered=*/true);
            }
          }
        }
      }
      // The avatar is not part of the join snapshot: the RCC spawns the
      // character only after the client sends real ControlState input (the
      // native's builder rejects nActions == 0). These route frames are
      // keepalives, so say so once instead of leaving a character-less world
      // unexplained.
      if (!char_note && live && live->has_world()) {
        const auto e = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now() - run_start)
                           .count();
        if (e >= 12) {
          char_note = true;
          printf("raknet: no character replicated after %llds — the RCC spawns "
                 "the avatar only for real ControlState input (nActions>=1); "
                 "this session sends route/keepalive frames\n",
                 (long long)e);
        }
      }
      a6_step += 4;
      ++a6_stage;
    };
    auto out_of_time = [&]() {
      if (seconds <= 0) return false;
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - run_start)
                               .count();
      return elapsed >= int64_t(seconds) * 1000;
    };
    if (headless) {
      if (seconds > 0)
        printf("raknet: connected; running for %ds (--seconds, headless)\n",
               seconds);
      else
        printf("raknet: connected; running until interrupted (Ctrl+C, "
               "headless)\n");
      while (!g_stop && !out_of_time()) {
        client.pump(30);
        const long long quiet = client.ms_since_rx();
        if (quiet > 6000 && quiet - quiet_announced > 6000) {
          quiet_announced = quiet;
          printf("raknet: NO SERVER DATAGRAM for %llds — the RCC has sent "
                 "nothing (it never sends a disconnect; this is either a "
                 "reaped session or an idle world)\n",
                 quiet / 1000);
        }
        pumpInput();
        if (live) live->pump(0.03);
        // ACK every ~250 ms; the native does not ACK every input tick.
        static int ack_slice = 0;
        if (++ack_slice % 8 == 0) client.sendAcks();
      }
    } else {
      printf("raknet: connected; %s (headful — close the window or Ctrl+C)\n",
             seconds > 0 ? "bounded run" : "running until interrupted");
      auto boxes = [&]() -> std::vector<rbxclient::ViewBox> {
        if (live && live->has_world())
          return rbxclient::collectBoxes(live->root());
        return {};
      };
      // The render loop owns the session: one non-blocking-ish pump per frame
      // keeps ACKs flowing and feeds the same parser the headless loop feeds.
      auto tick = [&](double dt) -> bool {
        if (g_stop) return false;
        // Drain everything queued, not a time slice: the join snapshot is
        // ~1 MB in one burst, and anything left in the socket buffer while
        // the frame is drawn is dropped by the OS.
        client.pump(0);
        client.sendAcks();
        const long long quiet = client.ms_since_rx();
        if (quiet > 6000 && quiet - quiet_announced > 6000) {
          quiet_announced = quiet;
          printf("raknet: NO SERVER DATAGRAM for %llds — the RCC has sent "
                 "nothing (it never sends a disconnect; this is either a "
                 "reaped session or an idle world)\n",
                 quiet / 1000);
        }
        pumpInput();
        if (live) live->pump(dt);
        return !g_stop && !out_of_time();
      };
      std::string verr;
      const int vrc = rbxclient::runViewerLive(boxes, tick, shot, &verr);
      if (vrc != 0) {
        fprintf(stderr, "raknet: view: %s\n", verr.c_str());
        return vrc;
      }
      g_stop = 1;
    }
    printf("raknet: %s\n", g_stop ? "interrupted — closing session"
                                   : "run window complete");
    if (live) {
      printf("raknet: live: %s\n", live->stats().c_str());
      if (live->has_world())
        printf("raknet: classes: %s\n", live->class_stats().c_str());
      if (!live->has_world())
        printf("raknet: no replication snapshot yet (0x97 never arrived); "
               "Transport framed, world not applied\n");
    }
    if (const uint8_t* dg = client.last_datagram()) {
      printf("raknet: last server datagram %zuB: %s\n", client.last_datagram_len(),
             rbx::hexPrefix(dg, client.last_datagram_len(), 48).c_str());
    }
  }
  printf("raknet: result stage=%d mtu=%u guid=%llu session_token=%s "
         "reply2=%s data=%s datagrams=%llu/%llu acks=%llu srv_acks=%llu dups=%llu "
         "pkts=%llu payloads=%llu bytes=%llu app=%llu split=%llu rtx=%llu\n",
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
         (unsigned long long)client.bytes_rx(),
         (unsigned long long)client.app_messages(),
         (unsigned long long)client.app_split_messages());
  // A verified handshake without a placed session is still a failed join;
  // report it instead of letting scripts read a challenge-less run as success.
  return (client.reply2_ok() && session_up) ? 0 : 1;
}

} // namespace rbxclient
