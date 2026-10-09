#!/usr/bin/env python3
"""roblox_random.py — exact port of Roblox's Random (PCG) from libroblox.so.

Decompiled sources:
  Random.new(seed) (sub_27B30AE):
      if |seed| <= 2^53:  state = M * (uint32)(int32)seed + C
      else:               state = C
      M = 0x5851F42D4C957F2D, C = 0x399D2694695129DE
  NextInteger(min,max) (sub_2307554):
      v3 = |min-max|; lo = min(min,max)
      next = M*state + 105
      out  = ROR32((state>>27) ^ (state>>45), state>>59)   # 32-bit output of current state
      small path (|min-max|>>1 <= 0x7FFFFFFE):
          state = next
          return lo + ((out * (v3+1)) >> 32)              # 64-bit multiply, >>32
      large path: uses two outputs (see decompile) — not needed by the challenge,
                  implemented for completeness.
  NextNumber(min,max) (sub_42BFF60):
      reads 64 bits from TWO consecutive outputs, maps to [0,1), scales.
"""
M64 = (1 << 64) - 1
M = 0x5851F42D4C957F2D
C = 0x399D2694695129DE
INC = 105
TWO53 = 9007199254740992.0


def _ror32(x, r):
    x &= 0xFFFFFFFF
    r &= 31
    return ((x >> r) | (x << (32 - r))) & 0xFFFFFFFF


def _out32(state):
    return _ror32((state >> 27) ^ (state >> 45), state >> 59)


def _c_cast_i32_from_double(v):
    """C (int) cast of a double on x86-64 (cvttsd2si semantics)."""
    import math
    if math.isnan(v) or v >= 2147483648.0 or v < -2147483648.0:
        return -2147483648  # 0x80000000
    return int(v)  # trunc toward zero


class Random:
    def __init__(self, seed=None):
        if seed is None:
            # Roblox seeds from a global entropy source; not reproducible.
            raise ValueError("seed required for deterministic use")
        if abs(seed) <= TWO53:
            s = _c_cast_i32_from_double(float(seed)) & 0xFFFFFFFF
            self.state = (M * s + C) & M64
        else:
            self.state = C

    def next_integer(self, lo, hi):
        lo_i = _c_cast_i32_from_double(float(lo))
        hi_i = _c_cast_i32_from_double(float(hi))
        v3 = lo_i - hi_i
        if lo_i <= hi_i:
            v3 = hi_i - lo_i
        if lo_i < hi_i:
            hi_i = lo_i  # hi_i becomes min
        if (v3 >> 1) > 0x7FFFFFFE:
            # large-range path (two outputs)
            v4 = (M * self.state + INC) & M64
            v5 = _out32(self.state)
            v7 = _out32(v4)
            self.state = (M * v4 + INC) & M64
            v8 = v3 + 1
            if v8 != 0:
                return ((((v7 * (v8 & 0xFFFFFFFF)) & M64) >> 32) + v7 * (v8 >> 32) + hi_i + ((v5 * (v8 >> 32)) >> 32)) & M64
            return (v5 | (v7 << 32)) & M64
        else:
            v4 = (M * self.state + INC) & M64
            v5 = _out32(self.state)
            self.state = v4
            return hi_i + ((v5 * (v3 + 1)) >> 32)

    def next_number(self, lo=0.0, hi=1.0):
        # 64-bit output from two consecutive outputs; state advances twice
        v8 = (M * self.state + INC) & M64
        a = _out32(self.state)
        b = _out32(v8)
        self.state = (M * v8 + INC) & M64
        # v9 = unpacklo_epi32([x, y]) - 2^32 (xmmword_204020), hadd, ldexp(-64)
        # = (x + y*2^32) / 2^64
        frac = ((a + (b << 32)) & M64) / 18446744073709551616.0
        return lo + frac * (hi - lo)


if __name__ == "__main__":
    # sanity: basic stream from a known seed (values unverified until runner test)
    r = Random(12345)
    print([r.next_integer(1, 2147483647) for _ in range(4)])
    r = Random(0)
    print([r.next_integer(1, 100) for _ in range(8)])
