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
#include "instance/instance.h"
#include "instance/remote.h"
#include "join/join.h"
#include "join/target.h"
#include "raknet/challenge.h"
#include "raknet/input.h"
#include "raknet/join_flow.h"
#include "raknet/join_msg.h"
#include "raknet/raknet.h"
#include "raknet/protocol.h"
#include "view/view.h"

#include <nlohmann/json.hpp>

namespace {
// Session-age anchor for the lifecycle diagnostics. A report of "it drops
// after N minutes" is only comparable between runs if the disconnect line
// carries the age it happened at, so the message sink and the run loop read
// one shared anchor. Reset whenever a session goes live.
std::chrono::steady_clock::time_point g_session_t0;

long long sessionAgeMs() {
  if (g_session_t0.time_since_epoch().count() == 0) return -1;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - g_session_t0)
      .count();
}

// `--seconds N` is a budget for the *program*, not for one session. Counting
// it from the join made a rejoin start the window again, so `--seconds 420`
// stayed up for fifteen minutes with a second live session behind it.
std::chrono::steady_clock::time_point g_run_t0;

bool runWindowOver(long long seconds) {
  if (seconds <= 0) return false;
  return std::chrono::steady_clock::now() - g_run_t0 >=
         std::chrono::seconds(seconds);
}
} // namespace

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
                                      .flag("--packet-manifest")
                                      .flag("--req2")
                                      .flag("--no-crypto")
                                      // Display flags belong to RbxTransport.
                                      // RakNet has no renderer, so the unified
                                      // selector hands the same command line
                                      // to either transport.
                                      .flag("--headless")
                                      .flag("--headful")
                                      .flag("--render")
                                      .flag("--no-render")
                                      .flag("--scripts")
                                      .flag("--no-scripts")
                                      .flag("--gui")
                                      .flag("--no-gui")
                                      .flag("--physics")
                                      .flag("--no-physics")
                                      .value("--input")
                                      .value("--slots")
                                      .value("--app-probe")
                                      .value("--shot");
  return s;
}

} // namespace

