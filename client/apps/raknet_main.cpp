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

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <climits>
#include <fstream>
#include <iterator>
#include <string>

#include "audio/audio.h"
#include "common/cli.h"
#include "common/avatar_watch.h"
#include "common/features.h"
#include "common/session_config.h"
#include "common/hex.h"
#include "common/util.h"
#include "engine/live_runtime.h"
#include "engine/local_player.h"
#include "instance/instance.h"
#include "instance/remote.h"
#include "join/join.h"
#include "join/target.h"
#include "raknet/challenge.h"
#include "raknet/ack83.h"
#include "raknet/send.h"
#include "send/bridge.h"
#include "send/entity_batch.h"
#include "raknet/input.h"
#include "raknet/join_flow.h"
#include "raknet/join_msg.h"
#include "raknet/raknet.h"
#include "gui/gui_layout.h"
#include "instance/gui_input.h"
#include "instance/host_input.h"
#include "instance/instance.h"
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
                                      .value("--cookie-file")
                                      .value("--join-json")
                                      .value("--save-join")
                                      .value("--job-id")
                                      .value("--require-job")
                                      .value("--exec")
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
                                      .flag("--luau")
                                      .flag("--no-luau")
                                      .flag("--physics")
                                      .flag("--no-physics")
                                      .flag("--audio")
                                      .flag("--no-audio")
                                      .flag("--mic")
                                      .flag("--no-mic")
                                      .flag("--camera")
                                      .flag("--no-camera")
                                      .value("--features")
                                      .value("--profile")
                                      .value("--config")
                                      .value("--mode")
                                      .value("--world")
                                      .value("--pump-ms")
                                      .value("--spawn-retry")
                                      .value("--spawn-retry-after")
                                      .flag("--print-config")
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
  // Session configuration precedence: defaults -> --config/BOBLOX_CONFIG ->
  // BOBLOX_* environment -> command line. A supervisor can therefore keep one
  // JSON document per account ("mode":"join", "pump_ms":4, ...) and still
  // override a single field for a test run.
  rbx::features::SessionConfig session;
  {
    std::string config_path = args.get("--config");
    if (config_path.empty())
      if (const char* env = getenv("BOBLOX_CONFIG")) config_path = env;
    if (!config_path.empty()) {
      std::string cerr;
      if (!rbx::features::loadSessionConfig(config_path, &session, &cerr)) {
        fprintf(stderr, "raknet: %s\n", cerr.c_str());
        return 2;
      }
      printf("config: %s\n", config_path.c_str());
    }
    auto envStr = [](const char* name, std::string* out) {
      if (const char* v = getenv(name)) *out = v;
    };
    envStr("BOBLOX_PROFILE", &session.profile);
    envStr("BOBLOX_MODE", &session.mode);
    envStr("BOBLOX_FEATURES", &session.features);
    envStr("BOBLOX_WORLD", &session.world);
    envStr("BOBLOX_INPUT", &session.input);
    envStr("BOBLOX_EXEC", &session.exec);
    envStr("BOBLOX_JOIN_JSON", &session.join_json);
    envStr("BOBLOX_COOKIE_FILE", &session.cookie_file);
    envStr("BOBLOX_JOB_ID", &session.job_id);
    envStr("BOBLOX_REQUIRE_JOB", &session.require_job);
    envStr("BOBLOX_SAVE_JOIN", &session.save_join);
    if (const char* v = getenv("BOBLOX_PUMP_MS")) session.pump_ms = atoi(v);
    if (const char* v = getenv("BOBLOX_SECONDS")) session.seconds = atoll(v);
    if (const char* v = getenv("BOBLOX_PLACE_ID")) session.place_id = atoll(v);
    if (const char* v = getenv("BOBLOX_SPAWN_RETRY"))
      session.spawn_retry = atoi(v);
    if (const char* v = getenv("BOBLOX_SPAWN_RETRY_AFTER"))
      session.spawn_retry_after = atoi(v);
    if (args.has("--profile")) session.profile = args.get("--profile");
    if (args.has("--mode")) session.mode = args.get("--mode");
    if (args.has("--features")) session.features = args.get("--features");
    if (args.has("--world")) session.world = args.get("--world");
    if (args.has("--pump-ms")) session.pump_ms = int(args.number("--pump-ms"));
    if (args.has("--seconds")) session.seconds = args.number("--seconds");
    if (args.has("--input")) session.input = args.get("--input");
    if (args.has("--exec")) session.exec = args.get("--exec");
    if (args.has("--join-json")) session.join_json = args.get("--join-json");
    if (args.has("--save-join")) session.save_join = args.get("--save-join");
    if (args.has("--place-id")) session.place_id = args.number("--place-id");
    if (args.has("--cookie-file"))
      session.cookie_file = args.get("--cookie-file");
    if (args.has("--job-id")) session.job_id = args.get("--job-id");
    if (args.has("--require-job"))
      session.require_job = args.get("--require-job");
    if (args.has("--spawn-retry"))
      session.spawn_retry = int(args.number("--spawn-retry"));
    if (args.has("--spawn-retry-after"))
      session.spawn_retry_after = int(args.number("--spawn-retry-after"));
  }
  if (session.spawn_retry_after < 1) session.spawn_retry_after = 1;
  if (session.pump_ms < 1) session.pump_ms = 1;
  if (session.seconds < 0) {
    fprintf(stderr, "raknet: --seconds/BOBLOX_SECONDS must be >= 0\n");
    return 2;
  }
  if (session.place_id < 0) {
    fprintf(stderr, "raknet: --place-id/BOBLOX_PLACE_ID must be >= 0\n");
    return 2;
  }
  if (session.features == "list") {
    fputs(rbx::features::featureList().c_str(), stdout);
    return 0;
  }
  rbx::features::Set feature_set;
  std::string feature_error;
  if (!rbx::features::applySessionConfig(session, &feature_set,
                                         &feature_error)) {
    fprintf(stderr, "raknet: %s\n", feature_error.c_str());
    return 2;
  }
  const std::string& profile = session.profile;
  // Positive legacy switches win over negative ones, matching the previous
  // behaviour. The resolved set is the base; these are overrides.
  if (args.has("--luau") || args.has("--scripts"))
    feature_set.luau = true;
  else if (args.has("--no-luau") || args.has("--no-scripts"))
    feature_set.luau = false;
  if (args.has("--render") || args.has("--headful"))
    feature_set.render = true;
  else if (args.has("--headless") || args.has("--no-render"))
    feature_set.render = false;
  if (args.has("--gui"))
    feature_set.gui = true;
  else if (args.has("--no-gui"))
    feature_set.gui = false;
  if (args.has("--audio"))
    feature_set.audio = true;
  else if (args.has("--no-audio"))
    feature_set.audio = false;
  if (args.has("--physics"))
    feature_set.physics = rbx::features::PhysicsMode::Client;
  else if (args.has("--no-physics"))
    feature_set.physics = rbx::features::PhysicsMode::Off;
  if (!rbx::features::validate(feature_set, &feature_error)) {
    fprintf(stderr, "raknet: %s\n", feature_error.c_str());
    return 2;
  }
  const bool world_enabled =
      feature_set.world == rbx::features::WorldMode::Full;
  if (args.has("--mic")) {
    fprintf(stderr, "raknet: feature 'mic' is not implemented in this build\n");
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
  // uses: --no-render (same as --headless), --no-luau/--no-scripts (skip Luau
  // chunks), --no-gui (drop PlayerGui), and --no-physics (no local solver;
  // ownership stays server-controlled). --profile sets the defaults; explicit
  // flags still win. Physics uses Jolt for the local character and for bodies
  // granted by NetworkOwnerV3 under Workspace.AuthorityMode.Automatic.
  const bool headless = !feature_set.render;
  printf("features: %s\n",
         rbx::features::summary(feature_set, profile).c_str());
  if (args.has("--print-config")) {
    printf("%s\n",
           rbx::features::sessionConfigJson(session, &feature_set).c_str());
    return 0;
  }
  // Single session per cookie. Two clients on one account make the RCC send
  // 0x0114 DisconnectRejoin to whichever joined first, and the two then fight
  // over the same character body — observed 2026-11-06 (two live processes,
  // churn every few seconds, every physics test invalid).  This runs after
  // --packet-manifest/--print-config: those are offline checks and must not be
  // blocked by a live session on the same account.
  {
    // Use the resolved session value, not just the CLI flag: a cookie named
    // by --config or BOBLOX_COOKIE_FILE is the one the client actually joins
    // with, so it must also be the one this process locks.  Hash the path so
    // a long cookie-file name cannot overflow the lock filename.
    std::string cookie = session.cookie_file;
    if (cookie.empty()) cookie = ".boblox-default-cookie";
    // A symlinked vault entry is the same account; canonicalize so the lock
    // does not depend on which path a supervisor happened to use.
    char resolved[PATH_MAX];
    if (realpath(cookie.c_str(), resolved)) cookie = resolved;
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : cookie) {
      h ^= c;
      h *= 1099511628211ull;
    }
    char lock_path[64];
    snprintf(lock_path, sizeof(lock_path), "/tmp/boblox-%016llx.lock",
             static_cast<unsigned long long>(h));
    const int lfd = open(lock_path, O_CREAT | O_RDWR, 0600);
    if (lfd >= 0 && flock(lfd, LOCK_EX | LOCK_NB) != 0) {
      fprintf(stderr,
              "raknet: another client already holds this cookie (%s); "
              "refusing to double-join\n", lock_path);
      return 3;
    }
  }
  const std::string shot = args.get("--shot");
  if (!shot.empty() && headless) {
    fprintf(stderr, "--shot needs --headful (omit --headless)\n");
    return 2;
  }

  const long long place = session.place_id;
  std::string cookie = args.get("--roblosecurity");
  // --cookie-file keeps Boblox on its own cookie, separate from the one Rum and
  // the executor use. Read as a file so it never appears in argv.
  if (cookie.empty()) {
    if (const std::string f = session.cookie_file; !f.empty()) {
      std::ifstream in(f, std::ios::binary);
      if (!in) { fprintf(stderr, "cookie file: cannot open %s\n", f.c_str()); return 2; }
      std::string v((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
      cookie = v;
    }
  }
  if (cookie.empty())
    for (const char* k : {"ROBLOX_ROBLOSECURITY", "RBX_ROBLOSECURITY"})
      if (const char* env = getenv(k)) { cookie = env; break; }
  const std::string join_path = session.join_json;
  const std::string save_path = session.save_join;
  const std::string job = session.job_id;
  // The join script is fetched from the web API and matchmaking may place us in
  // a different server than the one we asked for. --require-job makes that
  // visible and fatal (exit 3) so a harness can retry instead of silently
  // testing against the wrong server.
  const std::string require_job = session.require_job;
  const std::string exec_path = session.exec;
  // A join-only session has no world for a script to run in. Say so before the
  // handshake instead of starting a Luau VM that never sees an instance.
  if (!world_enabled && !exec_path.empty()) {
    fprintf(stderr,
            "raknet: --exec needs a world; --world=none/--mode=join cannot "
            "run scripts\n");
    return 2;
  }
  // Read the user script once. Every session gets its own Engine (a rejoin
  // must not feed a new RCC snapshot into the previous DataModel), so each
  // LiveRuntime receives a fresh copy of the same source.
  std::string exec_source;
  if (!exec_path.empty()) {
    std::ifstream in(exec_path, std::ios::binary);
    if (!in) {
      fprintf(stderr, "--exec: cannot open %s\n", exec_path.c_str());
      return 2;
    }
    exec_source.assign((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  }
  long universe = long(args.number("--universe-id"));
  std::string token_hex = args.get("--token");
  // 0 = stay connected until interrupted (the default, matching RbxTransport).
  const int seconds = int(session.seconds);
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
  // The 0x8a body must declare the platform the session was minted for, so it
  // comes from the same identity the join request used (rbx::joinPlatform).
  flow.platform = rbx::joinPlatform();

  // A placed join cannot proceed without the captured 0x90 body skeleton: it
  // carries the native's 53-key document and the tail constants the 0x92 key
  // is derived from. Those files are the previous session's own material, so
  // they are not in the repo and a fresh checkout has none of them. This used
  // to log one line and then run a session that could never join — with no
  // body there is nothing for the RCC to answer, so there is no 0x9B, no 0x97
  // and no 0x15 either, which reads exactly like "this server refuses me".
  // Fail before the handshake so the cause is unambiguous.
  // Neither template is required any more: the 0x90 body is generated from
  // build constants plus this session's join script/hydration, and the 0x8a
  // session document comes from the join script itself. An explicitly named
  // file that does not exist is still an error — silently ignoring a typo
  // would look like a protocol failure.
  if (!skip_join) {
    struct Need {
      const char* flag;
      std::string path;
    } needs[] = {{"--90-template", flow.join90},
                 {"--8a-template", flow.ticket8a}};
    for (const Need& n : needs) {
      if (!args.has(n.flag)) continue;
      if (rbx::fileReadable(rbx::resolveDataPath(n.path))) continue;
      fprintf(stderr, "join: %s was given but %s is not readable.\n", n.flag,
              n.path.c_str());
      return 2;
    }
  }

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
    if (!require_job.empty()) {
      const std::string actual = jr.reply.is_object()
                                     ? jr.reply.value("jobId", std::string())
                                     : std::string();
      if (actual != require_job) {
        fprintf(stderr,
                "raknet: job mismatch: wanted %s, joined %s (exiting 3)\n",
                require_job.c_str(), actual.c_str());
        return 3;
      }
      printf("raknet: job pin confirmed %s\n", actual.c_str());
    }
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
        // The join service mints these from the *request's* declared client
        // identity (the UA in join.cpp), not from the RakNet join body. Print
        // them so a session's platform is observable rather than inferred —
        // RBX_JOIN_UA is the knob that changes them.
        printf("join: session platform PlatformTypeId=%d DeviceType=%d\n",
               sd.value("PlatformTypeId", -1), sd.value("DeviceType", -1));
        // The other half of the identity: what the RCC is told in 0x8a. These
        // two lines must agree, or the session describes a client that does not
        // exist (RBX_JOIN_UA / RBX_JOIN_PLATFORM change them).
        printf("join: 0x8a platform=%s ua=%s\n", rbx::joinPlatform(),
               rbx::userAgent());
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
  rbx::set_local_user_name(js.value("UserName", std::string()));
  fprintf(stderr, "[id] local uid=%lld name='%s'\n",
          (long long)js.value("UserId", 0LL),
          js.value("UserName", std::string()).c_str());

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

  // Sound playback is host-backed. A missing device is a warning, not a
  // handshake failure: Sound:Play must simply stay false and say why.
  struct AudioStop {
      ~AudioStop() { rbxclient::audio::stop(); }
  } audio_stop;
  if (feature_set.audio) {
    std::string aerr;
    if (!rbxclient::audio::start(cookie, &aerr))
      fprintf(stderr, "raknet: audio unavailable: %s\n", aerr.c_str());
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
  // Visibility for the join-target ports: they diverge per place (NetStackPort
  // != UdmuxEndpoints[0].Port != ServerConnections[0].Port), and a wrong pick
  // shows up only as an RCC verdict, never as a local error.
  printf("raknet: target udmux=%s:%u netstack=%u server=%u rcc=%s\n",
         target.udmux_host.c_str(), unsigned(target.udmux_port),
         unsigned(target.rcc_port), unsigned(target.server_port),
         target.rcc_host.c_str());
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

  // Upstream stream + the executor bridge. Installed here, before any join,
  // so a --exec script that lands with the first snapshot already sees it.
  struct RaknetSender : rbxclient::UpstreamSender {
    rbxclient::RaknetClient* c = nullptr;
    rbxclient::SendPath* path = nullptr;
    bool send_unreliable(const uint8_t* d, size_t n) override {
      return c->sendUnreliable(d, n);
    }
    bool send_ordered(const uint8_t* d, size_t n) override {
      return c->sendJoinMessage(d, n, /*ordered=*/true) > 0;
    }
    bool ready() const { return c && c->data_ready(); }
  };
  RaknetSender sender;
  sender.c = &client;
  rbxclient::SendPath send_path(&sender, {}, log);
  sender.path = &send_path;
  const rbx::send::Api kSendApi{
      &sender,
      [](void* ctx, int m, int v) {
        return static_cast<RaknetSender*>(ctx)->path->hold(uint8_t(m), uint8_t(v));
      },
      [](void* ctx) { return static_cast<RaknetSender*>(ctx)->path->release(); },
      [](void* ctx, int m, int v) {
        return static_cast<RaknetSender*>(ctx)->path->queue_control(uint8_t(m), uint8_t(v));
      },
      [](void* ctx, const void* d, size_t n, bool ordered) {
        return static_cast<RaknetSender*>(ctx)->path->raw(
            static_cast<const uint8_t*>(d), n, ordered);
      },
      [](void* ctx) { return static_cast<RaknetSender*>(ctx)->ready(); },
  };
  rbx::send::install(&kSendApi);

  // The replication parser is transport-independent: RakNet hands it the
  // same application messages (0x83 chains, 0x97 join data) that the
  // RbxTransport path feeds from its channel stream. LiveRuntime adds the
  // varint framing the stream transport supplies on its own.
  std::string job_id = job;
  if (job_id.empty() && js.is_object() && js.contains("JobId"))
    job_id = js.value("JobId", std::string());
  // The join script carries the RCC job id as GameId; JobId is usually absent.
  if (job_id.empty() && js.is_object() && js.contains("GameId"))
    job_id = js.value("GameId", std::string());
  // The 0x9B challenge is solved against the session's GameId (the RCC job
  // id), exactly like the RbxTransport session; --job-id only overrides it.
  // Reaching it at all requires the 0xA7 queue that sendJoinFlow emits.
  std::string challenge_job = job_id;
  if (js.is_object() && js.contains("GameId"))
    challenge_job = js.value("GameId", challenge_job);
  std::unique_ptr<rbxclient::LiveRuntime> live;
  // Client-owned avatar: Jolt prediction, WASD/jump and the 2016 camera.
  // Persists across rejoins; attach() is keyed on the Instance pointers and
  // detaches cleanly when a new world replaces the old one.
  rbxclient::LocalPlayer local_player;
  // --exec live GUI proof: RBX_GUI_TEST drives the same host-input queue the
  // SDL backend uses, so this exercises hit-testing, Lua events, text editing
  // and wheel scrolling against a real replicated place.
  const bool gui_test = std::getenv("RBX_GUI_TEST") != nullptr;
  if (gui_test)
    rbx::input::set_test_mode(true);
  int gui_test_frame = 0;
  int gui_test_session = 0;
  int gui_test_seen_session = -1;
  auto guiTestTick = [&]() {
    if (!gui_test || !live || !live->has_world())
      return;
    // A rejoin gets a new Engine/world and reruns --exec. Restart the input
    // script for that world so the same assertions exercise the new GUI tree.
    if (gui_test_seen_session != gui_test_session) {
      gui_test_seen_session = gui_test_session;
      gui_test_frame = 0;
    }
    if (++gui_test_frame < 60)
      return;
    const int f = gui_test_frame;
    const float bx = 180.0f, by = 88.0f;   // ProbeButton center
    const float tx = 220.0f, ty = 152.0f;  // ProbeInput center
    const float sx = 220.0f, sy = 285.0f;  // ProbeScroll center
    if (f == 60) {
      const auto cmds = rbxclient::collectGui(live->root(), 1280, 720,
                                              &rbx::gui::view());
      const rbx::Instance* hit = rbxclient::hitTestGui(
          live->root(), 1280, 720, bx, by);
      printf("gui-probe-driver: cmds=%zu hit=%s\n", cmds.size(),
             hit ? hit->name().c_str() : "null");
      for (size_t i = 0; i < cmds.size() && i < 8; ++i)
        printf("gui-probe-driver: cmd[%zu] kind=%d %.0f,%.0f %.0fx%.0f %s\n",
               i, int(cmds[i].kind), cmds[i].x, cmds[i].y, cmds[i].w,
               cmds[i].h,
               cmds[i].source ? cmds[i].source->name().c_str() : "-");
      rbx::input::publish_mouse(bx, by, false, false, false, 1280, 720);
      printf("gui-probe-driver: hover button\n");
    } else if (f == 61) {
      const rbx::Instance* hv = rbx::gui::view().hovered;
      printf("gui-probe-driver: hover=%s\n",
             hv ? hv->name().c_str() : "null");
      rbx::input::publish_mouse(bx, by, true, false, false, 1280, 720);
    } else if (f == 62) {
      rbx::input::publish_mouse(bx, by, false, false, false, 1280, 720);
      printf("gui-probe-driver: clicked button\n");
    } else if (f == 63) {
      rbx::input::publish_mouse(tx, ty, false, false, false, 1280, 720);
    } else if (f == 64) {
      rbx::input::publish_mouse(tx, ty, true, false, false, 1280, 720);
    } else if (f == 65) {
      rbx::input::publish_mouse(tx, ty, false, false, false, 1280, 720);
    } else if (f == 66) {
      rbx::input::publish_text("Boblox GUI");
      printf("gui-probe-driver: typed text\n");
    } else if (f == 67) {
      rbx::input::publish_key(306, true); // ctrl
      rbx::input::publish_key(65, true);  // A
      rbx::input::publish_key(65, false);
      rbx::input::publish_key(306, false);
      printf("gui-probe-driver: selected all\n");
    } else if (f == 69) {
      rbx::input::publish_key(13, true); // Enter
    } else if (f == 70) {
      rbx::input::publish_key(13, false);
      printf("gui-probe-driver: submitted text\n");
    } else if (f == 71) {
      rbx::input::publish_mouse(sx, sy, false, false, false, 1280, 720);
    } else if (f == 72) {
      for (int i = 0; i < 6; ++i)
        rbx::input::publish_wheel(sx, sy, 0, -10);
      printf("gui-probe-driver: wheel scroll\n");
    } else if (f == 74) {
      rbx::input::publish_mouse(tx, ty, false, false, false, 1280, 720);
    } else if (f == 75) {
      rbx::input::publish_mouse(tx, ty, true, false, false, 1280, 720);
    } else if (f == 76) {
      rbx::input::publish_mouse(tx, ty, false, false, false, 1280, 720);
    } else if (f == 77) {
      rbx::input::publish_key(306, true);
      rbx::input::publish_key(65, true);
      rbx::input::publish_key(65, false);
      rbx::input::publish_key(306, false);
      printf("gui-probe-driver: text focused with selection\n");
    } else if (f == 78) {
      rbx::input::publish_mouse(bx, by, false, false, false, 1280, 720);
      printf("gui-probe-driver: hover restored for capture\n");
    }
  };
  auto tickLocalPlayer = [&](double dt) {
    if (!live || !live->has_world() ||
        feature_set.physics != rbx::features::PhysicsMode::Client) {
      local_player.detach();
      return;
    }
    rbx::Instance* root = const_cast<rbx::Instance*>(live->root());
    rbx::Instance* ws = root ? root->find_first_child_of_class("Workspace")
                             : nullptr;
    rbx::Instance* character = live->local_character_instance();
    if (ws && character) {
      local_player.set_network_owner_id(live->local_network_owner_id());
      local_player.tick(dt, ws, character);

      // Owner CFrame replication.  Native clients upload poses for the
      // unanchored assemblies the server granted them; the RCC already sends
      // the same property writes in the opposite direction for bodies we do
      // not own.  Cap the per-tick budget so a collapsing map cannot turn a
      // frame into an unbounded burst.
      static auto next_phys_upload = std::chrono::steady_clock::now();
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_phys_upload) {
        next_phys_upload = now + std::chrono::milliseconds(50);
        auto updates = local_player.take_physics_updates();
        std::vector<rbxclient::batch::CFrameWrite> writes;
        for (const auto& up : updates) {
          if (writes.size() >= 64) break;
          uint32_t ref = 0;
          if (!live->instance_ref(up.part, &ref) || ref == 0) continue;
          const uint16_t prop = live->property_id(up.part, "CFrame");
          if (!prop) continue;
          writes.push_back({ref, prop, &up.cframe});
        }
        if (!writes.empty()) {
          const auto b = rbxclient::batch::cframe_batch(writes);
          if (!client.sendJoinMessage(b.data(), b.size(), /*ordered=*/true))
            writes.clear();
        }
        if (!writes.empty()) {
          static long long total = 0;
          total += (long long)writes.size();
          if (total == (long long)writes.size())
            printf("raknet: owner physics upload started (%d bodies this tick, "
                   "reliable 0x83/03 CFrame writes)\n",
                   (int)writes.size());
        }
      }
    }
    else
      local_player.detach();
  };
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
  // The sink is named so the deterministic rejoin harness can feed the exact
  // same 0x15 payload the server sends; production only gets it from RakNet.
  std::function<void(const uint8_t*, size_t)> appMessage;
  if (!skip_join) {
    // Join-only: no LiveRuntime at all. Nothing parses record batches, nothing
    // allocates the Engine, and the message sink below still answers the
    // challenge, acks the 0x84 marker and notices a disconnect.
    appMessage = [&client, &live, &challenge_job, &challenge_seen,
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
            : code == 0x010c ? "DisconnectOnRemoteSysStats"
            : code == 0x010d ? "DisconnectHashTimeout"
            : code == 0x0130 ? "AndroidAnticheatKick"
            : code == 0x0132 ? "AndroidRootedKick"
            : code == 0x0136 ? "DisconnectCollaboratorNotAgeVerified"
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
    };
    client.setMessageSink(appMessage);
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
  const std::string input_mode =
      session.input.empty() ? "native" : session.input;
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
  // The RCC's LoadCharacter decision is a per-session lottery. A live world
  // with no local Character is a dead end for scripts and physics, so give
  // that session up for a fresh one instead of idling in it forever. This is
  // deliberately a rejoin, not a fabricated character or a replay of another
  // session's hydration record (both have been rejected on the wire).
  int spawn_retries = 0;
  // Opt-in: a slow but healthy join must not be torn down just because the
  // RCC has not linked the avatar yet. --spawn-retry N / BOBLOX_SPAWN_RETRY /
  // the config file choose the number of fresh sessions; the age gate is
  // --spawn-retry-after (default 30 s). A session that ever linked a
  // Character never retries (see AvatarWatch). The legacy RBX_SPAWN_RETRY
  // variable still wins for A/B runs.
  int spawn_retry_limit = 0;
  if (session.spawn_retry >= 0) spawn_retry_limit = session.spawn_retry;
  if (const char* sr = getenv("RBX_SPAWN_RETRY"))
    spawn_retry_limit = std::max(0, atoi(sr));
  const int spawn_retry_after_s = session.spawn_retry_after;
  // Deterministic DisconnectRejoin harness: after the world is live, feed the
  // same 0x15 body the RCC sends. This exercises the real sink, teardown,
  // reconnect and second-world path without waiting for a server-side job
  // rotate. One-shot per process so a successful run can continue afterwards.
  long long force_rejoin_ms = 0;
  if (const char* fr = getenv("RBX_FORCE_REJOIN_MS"))
    force_rejoin_ms = std::max(0, atoi(fr));
  bool force_rejoin_done = false;
  // Covers the join misses (a challenge the RCC never issues) plus one
  // attempt per allowed rejoin.
  for (int attempt = 0; attempt <= 11 && !g_stop; ++attempt) {
  const auto attempt_t0 = std::chrono::steady_clock::now();
  auto injectRejoin = [&]() -> bool {
    if (force_rejoin_done || force_rejoin_ms <= 0 || !live ||
        !live->has_world() || !appMessage)
      return false;
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - attempt_t0);
    if (age.count() < force_rejoin_ms)
      return false;
    force_rejoin_done = true;
    printf("raknet: TEST injecting 0x15 DisconnectRejoin after %lldms\n",
           static_cast<long long>(age.count()));
    const uint8_t payload[5] = {0x15, 0x00, 0x00, 0x01, 0x14};
    appMessage(payload, sizeof payload);
    return session_dead;
  };
  if (session_up) break;
  if (attempt > 0) {
    printf("raknet: rejoining with a fresh session (attempt %d of 3)\n", attempt + 1);
    // Jolt and the camera still point at the previous Engine world. Drop them
    // before the old LiveRuntime is replaced; the new world re-attaches on its
    // first tick.
    local_player.detach();
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
      if (job_id.empty() && js.is_object() && js.contains("GameId"))
        job_id = js.value("GameId", std::string());
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
  // One LiveRuntime per network session. The previous world, its Luau state
  // and its GUI tree belong to the old RCC and must be gone before the new
  // snapshot is parsed; otherwise finalizeSnapshot sees an already-applied
  // world and the rejoin silently keeps rendering stale instances.
  if (!skip_join && world_enabled) {
    // The old controller was detached in the attempt>0 branch above; replace
    // the Engine world now, before this session's snapshot can arrive.
    live = std::make_unique<rbxclient::LiveRuntime>(job_id, log,
                                                    feature_set.luau,
                                                    feature_set.gui);
    // The 141-byte form needs a real local PlayerControlState. Let the
    // runtime create and drive it only when that form is enabled.
    live->set_control_state_stream(getenv("RBX_A6_LONG") != nullptr);
    ++gui_test_session;
    if (!exec_source.empty())
      live->set_exec_source(exec_source);
    // The native client creates Players.LocalPlayer (and PlayerGui) at join;
    // give LiveRuntime the authenticated identity so replicated scripts can
    // use it from their first line even when no Player record is sent.
    live->setLocalUser(js.value("UserId", 0LL),
                       js.value("UserName", std::string()),
                       js.value("DisplayName", std::string()));
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
      // A worldless session has no Character to bootstrap; sending the
      // character-state record would ask the RCC to spawn one we never parse.
      if (world_enabled && !getenv("RBX_NO_8304")) {
        static const uint8_t kSpawnBootstrap[55] = {
            0x83, 0x04, 0x97, 0x05, 0x00, 0x00, 0x05, 0x16, 0x9f, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9f, 0x0d, 0x00, 0x00, 0x9f, 0x45, 0x70, 0x42, 0xc7, 0xf0, 0x3c, 0x42, 0x77, 0x8a, 0xe0, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x98, 0x0d, 0x00, 0x00, 0x80, 0xfb, 0x1a, 0x44, 0x23, 0x06, 0x01, 0x00, 0x00, 0x00};
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
        if (const char* raw5 = getenv("RBX_8305_RAW")) {
          const auto alt5 = rbx::readFileVec(raw5);
          if (alt5.size() >= 2 && alt5[0] == 0x83) {
            const int n5 = client.sendJoinMessage(alt5.data(), alt5.size());
            printf("raknet: raw 0x83/05 sent (%zuB, %d datagrams)\n",
                   alt5.size(), n5);
          } else {
            fprintf(stderr, "raknet: RBX_8305_RAW %s is not a 0x83 message\n",
                    raw5);
          }
        }
        if (getenv("RBX_8305_JOIN")) {
          static const uint8_t kSpawnPhysics[45] = {
              0x83, 0x05, 0x16, 0x9f, 0x0d, 0x00, 0x00, 0x00, 0x00,
              0x00, 0x00, 0x9f, 0x0d, 0x00, 0x00, 0x9f, 0x45, 0x70,
              0x42, 0xc7, 0xf0, 0x3c, 0x42, 0x77, 0x8a, 0xe0, 0x41,
              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
              0x98, 0x0d, 0x00, 0x00, 0x80, 0xfb, 0x1a, 0x44, 0x00};
          const int n5 = client.sendJoinMessage(kSpawnPhysics,
                                                sizeof kSpawnPhysics);
          printf("raknet: 0x83/05 spawn physics (%zuB, %d datagrams)\n",
                 sizeof kSpawnPhysics, n5);
        }
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
      if (world_enabled && attrs_8304 && *attrs_8304 && *attrs_8304 != '0' &&
          js.is_object() && js.contains("UserId") &&
          getenv("RBX_NO_HYDRATION") == nullptr) {
        std::string hj, hs, herr;
        if (rbx::fetchPlayerHydrationRaw(js["UserId"].get<long long>(), cookie,
                                        &hj, &hs, &herr)) {
          const auto msg = rbxmsg::buildHydrationDefault(hj, hs);
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
    // 240 Hz samples for the four actions in each 60 Hz long 0xA6 frame. The
    // ring is drained on the input tick; when it has fewer than four samples
    // the encoder repeats the newest one instead of inventing a direction.
    rbxclient::input::ControlSampleRing a6_samples;
    auto next_control_sample = std::chrono::steady_clock::now();
    uint32_t a6_step = 0x14c;
    int a6_sent = 0;
    int ack83_sent = 0;
    // 0x83/0x06 timing records. The native sends one every ~263 ms (3.8/s,
    // its second most frequent post-join frame) and this client never sent
    // any before 2026-11-07. Measured 3.67/s at 49 B after the change, against
    // the native's 3.80/s at 49 B, and two 420 s soaks (control and this
    // stream) both ran clean, so it is on by default and RBX_NO_06=1 removes
    // it for A/B runs.
    const bool send06 = getenv("RBX_NO_06") == nullptr;
    int ack06_sent = 0;
    auto next_ack06 = std::chrono::steady_clock::now();
    int a6_stage = 0;
    auto next_clock_tick = std::chrono::steady_clock::now();
    auto next_clock_ping = std::chrono::steady_clock::time_point{};
    uint64_t ping_rng =
        uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) ^
        (uint64_t(getpid()) << 32);
    auto next_ping_nonce = [&ping_rng]() {
      ping_rng ^= ping_rng << 13;
      ping_rng ^= ping_rng >> 7;
      ping_rng ^= ping_rng << 17;
      return uint32_t(ping_rng);
    };
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
    // Liveness: the RCC never sends a disconnect when it ends a session, so a
    // silent socket must be reported instead of looking like a quiet session.
    long long quiet_announced = 0;
    bool replicated_logged = false;
    // The latch lives in AvatarWatch so the retry policy is testable without
    // a socket: once a replicated Character linked, a later gap is a respawn
    // or model swap, not a failed join.
    rbx::client::AvatarWatch avatar_watch;
    bool spawn_retry = false;
    auto spawn_retry_wanted = [&]() -> bool {
      if (!live || !live->has_world()) return false;
      const double age = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - run_start)
                             .count();
      return avatar_watch.retryDue(true, age, double(spawn_retry_after_s),
                                   spawn_retries, spawn_retry_limit,
                                   join_path.empty());
    };
    bool local_logged = false;
    bool late04_sent = false;
    bool a1_held_note = false;
    // Input cadence. The earlier 75 ms default came from a 34 s capture in
    // which the client was still loading (13.3 0xA6/s). A same-job round
    // measured over 194 s of steady state (tools/parity_round.sh, native
    // `dgram_*` deduped by (channel, ordering index)) gives the real numbers:
    //   0xA6        59.3/s  (p50 16 ms)     0x83/23 clock  24.5/s  (p50 41 ms)
    //   0x83/0A      9.3/s                  0x83/06        3.9/s
    //   0x83/05      3.0/s                  0x83/07        1.1/s
    //   0x83/1C      0.9/s                  0xA1           0.3/s
    // The clock carried by 0xA6 is a 240 Hz step counter: it advanced 39176
    // units over 164 s = 239/s, i.e. +4 per input frame at 60 Hz. (An earlier
    // note misread that as "4/ms" and derived it from elapsed milliseconds,
    // which ran our pair 16.7x fast at 4000/s; the same-job capture shows both
    // slots advancing by exactly 4 per frame for 48,013 frames.)
    // 60 Hz exactly. A 16 ms integer period ran at 62.5/s, which also drove
    // the 0xA6 step pair at 250/s instead of the native's 238-240/s; 16667 us
    // is 60.0/s, and 4 steps per frame is the native's measured rate.
    int input_us = 16667;
    if (const char* im = getenv("RBX_INPUT_MS"))
      input_us = std::max(1000, atoi(im) * 1000);
    if (const char* iu = getenv("RBX_INPUT_US"))
      input_us = std::max(1000, atoi(iu));
    // Keep the (currently unticked) SendPath stream on the same cadence so a
    // future tick() cannot reintroduce a 75 ms route stream next to this one.
    send_path.set_cadence_ms(input_us / 1000);
    // Diagnostics only: the RCC disconnects us with 0x15 01 04 ("error while
    // receiving data") once the world is up, so each post-join stream is
    // gated so the offending one can be bisected by env.
    const bool no_hb = getenv("RBX_NO_HB") != nullptr;
    const bool no_clock = getenv("RBX_NO_CLOCK") != nullptr;
    const bool no_clock_ping = getenv("RBX_NO_CLOCK_PING") != nullptr;
    int clock_ping_batches = 0;
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
    // Opt-in long ControlState form. It needs a real PlayerControlState
    // UnitVector3 and a live ref, so a client with no PlayerModule keeps the
    // accepted 37-byte frame instead of emitting a fabricated direction.
    const bool a6_long = getenv("RBX_A6_LONG") != nullptr;
    const uint32_t a6_long_ref = getenv("RBX_A6_REF")
                                     ? uint32_t(strtoul(getenv("RBX_A6_REF"),
                                                        nullptr, 0))
                                     : 0;
    bool a6_long_logged = false;
    const bool no_a1 = getenv("RBX_NO_A1") != nullptr;
    auto next_camera = std::chrono::steady_clock::now();
    // 0x83/0a per-instance ack items. Native rate is 3.3 frames/s, usually
    // one or two records plus an optional 0x23 tick; emitting one 14 B frame
    // every 123 ms was both too fast (8/s) and missing the chain terminator.
    auto next_ack0a = std::chrono::steady_clock::now();
    size_t ack0a_pick = 0;
    uint64_t ack0a_frames = 0;
    auto sampleControl = [&](std::chrono::steady_clock::time_point now) {
      if (!a6_long || !live || !live->has_world()) return;
      if (now < next_control_sample) return;
      // A 60 Hz loop must still hand the ring four 4.167 ms slots. Read the
      // state once per due slot; if the loop stalled, resync instead of
      // replaying an unbounded backlog of stale samples.
      int budget = 32;
      do {
        next_control_sample += std::chrono::microseconds(4167);
        double x = 0.0, y = 0.0, z = 0.0;
        std::string field;
        if (!live->local_control_state_unit_vector(&x, &y, &z, &field)) {
          a6_samples.clear();
          next_control_sample = now + std::chrono::microseconds(4167);
          break;
        }
        a6_samples.push(x, y, z);
      } while (now >= next_control_sample && --budget > 0);
      if (now > next_control_sample + std::chrono::milliseconds(50))
        next_control_sample = now + std::chrono::microseconds(4167);
    };
    auto pumpInput = [&]() {
      const auto now = std::chrono::steady_clock::now();
      sampleControl(now);
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
        next_input += std::chrono::microseconds(input_us);
        const int idx = a6_stage < 3 ? a6_stage : 3;
        // One frame, one step: the native bumps both slots by 4 per frame, so
        // a catch-up burst advances the pair once per emitted frame instead of
        // repeating a sample.
        const uint32_t step = a6_step;
        a6_step += 4;
        // Once the world is live the native stops sending the 34-byte join
        // shape and holds the 37-byte steady frame for the rest of the
        // session (48,013 of 48,041 frames). Emitting the join shape forever
        // was the last byte-level 0xA6 difference.
        // Join-only modes never build a world, but the native's steady frame
        // does not depend on one; send it so a count-only session looks like a
        // live client instead of looping the join ladder forever.
        const bool steady =
            ((live && live->has_world()) || !world_enabled) && !control_input;
        std::vector<uint8_t> frame;
        if (steady) {
          double x = 0.0, y = 0.0, z = 0.0;
          std::string field;
          const uint32_t ref = a6_long_ref
                                   ? a6_long_ref
                                   : (live ? live->local_player_ref() : 0);
          if (a6_long && ref && live &&
              live->local_control_state_unit_vector(&x, &y, &z, &field)) {
            double dirs[4][3];
            if (a6_samples.ready()) {
              a6_samples.fillFour(dirs);
            } else {
              for (int k = 0; k < 4; ++k) {
                dirs[k][0] = x;
                dirs[k][1] = y;
                dirs[k][2] = z;
              }
            }
            frame = rbxclient::input::longFrame(step, step, ref, dirs);
            if (!frame.empty() && !a6_long_logged) {
              a6_long_logged = true;
              printf("raknet: 0xA6 long ControlState frame started "
                     "(ref=%u field=%s, 141B)\n",
                     ref, field.c_str());
            }
          }
          if (frame.empty())
            frame = rbxclient::input::steadyFrame(step, step);
        } else {
          frame = a6Frame(idx, step);
        }
        if (!no_a6 && !frame.empty() &&
            client.sendUnreliable(frame.data(), frame.size()) && ++a6_sent == 1)
          printf("raknet: 0xA6 input stream started (native route frames)\n");
        ++a6_stage;
      }
      // Do not replay an unbounded backlog after a stall (join load, a big
      // replication batch); resync the schedule instead. The threshold has to
      // be well above one frame interval: a heavy 1818 world makes single
      // iterations take longer than 400 ms, and at 400 ms *every* iteration
      // took this branch and dropped its backlog, which pinned the stream at
      // one frame per iteration (measured 31/s against the native's 60/s in
      // the 700 s round of 2026-11-07). The per-call budget above still bounds
      // a catch-up burst, so 2 s only discards a genuinely dead schedule.
      if (now > next_input + std::chrono::milliseconds(2000))
        next_input = now + std::chrono::microseconds(input_us);
      // Native 0xA1 camera frame: `a1 <u32le clock>` + a 39-byte body. In the
      // 34 s capture every frame carries the same body (only the clock
      // changes). The body's u24 instance reference is session-specific; the
      // capture id only works in the job it came from. Substituting the live
      // character model, HumanoidRootPart, or Player id was tested on 1818
      // and all three were rejected with 0x15 01 04, so the frame remains
      // gated until the exact reference semantics are recovered. The native
      // sends one per ~730 ms.
      {
        // RBX_A1_OWN=1 addresses the frame with our own Player ref instead of
        // a capture's: the A1 stream is the client's state/focus report, and
        // we have never sent one in a session of our own.
        const bool a1_own = getenv("RBX_A1_OWN") != nullptr;
        const uint32_t kA1RefId = a1_own && live ? live->local_player_ref()
                                                 : 0x0512b6u;
        if (!no_a1 && now >= next_camera) {
          next_camera = now + std::chrono::milliseconds(730);
          if (live && live->has_world() &&
              (a1_own ? kA1RefId != 0 : live->hasInstance(kA1RefId))) {
            const std::vector<uint8_t> a1 =
                rbxmsg::buildA1Camera(uint32_t(send_path.step()), kA1RefId);
            if (!a1.empty()) client.sendUnreliable(a1.data(), a1.size());
            if (a1_own && !a1_held_note) {
              a1_held_note = true;
              printf("raknet: 0xA1 state frames sent with own Player ref "
                     "0x%x\n", kA1RefId);
            }
          } else if (!a1_held_note) {
            a1_held_note = true;
            printf("raknet: 0xA1 camera frame held back — its capture id "
                   "(0x%06x) is not in this job's world\n", kA1RefId);
          }
        }
      }
      // Native pre-world physics stream: the reference sends 0x83/05 at
      // ~0.4 s from join, before the world/character is available. Probe with
      // the same initial transform.
      if (getenv("RBX_8305_JOIN_STREAM")) {
        static auto next_phys = std::chrono::steady_clock::now();
        if (now >= next_phys) {
          next_phys = now + std::chrono::milliseconds(400);
          const uint32_t clk = rbxclient::batch::net_clock_ms();
          const auto b = rbxclient::batch::physics_batch_clocked(
              clk, clk, clk + 3, 60.067f, 47.235f, 28.067f, 0u, 0.0f);
          client.sendUnreliable(b.data(), b.size());
          client.sendJoinMessage(b.data(), b.size(), /*ordered=*/true);
        }
      }
      // The native pushes its signed player attributes (0x83/0x04) on the
      // join path (above), not per input tick.
      // Liveness/clock streams. In a full-world session they start once the
      // world is live (the 0x05 ack reports the spawned Character). In
      // join-only mode they must start at join: 0x83/0x05 is exactly what
      // keeps the RCC's 300 s liveness timer from ending a worldless session.
      if ((live && live->has_world()) || !world_enabled) {
        // 0x83/0a: the native sends 3.32 frames/s with 1.38 records per
        // frame, not two records every frame.  The dominant shape is the
        // 21-byte one-record-plus-tick frame (395/776); the 34-byte pair is
        // only 167/776 and normally repeats the same ref with has-value=0.
        // Send one record at 300 ms, and add the second record every fourth
        // frame (4.17 records/s against the native's 4.58; 25% two-record
        // frames against the native's 25.1% in the fresh round).
        // Reliable-ordered, as the native sends it.  Live refs only: a ref
        // from another session
        // is an instance the RCC cannot resolve (its 0x0105 receive error).
        if (live && getenv("RBX_NO_0A") == nullptr) {
          const auto parts = live->local_character_parts();
          int budget = 4;
          while (now >= next_ack0a && budget-- > 0) {
            next_ack0a += std::chrono::milliseconds(300);
            // Prefer the instance the server just sent state for: that is what
            // the ack means. Fall back to the character's parts while the
            // state queue is empty.
            uint32_t ref = 0;
            // Default: the character's own parts, which is the instance the
            // client owns and the shape that survived longest. RBX_0A_SERVER=1
            // acks the ref of the record the server just sent instead — the
            // literal meaning of the item, but it names instances the RCC
            // never handed us, and every soak of it was shorter.
            const bool from_server = getenv("RBX_0A_SERVER") != nullptr;
            if (from_server && live->takeAppliedRef(&ref) && ref != 0) {
              // ack the server-driven ref
            } else if (!parts.empty()) {
              ref = parts[ack0a_pick % parts.size()].ref;
            }
            if (ref == 0) continue;
            ++ack0a_pick;
            ++ack0a_frames;
            const uint32_t clk = uint32_t(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - run_start).count() * 4);
            std::vector<rbxclient::batch::Ack0a> acks;
            rbxclient::batch::Ack0a a;
            a.ref = ref;
            a.prop = 0x1607; // native's steady-state ack property
            a.has_value = true;
            a.value = 0;
            a.clock = clk;
            acks.push_back(a);
            // The native's 34-byte form repeats the same ref with no value;
            // it is not a second part.  Every fourth frame keeps the mix at
            // ~1.25 records/frame instead of the old 2.0.
            if (ack0a_frames % 4 == 0) {
              rbxclient::batch::Ack0a b;
              b.ref = a.ref;
              b.prop = 0x1607;
              b.has_value = false;
              b.clock = clk;
              acks.push_back(b);
            }
            const auto f = rbxclient::batch::ack0a_batch(
                acks, rbxclient::batch::net_tick_240());
            client.sendReliable(f.data(), f.size());
          }
          // Do not replay a backlog after a stall; resync like the input
          // stream does.
          if (now > next_ack0a + std::chrono::seconds(2))
            next_ack0a = now + std::chrono::milliseconds(300);
        }
        // The native's post-join control stream, decrypted from /tmp/nat_live
        // (2026-10-30): a 19-byte 0x83/0x07 opener, 7-byte 0x83/0x23 input
        // clocks at ~6/s, and a 0x83/0x05 physics ack in two forms (45 B
        // unreliable every ~2.5 s, plus a 50 B reliable copy every ~5 s).
        if (!hb_started) {
          hb_started = true;
          next_clock_ping = now + std::chrono::milliseconds(2000);
          const char* no_op = getenv("RBX_NO_OPENER");
          if (canned83 && !no_hb && !(no_op && *no_op && *no_op != '0')) {
            const auto ev = rbx::hexDecode(
                "830701b612050013100000e04023b401000000");
            client.sendJoinMessage(ev.data(), ev.size());
            printf("raknet: 0x83/0x07 control opener sent (19B)\n");
          }
        } else {
          // Native clock-sync burst: 0x51..0x53 (9B) and 0x54..0x56 (25B)
          // every 5.004s, all carrying one client millisecond clock. The
          // 25-byte family also carries the system monotonic clock (the
          // native's a-b offset is the machine uptime) and an opaque nonce.
          // These are separate from the 0x83/0x23 tick stream.
          if (!no_clock_ping && now >= next_clock_ping) {
            next_clock_ping = now + std::chrono::milliseconds(5000);
            const uint64_t session_ms = uint64_t(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - run_start)
                    .count());
            const uint64_t system_ms = uint64_t(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            const uint32_t clk = uint32_t(session_ms);
            int sent = 0;
            for (uint8_t id : {0x51, 0x52, 0x53}) {
              const auto p = rbxclient::batch::clock_ping9(id, clk);
              sent += client.sendUnreliable(p.data(), p.size()) ? 1 : 0;
            }
            for (uint8_t id : {0x54, 0x55, 0x56}) {
              const auto p = rbxclient::batch::clock_ping25(
                  id, system_ms, session_ms, next_ping_nonce());
              sent += client.sendUnreliable(p.data(), p.size()) ? 1 : 0;
            }
            if (clock_ping_batches++ == 0 && sent > 0)
              printf("raknet: 0x51..0x56 clock-sync burst started "
                     "(6 pings/5s)\n");
          }
          if (!no_clock) {
            // 0x83/0x23 is reply-driven: one clock out per server clock in.
            // Measured in the same job, the server sent 10.9/s and the native
            // answered 12.1/s, so a free-running timer is always the wrong
            // rate (ours ran at 28-31/s). A 250 ms floor keeps a tick going
            // when the server is momentarily quiet.
            // A 0x83 frame is a *container*: one 0x83 byte, then whole
            // records back to back (native, same job):
            //   83 23 f8050000 0a 01 <ref> <prop> <flag> [<value>] <clock> 00
            //   83 23 18040000 23 1c040000 00
            //   83 06 <record> 05 <record> 23 <clock> 00
            // Our first version emitted exactly one record per frame, which is
            // why frame counts and per-frame record mixes could never line up.
            // The 0x0a record is the per-instance "applied up to <clock>" ack
            // the native sends ~22/s against live refs; ours was absent.
            auto sendClock = [&]() {
              // The 0x23 value is the client's 240 Hz tick, not a scaled
              // millisecond count. Measured on a native capture (place 1818,
              // 813 s): the 7-byte clock frame rises 304 -> 193804 = 239.1/s,
              // and the same value appears in the 0xA1 packet header and in
              // the 23 item of the 0x83/04 ready frame. This used to write
              // ms*4 = 4000/s, i.e. a clock running 16.7x faster than the real
              // client's, which any server-side time comparison sees.
              clock_step = rbxclient::batch::net_tick_240();
              std::vector<uint8_t> f;
              f.reserve(96);
              f.push_back(0x83);
              // 0x0a is not grafted onto this frame any more: it has its own
              // ~4/s schedule below, with up to two records and one tick per
              // frame. Up to eight items per reply at ~30 replies/s measured
              // 1,488 items in 50 s (~238/s) against the native's 22.6/s, and
              // an over-rate upstream is the one thing a server can shed a
              // session for.
              f.push_back(0x23);
              f.push_back(uint8_t(clock_step & 0xff));
              f.push_back(uint8_t((clock_step >> 8) & 0xff));
              f.push_back(uint8_t((clock_step >> 16) & 0xff));
              f.push_back(0x00);
              f.push_back(0x00);
              client.sendJoinMessage(f.data(), f.size());
            };
            // The same-job 210 s round measured the native at 24.40/s (gap
            // p50 32 ms) and us at 12.98/s with the old 67 ms gate. A 30 ms
            // gate targets the native's rate (30 ms overshot at 27.0/s; 33 ms
            // targets 24.4/s). The server sends clock records
            // faster than one per reply, so consume the whole backlog each
            // pass — a backlog must never build — while still emitting at most
            // one frame per gate. That also keeps running on a quiet server
            // the way the native does.
            const uint64_t want = client.clock_ticks_rx();
            clock_replied = want;
            if (now >= next_clock_tick) {
              next_clock_tick = now + std::chrono::milliseconds(33);
              sendClock();
            }
          }
          if (!no_ack83 && now >= next_ack83) {
            // The same-job 210 s round measured the native at 3.24/s and this
            // path at 1.65/s with 900/1800 ms gates. Halving both gates puts
            // the measured pair rate on the native's: 45 B unreliable every
            // 450 ms plus the 50 B reliable copy (which bundles a 0x23 clock
            // record) every 900 ms. The rates are calibrated on the wire, not
            // derived from the nominal timer.
            const bool pair = now >= next_ack83_pair;
            next_ack83 = now + std::chrono::milliseconds(450);
            if (pair) next_ack83_pair = now + std::chrono::milliseconds(900);
            // Live field: the 0x05 body carries a u24 millisecond clock and the
            // character's horizontal position, both of which the captured
            // frame has from a *different* session. Replaying it verbatim is
            // accepted, but it reports a clock that never advances and a
            // position we are not at — the one thing that can make an RCC
            // decide our character is not the one it is tracking. Build it
            // from this session once we know where we are, and keep the
            // captured bytes only until then.
            std::vector<uint8_t> unrel;
            std::vector<uint8_t> rel;
            const uint32_t ms = uint32_t(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - run_start).count());
            if (live && live->has_world()) {
              const auto parts = live->local_character_parts();
              const rbxclient::LiveRuntime::LocalPart* root = nullptr;
              for (const auto& p : parts) {
                if (!p.has_cframe) continue;
                if (p.name == "HumanoidRootPart") { root = &p; break; }
                if (!root) root = &p;
              }
              if (root) {
                unrel = rbxclient::ack83::ack05(
                    ms, float(root->x), float(root->z),
                    rbxclient::ack83::kDefaultV, /*reliable=*/false);
                if (pair)
                  rel = rbxclient::ack83::ack05(
                      ms, float(root->x), float(root->z),
                      rbxclient::ack83::kDefaultV, /*reliable=*/true);
              }
            }
            // No local Character yet: still report this session's live clock
            // rather than replaying a captured frame whose clock never moves
            // and whose position belongs to another session. The fallback
            // Character supplies the real local pose as soon as it exists.
            if (unrel.empty())
              unrel = rbxclient::ack83::ack05(
                  ms, 0.0f, 0.0f, rbxclient::ack83::kDefaultV, false);
            if (rel.empty() && pair)
              rel = rbxclient::ack83::ack05(
                  ms, 0.0f, 0.0f, rbxclient::ack83::kDefaultV, true);
            if (!unrel.empty() &&
                client.sendUnreliable(unrel.data(), unrel.size()) &&
                ack83_sent++ == 0)
              printf("raknet: 0x83/0x05 physics-ack stream started "
                     "(liveness; without it the RCC ends the session at "
                     "300 s with 0x15 266)\n");
            if (!rel.empty())
              client.sendJoinMessage(rel.data(), rel.size(), /*ordered=*/true);
          }
          if (send06 && now >= next_ack06) {
            // Native cadence: 3.8/s, gap p50 404 ms. The triple is the session
            // clock (sample, receive, apply) that the adjacent 0x05 record
            // carries; `lag` is that clock negated. n/m are the two small
            // counters whose meaning is not recovered, so they stay 0 rather
            // than pretend to a count we do not keep.
            next_ack06 = now + std::chrono::milliseconds(263);
            const uint32_t uptime = uint32_t(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - run_start).count());
            const uint32_t sess = uptime & 0xffffffu;
            const auto rec = rbxclient::ack83::ack06(
                uptime, sess - 2, sess - 1, sess, 0, 0, -int32_t(sess));
            if (!rec.empty() && client.sendReliable(rec.data(), rec.size()) &&
                ack06_sent++ == 0)
              printf("raknet: 0x83/0x06 timing stream started (native 3.8/s)\n");
          }
        }
      }
      // The avatar is not part of the join snapshot: the RCC spawns the
      // character only after the client sends real ControlState input (the
      // native's builder rejects nActions == 0). These route frames are
      // keepalives, so say so once instead of leaving a character-less world
      // unexplained.
      // RBX_04_LATE=1: send the hydration 0x83/04 once the world exists, so it
      // can address OUR Player ref. The captured ref/event are another
      // session's; the native's own 04 carries a ref that belongs to its
      // session (0x14735 vs the builder's old 0x41ef).
      if (live && live->has_world() && getenv("RBX_04_LATE") && !late04_sent) {
        const uint32_t pref = live->local_player_ref();
        if (pref) {
          std::string hj, hs, herr;
          if (rbx::fetchPlayerHydrationRaw(js["UserId"].get<long long>(), cookie,
                                          &hj, &hs, &herr)) {
            const uint8_t nonce[5] = {0x96, 0xE0, 0x94, 0xEC, 0x58};
            const uint32_t fl = getenv("RBX_04_FLAGS")
                                    ? uint32_t(strtoul(getenv("RBX_04_FLAGS"),
                                                       nullptr, 0))
                                    : 0x0516u;
            const auto msg =
                rbxmsg::buildHydration(hj, hs, pref, 0x1046, nonce, fl);
            const int n = client.sendJoinMessage(msg.data(), msg.size());
            late04_sent = true;
            printf("raknet: late 0x83/04 hydration ref=0x%x event=0x1046 "
                   "json=%zuB sig=%zuB (%d datagrams)\n",
                   pref, hj.size(), hs.size(), n);
          }
        }
      }
      if (live && live->has_world()) {
        const std::string local = live->local_character();
        const bool replicated = live->local_character_replicated();
        avatar_watch.observe(replicated);
        if (!local.empty() && !local_logged) {
          local_logged = true;
          replicated_logged = replicated;
          printf("raknet: local avatar %s: %s\n",
                 replicated ? "linked" : "fallback (client-only)",
                 local.c_str());
        }
        if (replicated && !replicated_logged) {
          replicated_logged = true;
          printf("raknet: local avatar replicated: %s\n", local.c_str());
        }
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - run_start);
        if (avatar_watch.noteDue(true, double(age.count()),
                                 double(spawn_retry_after_s))) {
          printf("raknet: no replicated local avatar after %llds — the RCC "
                 "did not spawn our Character (0x83/04 spawn bootstrap "
                 "missing?)\n",
                 (long long)age.count());
          if (spawn_retry_limit > 0)
            printf("raknet: opt-in character retry armed: up to %d fresh "
                   "session(s) after %ds\n",
                   spawn_retry_limit, spawn_retry_after_s);
          else
            printf("raknet: character retry disabled (--spawn-retry N "
                   "enables it)\n");
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
      if (!live) {
        printf("raknet: t=%llds join-only: tx=%llu rx=%llu srv_ack=%llu "
               "silent=%lldms\n",
               static_cast<long long>(uptime),
               (unsigned long long)client.datagrams_tx(),
               (unsigned long long)client.data_datagrams_rx(),
               (unsigned long long)client.server_acks(),
               client.ms_since_rx());
        fflush(stdout);
        return;
      }
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
      if (feature_set.physics == rbx::features::PhysicsMode::Client) {
        const auto ps = local_player.physics_stats();
        const auto cs = local_player.character();
        printf("raknet: physics t=%llds bodies=%zu static=%zu kinematic=%zu "
               "dynamic=%zu steps=%llu owner=0x%llx char=(%.3f, %.3f, %.3f) "
               "vel=(%.2f, %.2f, %.2f) grounded=%d state=%d\n",
               static_cast<long long>(uptime),
               ps.static_bodies + ps.kinematic_bodies + ps.dynamic_bodies,
               ps.static_bodies, ps.kinematic_bodies, ps.dynamic_bodies,
               (unsigned long long)ps.steps,
               (unsigned long long)live->local_network_owner_id(), cs.position.x,
               cs.position.y, cs.position.z, cs.velocity.x, cs.velocity.y,
               cs.velocity.z, cs.grounded ? 1 : 0, cs.state);
      }
      fflush(stdout);
    };
    // Wire watchdog: the RCC can end a session by simply going quiet, and the
    // only way to tell "it reaped us" apart from "our sends stopped" is to
    // print both directions on one line, once a second. At the 2026-11-07 reap
    // (session age ~370 s) the question is whether tx kept climbing while
    // srv_ack froze, and this is the instrument that answers it.
    auto wireWatch = [&]() {
      const char* ws = getenv("RBX_WIRE_SECS");
      if (!ws) return;
      static auto next_wire =
          std::chrono::steady_clock::now() + std::chrono::seconds(1);
      const auto wnow = std::chrono::steady_clock::now();
      if (wnow < next_wire) return;
      next_wire = wnow + std::chrono::seconds(std::max(1, atoi(ws)));
      const auto uptime =
          std::chrono::duration_cast<std::chrono::seconds>(wnow - run_start)
              .count();
      printf("raknet: wire t=%llds tx=%llu rx=%llu srv_ack=%llu ack_tx=%llu "
             "clk=%llu fail=%llu\n",
             static_cast<long long>(uptime),
             (unsigned long long)client.datagrams_tx(),
             (unsigned long long)client.data_datagrams_rx(),
             (unsigned long long)client.server_acks(),
             (unsigned long long)client.acks_sent(),
             (unsigned long long)client.clock_ticks_rx(),
             (unsigned long long)client.send_failures());
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
        client.pump(session.pump_ms);
        if (injectRejoin() || session_dead)
          break;
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
        if (spawn_retry_wanted()) {
          ++spawn_retries;
          printf("raknet: no local avatar — fresh-session retry %d/%d\n",
                 spawn_retries, spawn_retry_limit);
          spawn_retry = true;
          session_up = false;
          break;
        }
        if (live) {
          // Advance the script scheduler by the time actually elapsed, not by
          // a fixed 30 ms that only matched the old 30 ms pump.
          const auto now = std::chrono::steady_clock::now();
          double dt = std::chrono::duration<double>(now - last_live_pump).count();
          last_live_pump = now;
          if (dt > 0.25) dt = 0.25;
          live->pump(dt);
          tickLocalPlayer(dt);
        }
        stats_tick();
        wireWatch();
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
        if (injectRejoin() || session_dead)
          return false;
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
        guiTestTick();
        if (spawn_retry_wanted()) {
          ++spawn_retries;
          printf("raknet: no local avatar — fresh-session retry %d/%d\n",
                 spawn_retries, spawn_retry_limit);
          spawn_retry = true;
          session_up = false;
          return false;
        }
        if (live) {
          live->pump(dt);
          tickLocalPlayer(dt);
        }
        wireWatch();
        return !g_stop && !out_of_time();
      };
      auto viewCamera = [&](rbxclient::ViewCamera* out) {
        rbxclient::CameraState cam;
        if (!out || !local_player.view_camera(&cam))
          return false;
        out->valid = true;
        out->px = cam.x;
        out->py = cam.y;
        out->pz = cam.z;
        // Camera LookVector is the CFrame's -Z axis.
        out->lx = -cam.r02;
        out->ly = -cam.r12;
        out->lz = -cam.r22;
        out->fov_y = cam.fov_y;
        return true;
      };
      std::string verr;
      auto guiRoot = [&]() -> const rbx::Instance* {
        return live ? live->root() : nullptr;
      };
      const int vrc = rbxclient::runViewerLive(boxes, tick, shot, &verr,
                                               viewCamera, guiRoot, cookie);
      if (vrc != 0) {
        fprintf(stderr, "raknet: view: %s\n", verr.c_str());
        return vrc;
      }
      // Closing the window is a user stop; a dead session or a stalled join
      // must fall through to the rejoin path instead of ending the run.
      if (!session_dead && !join_retry && !spawn_retry) g_stop = 1;
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
    } else {
      printf("raknet: join-only session: %llu datagrams rx, %llu tx, %llu "
             "app messages, no world\n",
             (unsigned long long)client.data_datagrams_rx(),
             (unsigned long long)client.datagrams_tx(),
             (unsigned long long)client.app_messages());
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
    if (!g_stop && spawn_retry) {
      // A character-less session cannot be repaired in place either: the
      // Player and its Character refs belong to that RCC session. Retry the
      // whole join instead of sending a late or synthetic spawn record.
      printf("raknet: rejoining with a fresh session after a character-less join\n");
      session_up = false;
      spawn_retry = false;
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
