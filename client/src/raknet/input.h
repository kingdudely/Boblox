// input.h — the 0xA6 client input stream (ControlState + route frames).
//
// The native client's gameplay upstream is app message 0xA6, sent unreliable
// at ~60 Hz. Its frames batch up to four "sub-frames"; the first carries the
// step pair and a per-stage route field, the rest are `04 04 00` empty frame
// slots. Each sub-frame ends with an nActions byte; a non-zero action list is a
// ControlState block.
//
// Wire shapes, all captured from the native's decrypted 1818 TX stream
// (FINDINGS.md "Send path", /tmp/nat_live):
//
//   count=1  a6 01 01 00 01 <stepA u32 LE> <stepB u32 LE> 00 00
//   count=2  ... 00 00 04 04 00
//   count=3  ... 00 00 04 04 00 04 04 00
//   count=4  a6 04 01 00 <camera 11B> <stepA> <stepB> 00 00 04 04 00 ...
//
// Once the world is up the native switches the count=4 frame to a 37-byte
// steady form (48,013 of 48,041 frames in the 700 s same-job run):
//
//   a6 04 01 00 10 <13-byte route> <stepA u32 LE> <stepB u32 LE> 00 00
//      04 04 00 04 04 00 04 04 00
//
// The 14-byte route field (tag 0x10 + 13 bytes) is identical in every session
// captured on 1818, and the two u32s advance by exactly 4 per frame (240 Hz).
// The 34-byte form is only correct before the world loads.
//
// ControlState block (the shape the QUIC probe ladder proved framing-valid,
// send.cpp / FINDINGS "cb0 framing-valid"):
//   [N=01][A u32 LE][B u32 LE][innerLen u16 LE][presence u8][value]*
// A/B are frame counters the native writes from its own input clock; zero
// passes the framing check. `withControl` replaces the FINAL nActions byte,
// which is the sub-frame the server reads actions from.
//
// Movement semantics are NOT recovered: the native's forward mapping lives in
// its local player controller, and the bool-slot sweep (slots 0-7, both value
// polarities) produced no spawn on live sessions. This module therefore gives
// the caller the exact framing plus explicit slot control; it does not invent
// a "forward" key. Pass an explicit slot mask.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rbxclient {
namespace input {

// The native's camera blob (11B) from the count=4 frame. It embeds an instance
// id from the capture it came from, so it is only correct for a world that
// contains that id; RBX_A6_CAMERA=0 uses the short form instead.
extern const uint8_t kCameraBlob[11];

// The steady-state route field (tag 0x10 + 13 bytes), captured from the
// native's post-load 0xA6 stream on 1818. Constant for a whole session and
// identical across sessions.
extern const uint8_t kSteadyRoute[14];

// The native's 37-byte steady-state frame: count=4, the steady route field,
// the two frame clocks, and three empty sub-frames. `server` is the second
// clock slot; pass `step` for both when there is no server estimate yet.
std::vector<uint8_t> steadyFrame(uint32_t step, uint32_t server);

// Native route frame shape, index 0..3 (count = index+1). step is written to
// both step slots, which is what keeps the native's stepId continuity.
std::vector<uint8_t> routeFrame(int idx, uint32_t step);

// Rebuild a route frame from scratch. `control` empty = nActions 00; otherwise
// it replaces the final nActions byte.
std::vector<uint8_t> buildFrame(uint32_t step, int count,
                                const std::vector<uint8_t>& control,
                                bool camera);

// One ControlState block. `slots` is an 8-bit presence mask; each set slot
// contributes one bool value byte, ascending. Returns empty for slots==0.
std::vector<uint8_t> controlBlock(uint8_t slots, uint8_t values = 0xFF,
                                  uint32_t a = 0, uint32_t b = 0);

// Replace the final nActions byte of `frame` with `block`.
std::vector<uint8_t> withControl(const std::vector<uint8_t>& frame,
                                 const std::vector<uint8_t>& block);

// Frame index/step pair based on a monotonic 75 ms tick, matching the
// native's 13.3 Hz: indices 0,1,2 once, then index 3 forever.
struct StreamState {
  uint32_t step = 0x14c;
  int frame_index = 0;
};

std::vector<uint8_t> nextFrame(StreamState* s);

} // namespace input
} // namespace rbxclient
