#!/usr/bin/env python3
"""Replication capture survey (first artifact: run/repl_1818.bin).

Walks the saved chan1 byte stream (compactVarint framing, same as the
challenge finder), histograms top-level message IDs, and extracts the
0x97 JoinDataItemV2 snapshot (8-byte header + zstd frame).

Usage:
  python3 tools/repl_survey.py run/repl_1818.bin [--extract-join out.raw]
  python3 tools/repl_survey.py run/repl_1818.bin --extract-delta outdir/

Findings (2026-10-11, Crossroads 1818):
  0x93 x1 join reply | 0x9B x1 challenge | 0x97 x1 53KB snapshot
  0x81 x1 peer init | 0x84 x1 5B marker | 0x83 x3561 deltas
0x97 body = [97][2B magic a3 cf][2B 00 00][u32 LE uncompressed @5:9]
[zstd frame @9:]. Inner (352276B, ~76% printable) = schema dictionary
(class descriptors with property lists) + ~630 instance records.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from zstd_lib import decompress as zstd_decompress  # noqa: E402


def cv(p):
    n = 1 << (p[0] >> 6)
    if n > len(p):
        return None
    v = p[0] & 0x3F
    for b in p[1:n]:
        v = (v << 8) | b
    return v, n


def walk(d):
    off = 0
    while off < len(d):
        r = cv(d[off:])
        if r is None:
            raise ValueError(f"bad varint @{off}")
        ln, k = r
        if ln == 0 or off + k + ln > len(d):
            raise ValueError(f"bad frame @{off} len={ln}")
        yield off, d[off + k:off + k + ln]
        off += k + ln


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    d = open(sys.argv[1], "rb").read()
    hist = {}
    first = {}
    n = 0
    join = None
    for off, body in walk(d):
        hist[body[0]] = hist.get(body[0], 0) + 1
        first.setdefault(body[0], (off, len(body)))
        if body[0] == 0x97:
            join = body
        n += 1
    print(f"bytes={len(d)} msgs={n} framing_errors=0")
    for k in sorted(hist):
        off, ln = first[k]
        print(f"  0x{k:02x} x{hist[k]} first:@{off} len={ln}")
    if join is None:
        return 0
    magic, size = join[1:3], int.from_bytes(join[5:9], "little")
    inner = zstd_decompress(join[9:])
    print(f"0x97: magic={magic.hex()} uncompressed_field={size} "
          f"actual={len(inner)} match={size == len(inner)}")
    asc = sum(32 <= b < 127 for b in inner)
    print(f"inner: {len(inner)}B printable={asc / len(inner):.1%}")
    if "--extract-join" in sys.argv:
        out = sys.argv[sys.argv.index("--extract-join") + 1]
        open(out, "wb").write(inner)
        print(f"wrote {out}")
    if "--extract-delta" in sys.argv:
        # 0x83/0x16 batches: [16][flags][u32 comp][u32 decomp][zstd].
        # Verified: comp == frame bytes, decomp == inner bytes.
        outdir = sys.argv[sys.argv.index("--extract-delta") + 1]
        os.makedirs(outdir, exist_ok=True)
        n = 0
        for _, b in walk(d):
            if len(b) > 11 and b[0] == 0x83 and b[1] == 0x16 \
                    and b[11:15] == b"\x28\xb5\x2f\xfd":
                comp = int.from_bytes(b[3:7], "little")
                decomp = int.from_bytes(b[7:11], "little")
                # comp counts the frame only: one trailing byte follows it
                # ("Src size is incorrect" otherwise).
                fr = b[11:11 + comp]
                assert len(fr) == comp and fr[:4] == b"\x28\xb5\x2f\xfd"
                inner = zstd_decompress(fr)
                assert decomp == len(inner), (decomp, len(inner))
                open(os.path.join(outdir, f"delta16_{n:03d}.raw"),
                     "wb").write(inner)
                n += 1
        print(f"wrote {n} delta batches -> {outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
