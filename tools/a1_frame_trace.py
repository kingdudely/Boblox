#!/usr/bin/env python3
"""a1_frame_trace.py — walk a dumped 0xA1 frame record-by-record and show where
the boundary rule stops matching, so the framing can be read off real bytes.

    a1_frame_trace.py <schema.txt> <frame.bin>

Mirrors parseAppStateFrame (replicate.h: header at 0, tick/records/ack/flags,
records from offset 17, [ref][u16 class][u16 member_count][mask words][u16
body len][body]) and prints, for each record, the offset it starts at, the class,
the body length, and the bytes at the computed end so a 4-byte separator, a
shorter one, or a mis-sized body is visible.
"""
import struct
import sys


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


def read_record_head(b, p, ncls):
    """Return (ok, ref_lo, cls, mc, body_start, body_len) for a record at p."""
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
    q += 4 + 8 * words
    if q + 2 > len(b):
        return None
    blen = u16(b, q)
    body_start = q + 2
    if body_start + blen > len(b):
        return None
    return (lo, cls, mc, body_start, blen)


def solve(b, ncls):
    """DP over (record index, offset): every record must parse and the last one
    must end exactly at the frame end. Returns (gaps, end) or None."""
    n_rec = u32(b, 5)
    start = 17
    # dp[i] maps offset -> (gap list, record info)
    states = {start: []}
    for i in range(n_rec):
        nxt = {}
        for p, gaps in states.items():
            h = read_record_head(b, p, ncls)
            if h is None:
                continue
            lo, cls, mc, bs, blen = h
            end = bs + blen
            if i == n_rec - 1:
                if end == len(b):
                    return gaps + [0], end
                continue
            for gap in range(0, 9):
                q = end + gap
                if q not in nxt and read_record_head(b, q, ncls) is not None:
                    nxt[q] = gaps + [gap]
        # keep the map small but deterministic: first offset wins per target
        states = nxt
        if not states:
            return None
    return None


def main(argv):
    schema = load_schema(argv[1])
    body = open(argv[2], "rb").read()
    print(f"frame len={len(body)} id=0x{body[0]:02x} tick={u32(body,1)} "
          f"records={u32(body,5)} ack={u32(body,9)} flags={u32(body,13)}")
    sol = solve(body, len(schema))
    if sol:
        gaps, end = sol
        print(f"exact solution: gaps between records {gaps[1:]} end={end}")
    else:
        print("no exact solution with gap in 0..8 per boundary")
    n_rec = u32(body, 5)
    p = 17
    for r in range(n_rec):
        if p + 19 > len(body):
            print(f"record {r}: offset {p} past end ({len(body)})")
            return 1
        # ref lo varint (up to 4 bytes)
        q = p
        lo = 0
        for i in range(4):
            lo |= (body[q] & 0x7F) << (7 * i)
            q += 1
            if not (body[q - 1] & 0x80):
                break
        if lo:
            q += 4
        cls = u16(body, q)
        q += 2
        mc = u16(body, q)
        q += 2
        words = max(1, (mc + 63) // 64)
        q += 8 * words
        blen = u16(body, q)
        q += 2
        end = q + blen
        marker = body[end:end + 4].hex(" ")
        print(f"record {r:2d} at {p:5d} ref={lo:<6} class={cls:<4} "
              f"{schema.get(cls,'?'):<22} mc={mc:<3} words={words} body={blen:<5} "
              f"end={end:<6} marker={marker}")
        if end > len(body):
            print("   body runs past the frame")
            return 1
        # Try the documented rule, then the unprefixed boundary.
        nxt = None
        if body[end:end + 4] and u16(body, end + 1) if end + 3 <= len(body) else False:
            pass
        for cand, label in ((end + 4, "+4"), (end, "+0")):
            if cand + 19 <= len(body):
                c_cls = u16(body, cand + 5)
                c_mc = u16(body, cand + 7)
                if c_cls < len(schema) and c_mc:
                    nxt = (cand, label)
                    break
        if nxt is None:
            print(f"   no boundary at {end} or {end+4}; bytes from end:")
            print("   " + body[end:end + 40].hex(" "))
            print("   " + repr(body[end:end + 40]))
            return 1
        p = nxt[0]
    print(f"walked {n_rec} records, ended at {p} of {len(body)}"
          f" (tail {len(body) - p} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
