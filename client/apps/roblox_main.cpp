// roblox_main.cpp — the one client binary: ./roblox
//
//   ./roblox --connection-type=RakNet       (RUPP + RakNet offline handshake)
//   ./roblox --connection-type=RbxTransport (QUIC via ngtcp2)
//   ./roblox                                (auto: probe, then fall back)
//
// The transports are otherwise independent programs (their own parsers and
// run loops: raknet_main.cpp, play_main.cpp). This file only picks one and
// forwards the remaining argv untouched. Which transports exist is fixed at
// configure time (RBX_RAKNET / RBX_TRANSPORT, both ON by default), so a
// request for a transport the build does not contain fails loudly instead
// of silently doing something else.
//
// Auto selection: RbxTransport is tried first because it is the smaller,
// more native-parity path; a bounded probe (no challenge solved in time, or
// the session failed) hands the same command line to RakNet, which is the
// path that currently completes a join. A user interrupt during the probe
// stops everything — it is not a failure to fall back from.
#include "entry.h"

#include "common/util.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef RBX_HAVE_RAKNET
#define RBX_HAVE_RAKNET 0
#endif
#ifndef RBX_HAVE_TRANSPORT
#define RBX_HAVE_TRANSPORT 0
#endif

namespace {

#if RBX_HAVE_RAKNET && RBX_HAVE_TRANSPORT
// How long auto lets RbxTransport try to prove itself before RakNet takes
// over. The QUIC handshake budget is 10.5 s and a healthy session solves the
// challenge within a few seconds of it, so this leaves room for one join.
constexpr int kAutoProbeMs = 20000;
#endif

void usage() {
  fprintf(stderr,
          "usage: roblox [--connection-type=NAME] [transport options]\n"
          "\n"
          "  --connection-type=raknet        RakNet (RUPP) client\n"
          "  --connection-type=rbxtransport  RbxTransport (QUIC) client\n"
          "  --connection-type=auto          probe RbxTransport, fall back to\n"
          "                                  RakNet (default when omitted)\n"
          "\n"
          "Common:\n"
          "  --place-id N            place to join\n"
          "  --roblosecurity COOKIE  .ROBLOSECURITY value (or $RBX_ROBLOSECURITY)\n"
          "  --join-json FILE        reuse a saved join reply (no HTTPS join)\n"
          "  --job-id UUID           pin one running instance of the place\n"
          "  --game-id N             universe id (joins that need it)\n"
          "\n"
          "RakNet only:\n"
          "  --seconds N             handshake/run window (default 10)\n"
          "  --90-template FILE      0x90 join body template\n"
          "  --8a-template FILE      0x8a ticket template\n"
          "  --pre-join FILE         pre-join pad datagram\n"
          "  --token-sub N           RUPP token subtype (1 = join token, default)\n"
          "  --skip-join             handshake only, no join body/ticket\n"
          "\n"
          "RbxTransport only:\n"
          "  --headful               raylib window (default)\n"
          "  --headless              no GL\n"
          "  --shot PATH             save one screenshot once the world is visible\n"
          "\n"
          "Maintainer knobs are RBX_* environment variables (see README).\n");
}

#if RBX_HAVE_RAKNET && RBX_HAVE_TRANSPORT
// Options only the RakNet parser accepts. An automatic run carrying one of
// them cannot be a RbxTransport run, so probing it would just print a usage
// error before the same fallback.
bool raknetOnlyCommandLine(const std::vector<char*>& argv) {
  static const char* kFlags[] = {
      "--seconds",     "--90-template", "--8a-template", "--pre-join",
      "--token-sub",   "--req2",        "--no-crypto",   "--skip-join",
      "--token",       "--cap-word",    "--session-id",  "--host",
      "--port",        "--universe-id", "--save-join",
  };
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string a = argv[i];
    for (const char* f : kFlags)
      if (a == f) return true;
    // --flag=value spelling
    for (const char* f : kFlags) {
      const std::string eq = std::string(f) + "=";
      if (a.rfind(eq, 0) == 0) return true;
    }
  }
  return false;
}
#endif

std::string builtinList() {
  std::string s;
#if RBX_HAVE_RAKNET
  s += "raknet";
#endif
#if RBX_HAVE_TRANSPORT
  if (!s.empty()) s += ", ";
  s += "rbxtransport";
#endif
  return s;
}

int unavailable(const char* which, const char* opt) {
  fprintf(stderr,
          "roblox: %s is not in this build (%s=OFF); built-in: %s\n",
          which, opt, builtinList().c_str());
  return 2;
}

} // namespace

int main(int argc, char** argv) {
  std::string type; // empty = auto
  std::vector<char*> fwd;
  fwd.push_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      usage();
      return 0;
    }
    if (a.rfind("--connection-type=", 0) == 0) {
      type = a.substr(std::string("--connection-type=").size());
      continue;
    }
    if (a == "--connection-type") {
      if (i + 1 >= argc) {
        fprintf(stderr, "roblox: --connection-type needs a value\n");
        return 2;
      }
      type = argv[++i];
      continue;
    }
    fwd.push_back(argv[i]);
  }
  const int fwd_argc = int(fwd.size());

  const std::string want = rbx::toLower(type);
  if (!want.empty() && want != "auto" && want != "raknet" &&
      want != "rbxtransport" && want != "rbx-transport") {
    fprintf(stderr, "roblox: unknown connection type '%s' (built-in: %s)\n",
            type.c_str(), builtinList().c_str());
    usage();
    return 2;
  }

  if (want == "raknet") {
#if RBX_HAVE_RAKNET
    return rbxclient::raknetMain(fwd_argc, fwd.data());
#else
    return unavailable("RakNet", "RBX_RAKNET");
#endif
  }
  if (want == "rbxtransport" || want == "rbx-transport") {
#if RBX_HAVE_TRANSPORT
    return rbxclient::transportMain(fwd_argc, fwd.data());
#else
    return unavailable("RbxTransport", "RBX_TRANSPORT");
#endif
  }

  // ---- auto -------------------------------------------------------------
#if RBX_HAVE_RAKNET && RBX_HAVE_TRANSPORT
  if (raknetOnlyCommandLine(fwd)) {
    printf("transport: auto — RakNet-only options present, skipping the "
           "RbxTransport probe\n");
    fflush(stdout);
    return rbxclient::raknetMain(fwd_argc, fwd.data());
  }
  bool interrupted = false;
  printf("transport: auto — probing RbxTransport (up to %ds), RakNet "
         "fallback\n",
         kAutoProbeMs / 1000);
  fflush(stdout);
  const int rc = rbxclient::transportMain(fwd_argc, fwd.data(), kAutoProbeMs,
                                          &interrupted);
  if (rc == 0 || interrupted) return rc;
  printf("transport: RbxTransport did not take this run (rc=%d); "
         "falling back to RakNet\n", rc);
  fflush(stdout);
  return rbxclient::raknetMain(fwd_argc, fwd.data());
#elif RBX_HAVE_RAKNET
  return rbxclient::raknetMain(fwd_argc, fwd.data());
#elif RBX_HAVE_TRANSPORT
  return rbxclient::transportMain(fwd_argc, fwd.data());
#else
  fprintf(stderr, "roblox: no transport built in (%s)\n", builtinList().c_str());
  return 2;
#endif
}
