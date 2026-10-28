// raknet_main.cpp — RakNet transport, run as ./roblox --connection-type=RakNet.
//
//   roblox --connection-type=RakNet --join-json run/join.json [--seconds 10]
//   roblox --connection-type=RakNet --place-id 1818 --roblosecurity <cookie>
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
#include "join/join.h"
#include "join/target.h"
#include "raknet/join_flow.h"
#include "raknet/raknet.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef RBX_HAVE_RAKNET
#define RBX_HAVE_RAKNET 0
#endif

using nlohmann::json;

namespace {

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
  if (args.has("--headful"))
    fprintf(stderr, "raknet: no viewer in this transport; running headless\n");
  if (args.has("--shot"))
    fprintf(stderr, "raknet: --shot unsupported (no viewer); ignored\n");

  const long long place = args.number("--place-id");
  const std::string cookie = args.get("--roblosecurity");
  const std::string join_path = args.get("--join-json");
  const std::string save_path = args.get("--save-join");
  const std::string job = args.get("--job-id");
  long universe = long(args.number("--universe-id"));
  std::string token_hex = args.get("--token");
  const int seconds = int(args.number("--seconds", 10));
  const int tok_sub = int(args.number("--token-sub"));
  const bool req2 = args.has("--req2");
  const bool no_crypto = args.has("--no-crypto");
  const bool skip_join = args.has("--skip-join");

  rbxclient::JoinFlowPaths flow;
  flow.join90 = args.get("--90-template", flow.join90);
  flow.ticket8a = args.get("--8a-template", flow.ticket8a);
  flow.pre_join = args.get("--pre-join", flow.pre_join);

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
  if (!client.connect(cfg, &err)) {
    fprintf(stderr, "raknet connect: %s\n", err.c_str());
    return 1;
  }
  for (int i = 0; i < seconds && !client.reply2_ok(); ++i) client.pump(1000);

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
    if (skip_join) {
      printf("raknet: --skip-join: join body/ticket not sent\n");
    } else {
      sendJoinFlow(client, js, jr, place, flow, log);
    }
    for (int i = 0; i < seconds; ++i) {
      client.pump(1000);
      client.sendAcks();
    }
    if (const uint8_t* dg = client.last_datagram()) {
      printf("raknet: last server datagram %zuB: %s\n", client.last_datagram_len(),
             rbx::hexPrefix(dg, client.last_datagram_len(), 48).c_str());
    }
  }
  printf("raknet: result stage=%d mtu=%u guid=%llu session_token=%s "
         "reply2=%s data=%s datagrams=%llu/%llu acks=%llu srv_acks=%llu dups=%llu "
         "pkts=%llu payloads=%llu bytes=%llu\n",
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
         (unsigned long long)client.bytes_rx());
  return client.reply2_ok() ? 0 : 1;
}

} // namespace rbxclient
