// send.h — client->server upstream builders (movement/input path).
//
// Modular home for everything we SEND on gameplay channels (the recv side
// lives in repl_*). Every builder is pure (bytes out, no I/O) and every
// grammar cites its native function. Safety ladder (FINDINGS "Send path",
// anti-cheat context — alt account first, never blind writes):
//   1. heartbeat() — A6 count=0 (framing-valid, semantically null).
//   2. replay of native post-answer bytes (msgs::routeSample).
//   3. fresh-stepId frames, then ControlState bit-brute-force (later).
// Each step ships only after the previous proves session survival +
// (for driven steps) server echo. transport send goes through
// Session::sendInput (chan 11) in session.{h,cpp}.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rbx {
namespace send {

// Upstream 0xA6 input heartbeat: [A6][count=0] (2 bytes). The idle-shape
// hypothesis (a quiet native client sends nothing game-conn upstream —
// TX-captured, tools/tx_capture.gdb): if the server tolerates count=0,
// chan-11 framing is proven without claiming any game state.
std::vector<uint8_t> heartbeat();

} // namespace send
} // namespace rbx
