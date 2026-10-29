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
#include "engine/live_runtime.h"
#include "join/join.h"
#include "join/target.h"
#include "raknet/challenge.h"
#include "raknet/join_flow.h"
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
    if (!save_path.empty() && !rbx::saveJoin(save_path, jr, &err)) {
      fprintf(stderr, "saveJoin: %s\n", err.c_str());
      return 2;
    }
  }
  const json& js = jr.join_script;

  rbx::JoinTarget target;
  if (!rbx::readJoinTarget(js, &target, &err)) {
    fprintf(stderr, "raknet: %s\n", err.c_str());
    return 2;
  }

  rbxclient::RaknetConfig cfg;
  cfg.host = args.get("--host", target.udmux_host);
  cfg.port = uint16_t(args.number("--port", target.udmux_port));
  cfg.rcc_port = target.rcc_port;
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
    client.pump(600);
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
      join_flow_open = false;
      if (!deferred_challenge.empty()) {
        rbxclient::answerChallenge(client, deferred_challenge.data(),
                                   deferred_challenge.size(), challenge_job,
                                   log);
        deferred_challenge.clear();
      }
      int waited = 0;
      while (waited < 3000 && !g_stop && !challenge_seen &&
             !(live && live->has_world())) {
        client.pump(200);
        client.sendAcks();
        if (live) live->pump(0.2);
        waited += 200;
      }
      session_up = challenge_seen || (live && live->has_world());
      if (!session_up)
        printf("raknet: no 0x9B challenge in 3s with this session\n");
    }
  }
  } // attempt loop

  if (session_up) {
    const auto run_start = std::chrono::steady_clock::now();
    // Upstream input. The native interleaves 0xA6 (unreliable) with 0x83
    // replication on the established session; A6 00 is the framing-valid
    // heartbeat RbxTransport uses. No input stream means the RCC has no
    // client state to echo a character/physics stream back to.
    auto next_input = std::chrono::steady_clock::now();
    int inputs_sent = 0;
    auto pumpInput = [&]() {
      const auto now = std::chrono::steady_clock::now();
      if (now < next_input) return;
      next_input = now + std::chrono::milliseconds(1000);
      static const uint8_t kHeartbeat[2] = {0xA6, 0x00};
      if (client.sendUnreliable(kHeartbeat, sizeof kHeartbeat) &&
          ++inputs_sent == 1)
        printf("raknet: 0xA6 input heartbeat started (unreliable)\n");
    };
    // The four post-answer 0xA6 route declarations the native sends on the
    // gameplay channel (~66 ms after the answer). Bytes are verbatim from the
    // native session (same constants as src/quic/send.cpp kRoutes); they are
    // keepalive shapes, not movement, but the RCC expects them before driven
    // input and the native RakNet capture interleaves 0xA6 with replication.
    static const char* kRoutes[] = {
        "a60101000114000000140000000000",
        "a60201000118000000180000000000040400",
        "a6030100011c0000001c0000000000040400040400",
        "a60401000120000000200000000000040400040400040400",
    };
    for (int i = 0; i < 4; ++i) {
      const auto route = rbx::hexDecode(kRoutes[i]);
      if (!route.empty() &&
          client.sendUnreliable(route.data(), route.size()))
        printf("raknet: 0xA6 route declaration %d sent (%zuB)\n", i,
               route.size());
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
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
        client.pump(250);
        client.sendAcks();
        pumpInput();
        if (live) live->pump(0.25);
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
         "pkts=%llu payloads=%llu bytes=%llu app=%llu split=%llu\n",
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
