#!/usr/bin/env python3
"""a1_gap_stats.py — what predicts the gap between two 0xA1 records?

parseAppStateFrame accepts either boundary (body end, or +4) and falls back to
a scan. That heuristic is why ~0.2% of frames desync. This measures, over the
frames that walk cleanly, what actually sits between two records: the gap
length and how it correlates with the ending record's body length, class, and
the "attribute section follows" mask bit.

    a1_gap_stats.py <schema.txt> <frames-dir> [max]

Prints the gap histogram and, for each gap, the counts of the predictors, so a
rule like "gap = 4 iff <something>" is visible instead of guessed.
"""
import os
import struct
import sys
from collections import Counter


def load_schema(path):
    names = {}
    with open(path) as f:
        for line in f:
            t = line.split()
            if len(t) >= 5 and t[0] == "class":
                names[int(t[1])] = t[2]
    return names


def u16(b, p):
    return struct.unpack_from("<H", b, p)[0]


def u32(b, p):
    return struct.unpack_from("<I", b, p)[0]


def record_head(b, p, ncls):
    if p + 13 > len(b):
        return None
    q = p
    lo = 0
    for i in range(4):
        if q >= len(b):
            return None
        lo |= (b[q] & 0x7F) << (7 * i)
        q += 1
        if not (b[q - 1] & 0x80):
            break
    if lo:
        q += 4
    if q + 4 > len(b):
        return None
    cls = u16(b, q)
    mc = u16(b, q + 2)
    if cls >= ncls or mc == 0 or mc > 4096:
        return None
    words = max(1, (mc + 63) // 64)
    mask_at = q + 4
    q = mask_at + 8 * words
    if q + 2 > len(b):
        return None
    blen = u16(b, q)
    body = q + 2
    if body + blen > len(b):
        return None
    mask = b[mask_at:mask_at + 8 * words]
    extra = mask[int(words) * 8 - 1] >> 7  # placeholder, replaced below
    return lo, cls, mc, words, body, blen, mask


def has_attr_bit(mask_bytes, words, mc):
    """True when a bit is set at index == number of named slots.

    The named slot count is not in the frame, but the observed invariant is
    that the flag is the highest set bit and sits above all member bits; the
    probe reports it as bit == named. Here we just report the highest set bit
    index so the caller can correlate it with the gap.
    """
    bits = int.from_bytes(mask_bytes, "little")
    if not bits:
        return 0
    return bits.bit_length() - 1


def walk(b, ncls, schema_mc):
    """Walk a frame with the exact-boundary rule; return [(gap, info)] or None."""
    n_rec = u32(b, 5)
    p = 17
    info = []
    for r in range(n_rec):
        h = record_head(b, p, ncls)
        if h is None:
            return None
        lo, cls, mc, words, body, blen, mask = h
        end = body + blen
        top = has_attr_bit(mask, words, mc)
        if r == n_rec - 1:
            if end != len(b):
                return None
            info.append((None, cls, mc, blen, top, end % 4))
            break
        gap = None
        for g in (4, 0):
            if record_head(b, end + g, ncls) is not None:
                gap = g
                break
        if gap is None:
            return None
        info.append((gap, cls, mc, blen, top, end % 4))
        p = end + gap
    return info


def main(argv):
    schema = load_schema(argv[1])
    d = argv[2]
    maxf = int(argv[3]) if len(argv) > 3 else 10 ** 9
    files = sorted(f for f in os.listdir(d) if f.endswith(".bin"))
    gaps = Counter()
    by_mod = Counter()
    by_attr = Counter()
    by_len_mod4 = Counter()
    walked = 0
    for f in files[:maxf]:
        b = open(os.path.join(d, f), "rb").read()
        if len(b) < 21 or b[0] != 0xA1:
            continue
        info = walk(b, len(schema), {})
        if not info:
            continue
        walked += 1
        for gap, cls, mc, blen, top, endm in info:
            if gap is None:
                continue
            gaps[gap] += 1
            by_mod[(gap, endm)] += 1
            by_attr[(gap, 1 if top >= mc else 0)] += 1
            by_len_mod4[(gap, blen % 4)] += 1
    print(f"frames walked exactly: {walked}/{min(len(files), maxf)}")
    print("gap histogram:", dict(gaps))
    print("gap x (body end % 4):", dict(by_mod))
    print("gap x (top mask bit >= member_count):", dict(by_attr))
    print("gap x (body_len % 4):", dict(by_len_mod4))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
