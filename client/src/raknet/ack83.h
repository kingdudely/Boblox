// ack83.h — the 0x83 "client state" frames (0x05 physics/position report).
//
// The 0x83 container is a `0x83` byte followed by whole records, each starting
// with its id. 0x05 is the client's periodic position/liveness report; the RCC
// reaps a session that stops sending it (0x15 266 at exactly 300 s without it).
//
// Captured shape, 45 B unreliable (same-job native capture, place 1818):
//
//   83 05 16 | <u24 clockA> | <u32 0> | 00
//            | <u24 clockA> | <u32 0> | 00
//            | <f32 x> <f32 z> <f32 0.0>
//            | <u32 0>
//            | <flag u8>
//            | <u24 clockB> | 00 | <f32 v>
//            | 00                                   <- frame terminator
//
// `clockA` is the client's process clock in milliseconds (u24): in the capture
// it runs 1,000 units/s and sits 4.0 s ahead of the session epoch, and the two
// copies in a frame are identical. `clockB` is one field from the same clock
// (1-3 ms earlier). The three floats are the character's horizontal position;
// the capture walks (31.3, 14.7) -> (59.0, 50.0), which matches the spawn area,
// and the third float is always 0.
//
// The reliable variant is the same body plus a 0x23 clock record, 50 B.
//
// Replaying the captured frame verbatim works (the RCC accepts it) but carries
// a clock and a position from a different session. This builder writes the live
// ones.
#pragma once

#include <cstdint>
#include <vector>

namespace rbxclient {
namespace ack83 {

// `v` is the trailing float. It is the one field whose meaning is not
// recovered: in the capture it climbs 686.5 -> 803.5 over the first ~15 s and
// then holds, so it is carried as an observed constant (kDefaultV) rather than
// invented.
extern const float kDefaultV;

// 45 B unreliable form. `x`/`z` are the character's horizontal position.
std::vector<uint8_t> ack05(uint32_t clock_ms, float x, float z, float v,
                           bool reliable);

// 49 B 0x83/0x06 timing record — the native's most frequent post-join frame
// (3.8/s over 700 s) and the one this client never sent at all.
//
// Captured shape (same-job native capture, place 1818; all 3,091 records of
// the run share it):
//
//   83 06 01 | <u64 uptime_ms> | <u32 uptime_ms & 0xffffff>
//            | <u32 t0> <u32 t1> <u32 t2>       <- session-clock triple
//            | <u32 n> <u32 m> <i32 lag>
//            | <u32 0> | <u32 e> | <u8 tail> | 00
//
// Field semantics that the capture settles:
//   * uptime_ms advances at exactly 1000 units/s and runs ~5,754 s ahead of
//     the session clock, i.e. it is the client *process* clock, not elapsed
//     time since joining.
//   * t0/t1/t2 also advance at 1000/s but in the session epoch, and t2 is
//     byte-identical to the clock of an adjacent 0x05 record in the same
//     frame, so the triple is (sample, receive, apply) times for the physics
//     state 0x05 reports.
//   * lag is that same clock negated and sign-extended through 24 bits.
//   * n and m stay small (0..0x10) and are not recovered; they are passed in
//     so a caller can keep them at what the capture shows rather than have a
//     constant invented here.
std::vector<uint8_t> ack06(uint32_t uptime_ms, uint32_t t0, uint32_t t1,
                           uint32_t t2, uint32_t n, uint32_t m, int32_t lag);

} // namespace ack83
} // namespace rbxclient