namespace rbxclient {

int raknetMain(int argc, char** argv) {
  // Live runs are piped through timestamps/tee: without line buffering the
  // whole log lands in 4 KB chunks at exit and the timeline is lost.
  setvbuf(stdout, nullptr, _IOLBF, 0);
  g_run_t0 = std::chrono::steady_clock::now();
  const rbx::cli::Args args(argc, argv, spec());
  if (!args.ok()) {
    fprintf(stderr, "raknet: %s\n", args.error().c_str());
    return 2;
  }
  // Offline coverage report: every application id in 0x81..0xa9, with its
  // direction and whether this build has a receive decoder / send builder.
  // This runs before any cookie or network work so it is usable as a check.
  if (args.has("--packet-manifest")) {
    fputs(rbxclient::raknet::app_manifest().c_str(), stdout);
    return 0;
  }
  // Presentation is shared with RbxTransport: headful is the default (the
  // same collectBoxes/runViewerLive path), --headless runs the identical
  // Engine world with no GL, and --shot saves one frame then exits.
  //
  // Every optional subsystem is a CLI switch so a run pays only for what it
  // uses: --no-render (same as --headless), --no-scripts (skip Luau chunks),
  // --no-physics (accepted for symmetry; this build has no client physics —
  // the server owns it). --render/--scripts/--physics are the positive forms
  // and win over their --no- counterpart.
  const bool render_pos = args.has("--render") || args.has("--headful");
  bool headless = args.has("--headless") || args.has("--no-render");
  if (render_pos) headless = false;
  const bool run_scripts =
      !args.has("--no-scripts") || args.has("--scripts");
  // The GUI tree is on by default (LocalScripts wait for PlayerGui); --no-gui
  // drops it for a minimal client. --gui wins when both spellings are present.
  const bool gui = !args.has("--no-gui") || args.has("--gui");
  printf("features: render=%s scripts=%s gui=%s physics=%s\n",
         headless ? "off" : "on", run_scripts ? "on" : "off",
         gui ? "on" : "off",
         args.has("--no-physics") && !args.has("--physics")
             ? "off (server-authoritative; no client physics in this build)"
             : "server-authoritative");
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
  // The view and LiveRuntime::local_character resolve the local avatar by
  // UserId; without this they cannot tell our Player from another player's.
  rbx::set_local_user_id(js.value("UserId", 0LL));

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
  // Engine scripts use the real client->server remote path. The runtime only
  // knows the wire grammar; this sink is the RakNet transport.
  rbx::set_remote_send(
      [](void* user, const uint8_t* d, size_t n, bool unreliable) {
        auto* c = static_cast<rbxclient::RaknetClient*>(user);
        if (std::getenv("RBX_DEBUG_REMOTE"))
          printf("remote tx %zuB %s: %s\n", n,
                 unreliable ? "unreliable" : "reliable",
                 rbx::hexEncode(d, n > 48 ? 48 : n).c_str());
        return unreliable ? c->sendUnreliable(d, n)
                          : c->sendJoinMessage(d, n, /*ordered=*/true) > 0;
      },
      &client);

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
  // Set when the RCC ends the session (0x15) or stops sending entirely. The
  // run loop turns it into a fresh join instead of exiting: a dropped session
  // is the "stays a minute, then disconnects" symptom, and RakNet has no
  // reconnect of its own once the RCC has forgotten the session.
  bool session_dead = false;
  int rejoins = 0;
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
    live = std::make_unique<rbxclient::LiveRuntime>(job_id, log, run_scripts,
                                                    gui);
    // The native client creates Players.LocalPlayer (and PlayerGui) at join;
    // give LiveRuntime the authenticated identity so replicated scripts can
    // use it from their first line even when no Player record is sent.
    live->setLocalUser(js.value("UserId", 0LL),
                       js.value("UserName", std::string()),
                       js.value("DisplayName", std::string()));
    client.setMessageSink([&client, &live, &challenge_job, &challenge_seen,
                           &join_flow_open, &deferred_challenge, &session_dead,
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
      // 0x15 = RakNet ID_DISCONNECTION_NOTIFICATION. Body is [u16 0][u16 BE
      // code]: 260/261 are the receive packet/stream errors the RCC uses when
      // it cannot parse our traffic, 258 is a security-key mismatch. It never
      // reconnects, so record the verdict and let the run loop rejoin.
      if (n >= 5 && p[0] == 0x15) {
        const uint16_t code = uint16_t((uint16_t(p[3]) << 8) | p[4]);
        // Names from the ConnectionError enum (tools/api_dump.json) and the
        // native name switch (libroblox sub_33E7C3F):
        //   0x0102 DisconnectSecurityKeyMismatch
        //   0x0104 DisconnectReceivePacketError
        //   0x0105 DisconnectReceivePacketStreamError
        //   0x0106 DisconnectSendPacketError
        //   0x0114 DisconnectRejoin  <- routine: the RCC is *asking* for a
        //          reconnect (job rotate/maintenance), not reporting bad
        //          traffic. The native client simply rejoins; ours used to
        //          treat it as the end of the run, which is the "drops out of
        //          nowhere after 30-60 s" symptom.
        //   0x0115 DisconnectConnectionLost
        //   0x0117 DisconnectRaknetErrors
        const char* why =
            code == 0x0114 ? "DisconnectRejoin (server asked us to reconnect)"
            : code == 0x0106 ? "DisconnectSendPacketError"
            : code == 0x0104 ? "DisconnectReceivePacketError"
            : code == 0x0105 ? "DisconnectReceivePacketStreamError"
            : code == 0x0102 ? "DisconnectSecurityKeyMismatch"
            : code == 0x0115 ? "DisconnectConnectionLost"
            : code == 0x0117 ? "DisconnectRaknetErrors"
                             : "disconnect";
        const long long age = sessionAgeMs();
        printf("raknet: 0x15 %s (code %u / 0x%04x) at session age %lld.%03llds — "
               "session ended by the RCC\n",
               why, unsigned(code), unsigned(code), age / 1000, age % 1000);
        session_dead = true;
        return;
      }
      // 0x95 = ID_SECURITY_KEY_REJECTED: the RUPP token we present was not
      // the one the session expects (the RCC rotates it). Also terminal.
      if (n >= 1 && p[0] == 0x95) {
        printf("raknet: 0x95 security key rejected (%zuB) — session is over\n",
               n);
        session_dead = true;
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
        const std::vector<uint8_t> ack = rbxmsg::buildMarkerAck(marker);
        if (client.sendJoinMessage(ack.data(), ack.size(), /*ordered=*/true))
          printf("raknet: 0x84 marker %u acked (0x04)\n", marker);
      }
      // 0x83/0x05 is a one-way data-channel heartbeat: the RCC sends it and no
      // reply is expected. It is not a ping awaiting 0x06 — the native's own
      // 0x83/0x06 frames are instance-ack record arrays
      // (`83 06 01 <u32 id> 00 00 00 00 <u32 id> ...`) keyed on the server's
      // own 0xc5____ ids (same-job capture /tmp/nat_same). Both replies were
      // measured to end the session ~1 s after the join with a 0x0105 receive
      // error:
      //   - flipping the subtype to 0x06 (the old behaviour, /tmp/our6.log)
      //     hands the RCC's ack parser a heartbeat body;
      //   - echoing it back as 0x05 is rejected too (2026-10-31, /tmp/join7.log:
      //     the 0x15 lands immediately after the mirror).
      // Sending nothing is what stays joined (/tmp/join8.log, 2026-10-31:
      // 70 s, 1469 live frames, 1689 spawns, no 0x15).
      // Opt back in for A/B runs: RBX_PING05=mirror echoes it as 0x05,
      // RBX_PING05=ack06 restores the old flip.
      if (n >= 3 && p[0] == 0x83 && p[1] == 0x05) {
        const std::string mode = getenv("RBX_PING05") ? getenv("RBX_PING05") : "";
        if (mode == "mirror" || mode == "ack06") {
          std::vector<uint8_t> pong(p, p + n);
          if (mode == "ack06") pong[1] = 0x06;
          if (client.sendJoinMessage(pong.data(), pong.size(),
                                     /*ordered=*/false))
            printf("raknet: 0x83/0x05 reply 0x83/%02x (%zuB)\n", pong[1],
                   pong.size());
        }
      }
      // The 0x93 join reply is the server's verdict on the join: log its head
      // so a silent gate can be told apart from an accepted-but-idle session.
      if (n >= 2 && p[0] == 0x93)
        fprintf(stderr, "raknet: 0x93 head %s\n", rbx::hexPrefix(p, n, 48).c_str());
      // RBX_DUMP_APP=<dir>: write complete app payloads to numbered files so
      // framing work is done on real bytes, not on truncated hex. The cap
      // keeps a long session from filling the disk. This is a probe, not a
      // protocol feature; unknown ids are already counted in the census.
      if (const char* dir = getenv("RBX_DUMP_APP")) {
        static int dumped = 0;
        const char* lim = getenv("RBX_DUMP_APP_LIMIT");
        const int limit = lim ? atoi(lim) : 256;
        const char* only = getenv("RBX_DUMP_APP_ID"); // hex, e.g. a1
        const bool wanted =
            !only || !*only ||
            (n > 0 && p[0] == uint8_t(std::strtoul(only, nullptr, 16)));
        if (wanted && n > 0 && dumped < limit) {
          char path[1024];
          std::snprintf(path, sizeof path, "%s/%05d_%02x_%zu.bin", dir,
                        dumped, p[0], n);
          if (FILE* f = std::fopen(path, "wb")) {
            std::fwrite(p, 1, n, f);
            std::fclose(f);
          }
          ++dumped;
        }
      }
      if (const char* hx = getenv("RBX_APP_HEX")) {
        static int hex_logged = 0;
        const bool all = std::strcmp(hx, "1") == 0;
        if ((all || (n > 0 && p[0] == uint8_t(std::strtoul(hx, nullptr, 16)))) &&
            hex_logged < 8) {
          ++hex_logged;
          printf("raknet: app hex %02x (%zuB):", p[0], n);
          for (size_t k = 0; k < n && k < 48; ++k)
            printf(" %02x", p[k]);
          printf("\n");
        }
      }
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
  // advancing by 4 per message (0x14c, 0x150, 0x154, 0x158, ...). Only the
  // count=4 steady-state frame carries the build's 11-byte tag
  // (`10 03 00 64 d0 b7 e6 d7 ff 03 e0`); counts 1-3 use the short
  // `a6 <count> 01 00 01 <step8> 00 00 (04 04 00)*<count-1>` shape. The old
  // count=2/3 templates carried the tag, so both were 10 bytes longer than
  // anything the native ever sends and the RCC answered the input stream with
  // a receive-error disconnect.
  // 0xA6 input. `native` (default) sends the captured route frames: counts
  // 1,2,3 once, then count 4 forever, step +4, ~13 Hz. `control` replaces the
  // frame's final nActions byte with a ControlState block carrying the slots
  // from --slots (a comma list of 0..7). The framing is native; the movement
  // semantics of each slot are not recovered, so this is an explicitly
  // chosen probe, never a guess about "forward".
  const std::string input_mode = args.get("--input", "native");
  const bool input_known = input_mode == "native" || input_mode == "control";
  if (!input_known)
    fprintf(stderr, "raknet: --input '%s' is not native|control; using native\n",
            input_mode.c_str());
  uint8_t control_slots = 0;
  {
    const std::string slots = args.get("--slots");
    size_t p = 0;
    while (p < slots.size()) {
      size_t q = slots.find(',', p);
      if (q == std::string::npos) q = slots.size();
      const std::string one = slots.substr(p, q - p);
      if (!one.empty()) {
        const int bit = atoi(one.c_str());
        if (bit >= 0 && bit <= 7) control_slots |= uint8_t(1u << bit);
        else fprintf(stderr, "raknet: --slots bit %d outside 0..7 ignored\n", bit);
      }
      p = q + 1;
    }
  }
  const bool control_input = input_known && input_mode == "control" &&
                             control_slots != 0;
  if (input_mode == "control" && control_slots == 0)
    fprintf(stderr, "raknet: --input=control needs --slots (e.g. --slots=0); "
                    "sending native route frames\n");
  auto a6Frame = [control_input, control_slots](int idx, uint32_t step) {
    if (control_input)
      return rbxclient::input::buildFrame(
          step, 1, rbxclient::input::controlBlock(control_slots), false);
    return rbxclient::input::routeFrame(idx, step);
  };
  const std::string app_probe = args.get("--app-probe");
  bool session_up = false;
  // Covers the join misses (a challenge the RCC never issues) plus one
  // attempt per allowed rejoin.
  for (int attempt = 0; attempt <= 11 && !g_stop; ++attempt) {
  if (session_up) break;
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
      // The 19-byte 0x83/0x07 opener must NOT be sent here: it takes ordering
      // slot 5, which is exactly where the native puts its 0x9B answer, and
      // pushes our answer to 6 and hydration to 7. That shift is the gate the
      // RCC answered with 0x15 00 00 01 01 ("answer at index 6") in the earlier
      // join work, and with it restored the RCC now disconnects the placed
      // session with a receive error. The opener belongs after the answer and
      // the attributes push, so it is sent from the input pump once the world
      // is up (slot 7+), exactly like the native's post-join stream.
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
      // RBX_8304_RAW=<file>: replay a captured native 0x83/0x04 record at the
      // slot it occupies in the native's stream (ordering index 6, right
      // after the 0x9B answer). This is the one post-answer message the
      // native sends that this client does not; the file must be a complete
      // app message starting with 0x83. Env-only: it is a live probe, not a
      // default, because the record can embed session ids.
      // The native's first post-answer record is 0x83/0x04, its client-owned
      // character state; the RCC will not spawn the local avatar without one.
      // A/B on two accounts (2026-11-03): with this 55-byte record at ordering
      // slot 6 the Player joins and the character links (king 13 parts, mizg 26
      // parts) and the RCC streams 0xA1 state; without it no Character and no
      // 0xA1 at all. The record's small instance ids come from the capture, but
      // they were unrelated to both live jobs and the RCC still spawned, so it
      // reads the record as "the client has a character" rather than resolving
      // those ids. RBX_NO_8304=1 disables it; RBX_8304_RAW=<file> overrides the
      // bytes (complete app message starting with 0x83).
      if (!getenv("RBX_NO_8304")) {
        static const uint8_t kSpawnBootstrap[55] = {
            0x83, 0x04, 0xbe, 0x03, 0x00, 0x00, 0x05, 0x16, 0x95, 0x0a,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x96, 0x0a, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0xb4, 0xbd, 0xd1, 0x41, 0xfc, 0x79,
            0x8f, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x92, 0x0a, 0x00, 0x00, 0x80, 0x9c, 0x48, 0x44, 0x23,
            0x6c, 0x00, 0x00, 0x00, 0x00};
        const uint8_t* bp = kSpawnBootstrap;
        size_t bn = sizeof kSpawnBootstrap;
        std::vector<uint8_t> alt;
        if (const char* raw = getenv("RBX_8304_RAW")) {
          alt = rbx::readFileVec(raw);
          if (alt.size() >= 2 && alt[0] == 0x83) {
            bp = alt.data();
            bn = alt.size();
          } else {
            fprintf(stderr, "raknet: RBX_8304_RAW %s is not a 0x83 message\n",
                    raw);
          }
        }
        const int n = client.sendJoinMessage(bp, bn);
        printf("raknet: 0x83/04 spawn bootstrap (%zuB, %d datagrams)%s\n", bn, n,
               alt.empty() ? "" : " [raw override]");
      }
      // OFF by default: the account JSON does not belong on 0x83/0x04.
      //
      // A same-job capture (2026-10-30, /tmp/nat_same: native and this client
      // in one gameInstanceId) shows the native's 0x83/0x04 is a 55-byte
      // binary replication record that follows the challenge answer, and its
      // account JSON (lastPerformed/ageBracket/...) appears nowhere in the
      // RakNet stream — the six identity fields ride inside the 0x90 document
      // instead (see join_msg.cpp). Feeding a 318-byte JSON to the subtype the
      // RCC parses as replication is a receive-side parse error, the shape of
      // the 0x0104/0x0105 disconnects we get ~10 s in. RBX_ATTRS_8304=1
      // restores the old push for A/B runs.
      const char* attrs_8304 = getenv("RBX_ATTRS_8304");
      if (attrs_8304 && *attrs_8304 && *attrs_8304 != '0' &&
          js.is_object() && js.contains("UserId") &&
          getenv("RBX_NO_HYDRATION") == nullptr) {
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
      g_session_t0 = std::chrono::steady_clock::now();
      if (!challenge_seen && !(live && live->has_world()))
        printf("raknet: no 0x9B challenge yet — running the session (native "
               "does not wait for one)\n");
    }
  }
  if (session_up) {
    // Explicit one-shot app probes. 0x96/0x99 are native-shaped client
    // messages that are not part of the RakNet join ladder on every job, so
    // they stay opt-in instead of changing the proven join sequence.
    if (!app_probe.empty()) {
      size_t p = 0;
      while (p < app_probe.size()) {
        const size_t q = app_probe.find(',', p);
        const std::string one = app_probe.substr(
            p, q == std::string::npos ? std::string::npos : q - p);
        std::vector<uint8_t> msg;
        if (one == "96")
          msg = rbxmsg::build96();
        else if (one == "96f")
          msg = rbxmsg::build96f();
        else if (one == "99")
          msg = rbxmsg::build99();
        else if (!one.empty())
          fprintf(stderr, "raknet: --app-probe '%s' is not 96|96f|99\n",
                  one.c_str());
        if (!msg.empty()) {
          const int n = client.sendJoinMessage(msg.data(), msg.size());
          printf("raknet: 0x%02x app probe sent (%zuB, %d datagrams)\n",
                 unsigned(msg[0]), msg.size(), n);
        }
        if (q == std::string::npos) break;
        p = q + 1;
      }
    }
    const auto run_start = std::chrono::steady_clock::now();
    // Native input stream, replayed in the native's order and cadence: 01,
    // 02, 03 once, then 04 at ~30 Hz, stepIds +4 per message. A one-shot
    // burst was not enough (the RCC stops receiving route ACKs and never
    // starts replicating); the native's live capture keeps this running for
    // the whole session.
    auto next_input = std::chrono::steady_clock::now();
    uint32_t a6_step = 0x14c;
    int a6_sent = 0;
    int ack83_sent = 0;
    int a6_stage = 0;
    auto next_clock_tick = std::chrono::steady_clock::now();
    auto next_ack83 = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(2500);
    auto next_ack83_pair = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(5000);
    uint32_t clock_step = 0x270; // native's first 0x83/23 value, +8 per tick
    uint64_t clock_replied = 0;  // server ticks already answered
    bool hb_started = false;
    // Canned post-join frames, taken from another session's native stream.
    //
    // Both blobs reference instance ids from the capture they came from
    // (0x0512b6 in the opener, 0x10dc16 in the ack pair). This job's parts are
    // assigned ids in the 0xc5____ range, so replaying them hands the RCC
    // references to instances that do not exist — the shape of its 0x0105
    // receive error. The native's own 0x83/07, 0x83/1c and 0x83/0e frames
    // always carry ids from the live session (same-job capture, /tmp/nat_same).
    // Off by default; RBX_CANNED_83=1 restores them for A/B runs.
    const char* canned = getenv("RBX_CANNED_83");
    const bool canned83 = canned && *canned && *canned != '0';
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
    bool local_logged = false;
    bool a1_held_note = false;
    // Input cadence. The earlier 75 ms default came from a 34 s capture in
    // which the client was still loading (13.3 0xA6/s). A same-job round
    // measured over 194 s of steady state (tools/parity_round.sh, native
    // `dgram_*` deduped by (channel, ordering index)) gives the real numbers:
    //   0xA6        59.3/s  (p50 16 ms)     0x83/23 clock  24.5/s  (p50 41 ms)
    //   0x83/0A      9.3/s                  0x83/06        3.9/s
    //   0x83/05      3.0/s                  0x83/07        1.1/s
    //   0x83/1C      0.9/s                  0xA1           0.3/s
    // The clock carried by 0xA6 and 0x83/23 is the session clock in quarter
    // milliseconds: it advanced 39176 units over 164 s = 239/s = 4/ms, which
    // is why the native's deltas are 4, 8, 12 and never a fixed step.
    int input_ms = 16;
    if (const char* im = getenv("RBX_INPUT_MS")) input_ms = std::max(10, atoi(im));
    // Diagnostics only: the RCC disconnects us with 0x15 01 04 ("error while
    // receiving data") once the world is up, so each post-join stream is
    // gated so the offending one can be bisected by env.
    const bool no_hb = getenv("RBX_NO_HB") != nullptr;
    const bool no_clock = getenv("RBX_NO_CLOCK") != nullptr;
    // The native's 0x83/0x05 pair is the client's per-instance physics/state
    // acknowledgement, and it is what the RCC's liveness timer waits for: a
    // session without it ends with `0x15 DisconnectTimeout` (266) at exactly
    // 300.1 s, every time, on every place tried. Measured 2026-11-06:
    //   default                       -> 266 at 300.514 s
    //   with the pair (opener off)    -> still live at 361 s, no 0x15
    // It used to ride on RBX_CANNED_83 together with the 0x83/0x07 opener,
    // but the opener is the frame that gets us 0x0104/0x0105 kicked, so the
    // pair is now on by default and the opener stays gated. RBX_NO_ACK83=1
    // restores the old silent steady state for A/B runs.
    const bool no_ack83 = getenv("RBX_NO_ACK83") != nullptr;
    const bool no_a6 = getenv("RBX_NO_A6") != nullptr;
    const bool no_a1 = getenv("RBX_NO_A1") != nullptr;
    auto next_camera = std::chrono::steady_clock::now();
    auto pumpInput = [&]() {
      const auto now = std::chrono::steady_clock::now();
      if (now < next_input) return;
      // Emit every frame whose time has come, not one per loop iteration. The
      // native holds ~59.5 0xA6/s no matter what the world is doing; one
      // frame per iteration tracked the loop rate instead (21/s on a heavy
      // 1818 join) and starved the server's input queue, which is what a
      // 700 s round shows as "a6 ours 14976 / native 41701". Each frame
      // carries its scheduled timestamp, so a catch-up burst is still evenly
      // spaced on the wire.
      int a6_budget = 24;
      while (now >= next_input && a6_budget-- > 0) {
        const auto tick = next_input;
        next_input += std::chrono::milliseconds(input_ms);
        const int idx = a6_stage < 3 ? a6_stage : 3;
        // Frame step is the session clock (4 units/ms), taken from the tick's
        // scheduled time so a burst advances it by exactly one interval per
        // frame instead of repeating one sample.
        a6_step = uint32_t(
            std::chrono::duration_cast<std::chrono::milliseconds>(tick - run_start)
                .count() * 4);
        const auto frame = a6Frame(idx, a6_step);
        if (!no_a6 && !frame.empty() &&
            client.sendUnreliable(frame.data(), frame.size()) && ++a6_sent == 1)
          printf("raknet: 0xA6 input stream started (native route frames)\n");
        ++a6_stage;
      }
      // Do not replay a huge backlog after a stall (join load, a big
      // replication batch); resync the schedule instead.
      if (now > next_input + std::chrono::milliseconds(400))
        next_input = now + std::chrono::milliseconds(input_ms);
      // Native 0xA1 camera frame: `a1 <u32le clock>` + a 39-byte body. In the
      // 34 s capture every frame carries the same body (only the clock
      // changes). The body's u24 instance reference is session-specific; the
      // capture id only works in the job it came from. Substituting the live
      // character model, HumanoidRootPart, or Player id was tested on 1818
      // and all three were rejected with 0x15 01 04, so the frame remains
      // gated until the exact reference semantics are recovered. The native
      // sends one per ~730 ms.
      {
        constexpr uint32_t kA1RefId = 0x0512b6;
        if (!no_a1 && now >= next_camera) {
          next_camera = now + std::chrono::milliseconds(730);
          if (live && live->has_world() && live->hasInstance(kA1RefId)) {
            const std::vector<uint8_t> a1 =
                rbxmsg::buildA1Camera(uint32_t(a6_step), kA1RefId);
            if (!a1.empty()) client.sendUnreliable(a1.data(), a1.size());
          } else if (!a1_held_note) {
            a1_held_note = true;
            printf("raknet: 0xA1 camera frame held back — its capture id "
                   "(0x%06x) is not in this job's world\n", kA1RefId);
          }
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
          const char* no_op = getenv("RBX_NO_OPENER");
          if (canned83 && !no_hb && !(no_op && *no_op && *no_op != '0')) {
            const auto ev = rbx::hexDecode(
                "830701b612050013100000e04023b401000000");
            client.sendJoinMessage(ev.data(), ev.size());
            printf("raknet: 0x83/0x07 control opener sent (19B)\n");
          }
        } else {
          if (!no_clock) {
            // 0x83/0x23 is reply-driven: one clock out per server clock in.
            // Measured in the same job, the server sent 10.9/s and the native
            // answered 12.1/s, so a free-running timer is always the wrong
            // rate (ours ran at 28-31/s). A 250 ms floor keeps a tick going
            // when the server is momentarily quiet.
            // A 0x83 frame is a *container*: one 0x83 byte, then whole
            // records back to back (native, same job):
            //   83 23 f8050000 0a 01 <ref> 00 b815 01 <u24 clock> 00
            //   83 23 18040000 23 1c040000 00
            //   83 06 <record> 05 <record> 23 <clock> 00
            // Our first version emitted exactly one record per frame, which is
            // why frame counts and per-frame record mixes could never line up.
            // The 0x0a record is the per-instance "applied up to <clock>" ack
            // the native sends ~22/s against live refs; ours was absent.
            const bool no_0a = getenv("RBX_NO_0A") != nullptr;
            auto append0a = [](std::vector<uint8_t>& f, uint32_t ref,
                               uint32_t clock) {
              f.push_back(0x0a);
              f.push_back(0x01);
              f.push_back(uint8_t(ref & 0xff));
              f.push_back(uint8_t((ref >> 8) & 0xff));
              f.push_back(uint8_t((ref >> 16) & 0xff));
              f.push_back(0x00);
              f.push_back(0xb8);
              f.push_back(0x15);
              f.push_back(0x00);  // no fractional byte
              f.push_back(uint8_t(clock & 0xff));
              f.push_back(uint8_t((clock >> 8) & 0xff));
              f.push_back(uint8_t((clock >> 16) & 0xff));
              f.push_back(0x00);
            };
            auto sendClock = [&]() {
              clock_step = uint32_t(
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - run_start).count() * 4);
              std::vector<uint8_t> f;
              f.reserve(96);
              f.push_back(0x83);
              if (!no_0a && live && live->has_world()) {
                // Live refs only: a ref from another session is an instance
                // the RCC cannot resolve (its 0x0105 receive error).
                const auto parts = live->local_character_parts();
                int n = 0;
                for (const auto& p : parts) {
                  if (p.ref == 0) continue;
                  append0a(f, p.ref, clock_step);
                  if (++n >= 8) break;
                }
              }
              f.push_back(0x23);
              f.push_back(uint8_t(clock_step & 0xff));
              f.push_back(uint8_t((clock_step >> 8) & 0xff));
              f.push_back(uint8_t((clock_step >> 16) & 0xff));
              f.push_back(0x00);
              f.push_back(0x00);
              client.sendJoinMessage(f.data(), f.size());
            };
            const uint64_t want = client.clock_ticks_rx();
            if (clock_replied > want) clock_replied = want;  // fresh session
            int clk_budget = 4;
            bool replied = false;
            while (clock_replied < want && clk_budget-- > 0) {
              ++clock_replied;
              sendClock();
              replied = true;
            }
            if (!replied && now >= next_clock_tick) {
              // Floor only for a genuinely quiet server. At 250 ms it added
              // 4/s on top of the replies and put us at 18.1/s against the
              // native's 13.8/s; at 1 s the total lands within a few percent.
              next_clock_tick = now + std::chrono::milliseconds(1000);
              sendClock();
            }
          }
          if (!no_ack83 && now >= next_ack83) {
            // Native, same job: 45 B unreliable at 2.2/s and 50 B reliable at
            // 1.0/s (the reliable copy bundles a 0x23 clock record, hence the
            // extra 5 bytes). Ours sent one of each every 2.5/5 s.
            const bool pair = now >= next_ack83_pair;
            next_ack83 = now + std::chrono::milliseconds(500);
            if (pair) next_ack83_pair = now + std::chrono::milliseconds(1000);
            const auto unrel = rbx::hexDecode(kAck83Unrel);
            if (!unrel.empty() &&
                client.sendUnreliable(unrel.data(), unrel.size()) &&
                ack83_sent++ == 0)
              printf("raknet: 0x83/0x05 physics-ack stream started "
                     "(liveness; without it the RCC ends the session at "
                     "300 s with 0x15 266)\n");
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
      if (live && live->has_world()) {
        const std::string local = live->local_character();
        if (!local.empty() && !local_logged) {
          local_logged = true;
          printf("raknet: local avatar linked: %s\n", local.c_str());
        }
        if (!char_note && local.empty()) {
          const auto e = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - run_start)
                             .count();
          if (e >= 12) {
            char_note = true;
            printf("raknet: no local avatar after %llds — the RCC did not "
                   "spawn our Character (0x83/04 spawn bootstrap missing?)\n",
                   (long long)e);
          }
        }
      }
    };
    auto out_of_time = [&]() { return runWindowOver(seconds); };
    // Join watchdog. The RCC answers a placed join with 0x97 (schema + join
    // data) and then streams the record batches that build the world. It
    // sometimes answers 0x97 and streams nothing else (measured on Adopt Me
    // 920587237: about one join in three); that session never completes, and
    // the native client's answer is a fresh session, not an endless wait. A
    // single 0x97 is the reference point, so a slow join on a big place is
    // not mistaken for a stalled one.
    long long join_stall_ms = 20000;
    if (const char* v = getenv("RBX_JOIN_STALL_MS"))
      join_stall_ms = std::max(2000, atoi(v));
    auto join_stalled = [&]() -> bool {
      if (!live || live->has_world() || !live->join_data_seen()) return false;
      const long long quiet = live->ms_since_progress();
      return quiet >= 0 && quiet >= join_stall_ms;
    };
    bool join_retry = false;
    // Periodic progress line while the session runs. A long soak that is killed
    // from outside otherwise leaves no numbers behind, and the discontinuities
    // (a1 apply ratio, script count) are what actually explain a late drop.
    auto last_stats = std::chrono::steady_clock::now();
    auto stats_tick = [&]() {
      if (!live) return;
      const auto now = std::chrono::steady_clock::now();
      const auto gap = std::chrono::duration_cast<std::chrono::seconds>(
                           now - last_stats)
                           .count();
      long long stats_secs = 60;
      if (const char* v = getenv("RBX_STATS_SECS"))
        stats_secs = std::max(1, atoi(v));
      if (gap < stats_secs) return;
      last_stats = now;
      const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                              now - run_start)
                              .count();
      printf("raknet: t=%llds live: %s\n", static_cast<long long>(uptime),
             live->stats().c_str());
      // Whether the avatar's pose changes without us simulating it is the
      // whole "can the server own the character" question, measured: the
      // client owns its own character, so a static pose means the server is
      // not stepping it for us and no physics implementation is needed.
      for (const auto& p : live->local_character_parts()) {
        if (p.has_cframe && p.name == "HumanoidRootPart")
          printf("raknet: avatar pose t=%llds (%.3f, %.3f, %.3f) ref=0x%06x\n",
                 static_cast<long long>(uptime), p.x, p.y, p.z, p.ref);
      }
      fflush(stdout);
    };
    if (headless) {
      if (seconds > 0)
        printf("raknet: connected; running for %ds (--seconds, headless)\n",
               seconds);
      else
        printf("raknet: connected; running until interrupted (Ctrl+C, "
               "headless)\n");
      // The loop period IS the input cadence: pump(30) capped the whole loop
      // at ~33 Hz, which is why 0xA6 went out at 29/s instead of the native's
      // 59.6/s and 0x83/0x23 at 15/s instead of 28.3/s. 2 ms keeps the timers
      // below accurate to a couple of ms without spinning a core.
      auto last_live_pump = std::chrono::steady_clock::now();
      while (!g_stop && !out_of_time()) {
        client.pump(2);
        if (join_stalled()) {
          join_retry = true;
          printf("raknet: join stalled — 0x97 arrived %llds ago with no "
                 "snapshot progress; dropping the session and rejoining\n",
                 live->ms_since_join_data() / 1000);
          break;
        }
        const long long quiet = client.ms_since_rx();
        if (quiet > 6000 && quiet - quiet_announced > 6000) {
          quiet_announced = quiet;
          printf("raknet: NO SERVER DATAGRAM for %llds — the RCC has sent "
                 "nothing (it never sends a disconnect; this is either a "
                 "reaped session or an idle world)\n",
                 quiet / 1000);
        }
        // A session the RCC has reaped goes silent rather than sending 0x15.
        // Treat it like an explicit disconnect so the client can rejoin.
        if (quiet > 12000) {
          printf("raknet: server silent for %llds — session is dead\n",
                 quiet / 1000);
          session_dead = true;
          break;
        }
        pumpInput();
        if (live) {
          // Advance the script scheduler by the time actually elapsed, not by
          // a fixed 30 ms that only matched the old 30 ms pump.
          const auto now = std::chrono::steady_clock::now();
          double dt = std::chrono::duration<double>(now - last_live_pump).count();
          last_live_pump = now;
          if (dt > 0.25) dt = 0.25;
          live->pump(dt);
        }
        stats_tick();
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
        if (join_stalled()) {
          join_retry = true;
          printf("raknet: join stalled — 0x97 arrived %llds ago with no "
                 "snapshot progress; dropping the session and rejoining\n",
                 live->ms_since_join_data() / 1000);
          return false;
        }
        const long long quiet = client.ms_since_rx();
        if (quiet > 6000 && quiet - quiet_announced > 6000) {
          quiet_announced = quiet;
          printf("raknet: NO SERVER DATAGRAM for %llds — the RCC has sent "
                 "nothing (it never sends a disconnect; this is either a "
                 "reaped session or an idle world)\n",
                 quiet / 1000);
        }
        if (quiet > 12000) {
          printf("raknet: server silent for %llds — session is dead\n",
                 quiet / 1000);
          session_dead = true;
          return false;
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
      // Closing the window is a user stop; a dead session or a stalled join
      // must fall through to the rejoin path instead of ending the run.
      if (!session_dead && !join_retry) g_stop = 1;
    }
    printf("raknet: %s\n", g_stop ? "interrupted — closing session"
                                   : "run window complete");
    if (live) {
      printf("raknet: live: %s\n", live->stats().c_str());
      if (live->has_world())
        printf("raknet: classes: %s\n", live->class_stats().c_str());
      if (!live->has_world()) {
        if (!live->join_data_seen())
          printf("raknet: no join data (0x97): the RCC never answered the "
                 "join; world not applied\n");
        else
          printf("raknet: join data (0x97) arrived but no record batch "
                 "followed; the RCC never streamed the world\n");
      }
    }
    if (!g_stop && join_retry) {
      // A half-join cannot be repaired inside the same session (the RCC's
      // snapshot stream is bound to it); redo the whole handshake with a
      // fresh join script, exactly like a disconnect rejoin.
      printf("raknet: rejoining with a fresh session after a stalled join\n");
      session_up = false;
      join_retry = false;
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }
    if (const uint8_t* dg = client.last_datagram()) {
      printf("raknet: last server datagram %zuB: %s\n", client.last_datagram_len(),
             rbx::hexPrefix(dg, client.last_datagram_len(), 48).c_str());
    }
  }
  // The RCC never reconnects a dropped session, and a silent socket is the
  // normal end-of-session signal on this transport. A fresh RakNet session
  // with a fresh join script is the only way back into the game, so redo the
  // whole handshake instead of exiting the program. DisconnectRejoin is
  // routine, so this is not a rare path: give it room, but pace it.
  // A bounded run must end the *program*, not one session: `out_of_time()`
  // counts from this session's join, so rejoining after the window restarts
  // the clock and `--seconds 420` ran for fifteen minutes, leaving a second
  // live session (and a full parse load) behind every test.
  if (!g_stop && session_dead && rejoins < 10 && !runWindowOver(seconds)) {
    ++rejoins;
    printf("raknet: session dropped — rejoining (rejoin %d)\n", rejoins);
    session_up = false;
    session_dead = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    continue;
  }
  break;
  } // attempt loop
  printf("raknet: result stage=%d mtu=%u guid=%llu session_token=%s "
         "reply2=%s data=%s datagrams=%llu/%llu acks=%llu naks=%llu srv_acks=%llu dups=%llu "
         "pkts=%llu payloads=%llu bytes=%llu app=%llu split=%llu rtx=%llu "
         "tx_fail=%llu\n",
         client.stage(), unsigned(client.mtu()),
         (unsigned long long)client.server_guid(),
         client.has_session_token() ? "yes" : "no",
         client.reply2_ok() ? "verified" : "no",
         client.data_ready() ? "armed" : "no",
         (unsigned long long)client.data_datagrams_rx(),
         (unsigned long long)client.data_frames_rx(),
         (unsigned long long)client.acks_sent(),
         (unsigned long long)client.naks_sent(),
         (unsigned long long)client.server_acks(),
         (unsigned long long)client.dups_dropped(),
         (unsigned long long)client.pkts_rx(),
         (unsigned long long)client.payloads_rx(),
         (unsigned long long)client.bytes_rx(),
         (unsigned long long)client.app_messages(),
         (unsigned long long)client.app_split_messages(),
         (unsigned long long)client.retransmits_tx(),
         (unsigned long long)client.send_failures());
  printf("raknet: %s\n", client.packet_census().c_str());
  // A verified handshake without a placed session is still a failed join;
  // report it instead of letting scripts read a challenge-less run as success.
  return (client.reply2_ok() && session_up) ? 0 : 1;
}

} // namespace rbxclient
