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

// A route-sample frame (msgs::routeSample(idx)) with fresh stepIds
// spliced at byte offset off (two u32 LE). The native step pair
// increments per message (R0..R3 bases +0x400/route, second = first-1
// under the H1 split); stale pairs are shape-tolerated but state-ignored
// (replay3 survived 100s, zero echo), so driven input needs continuity.
// H1 (steps at [4:12]) vs H2 (steps at [5:13]) is still open — the two
// freshH probes below test one split each. Returns empty on bad idx.
std::vector<uint8_t> routeWithSteps(int idx, size_t off, uint32_t a,
                                    uint32_t b);

// Minimal ControlState block (sub_3478E96, field order reversed from IDA): [N=01][A u32][B u32]
// [innerLen u16][presence][value] with A/B zero (framing-OK hypothesis;
// echo likely needs live) and innerLen computed. Slot bit k set, value
// all zeros of the tag's size: tag 0 (bool, 1B) by default; pass an
// explicit (tag, valueBytes) for other slot types. Block for k=0 bool:
// 01|00000000|00000000|0200|01|00 (13B). Spliced over a message's final
// N=00 byte by withControl() below.
std::vector<uint8_t> controlBlock(int slot, uint8_t tag = 0,
                                  std::vector<uint8_t> value = {});

// routeSample(idx) with its FINAL byte (the idle N=00) replaced by a
// ControlState block. Net +12B for bool slots. Returns empty on bad idx
// or empty block.
std::vector<uint8_t> withControl(int idx,
                                 const std::vector<uint8_t>& block);

// Native post-answer chan-11 bytes, verbatim (sessioncap s0019-s0022):
// idx 0..3, sizes 15/18/21/24B. Canonical store (session.cpp's sender and
// msgs::routeSample both read here — no second copy).
std::vector<uint8_t> routeSample(int idx);

} // namespace send
} // namespace rbx
