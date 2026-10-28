// entry.h — transport entry points behind the unified ./roblox binary.
//
// Each transport keeps its own argument parser and run loop; only the
// process entry point is shared. Whichever of RBX_RAKNET / RBX_TRANSPORT is
// configured off is not linked at all, so the other's code stays untouched
// (see client/CMakeLists.txt).
#pragma once

namespace rbxclient {

// RakNet client (RUPP + offline handshake + join flow). Returns 0 when the
// encrypted handshake verified, 1 on transport failure, 2 on usage errors.
int raknetMain(int argc, char** argv);

// RbxTransport (QUIC) client. probe_ms > 0 bounds a run: when the deadline
// passes before a challenge is solved, it returns 3 ("not proven") instead
// of running on. 0 runs until the session ends or the user interrupts.
// `interrupted` (optional) is set when SIGINT/SIGTERM ended the run, so the
// automatic selector does not mistake a user stop for a transport failure.
int transportMain(int argc, char** argv, int probe_ms = 0,
                  bool* interrupted = nullptr);

} // namespace rbxclient
