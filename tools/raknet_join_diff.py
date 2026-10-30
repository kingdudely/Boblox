#!/usr/bin/env python3
"""raknet_join_diff.py — reassemble the join messages from a plaintext datagram
dump so the native client and ours can be compared body-for-body.

Input is a directory of plaintext RakNet datagrams (`rNNNNNN.bin`), exactly what
`tools/raknet_native_rx.py` (native, --direction tx) and our client's
`RBX_RAKNET_DUMP` (own) produce.

A reliable-ordered message larger than one datagram arrives as `split_count`
fragments that share (kind, ordering, split id) and differ by split index, so
grouping on that tuple and concatenating by index rebuilds the app body.

  raknet_join_diff.py --dir DIR [--only 90,92,8a,8f,9b] [--hex N]
                      [--write OUTDIR]
"""
import argparse
import os
import sys

NAMES = {0x90: "90 client-info", 0x92: "92 nonce", 0x8A: "8a ticket",
         0x8F: "8f", 0x9B: "9b", 0x13: "13", 0x83: "83", 0x97: "97 join-data"}


def u24(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


def parse(d):
    """-> (flags, dgram_id, msgs, acks); msg = dict(id, kind, ord, chan, split, idx, data)"""
    if len(d) < 4:
        return None
    flags = d[0]
    # 0xc0 ACK / 0xa0 NAK: no internal packets, just a record range.
    if (flags & 0xE0) in (0xA0, 0xC0):
        return flags, u24(d, 1), [], True
    msgs = []
    off, n = 4, len(d)
    while off + 3 <= n:
        rel = d[off]
        bits = (d[off + 1] << 8) | d[off + 2]
        off += 3
        kind = rel >> 5
        if kind > 4 or (rel == 0 and bits == 0):
            break
        dlen = (bits + 7) // 8
        if kind >= 2:
            off += 3
        ordering = chan = None
        if kind in (1, 3, 4):
            if off + 4 > n:
                break
            ordering = u24(d, off)
            chan = d[off + 3]
            off += 4
        split = idx = None
        if rel & 0x10:
            split = int.from_bytes(d[off + 4:off + 6], "big")
            idx = int.from_bytes(d[off + 6:off + 10], "big")
            off += 10
        if off + dlen > n:
            break
        data = d[off:off + dlen]
        off += dlen
        msgs.append(dict(id=data[0] if data else None, kind=kind, ord=ordering,
                         chan=chan, split=split, idx=idx, data=data))
    return flags, u24(d, 1), msgs, False


def assemble(path):
    """-> list of (ordering, msg_id, body) for one dump dir, in arrival order."""
    groups = {}
    singles = []
    for name in sorted(os.listdir(path)):
        if not name.endswith(".bin"):
            continue
        got = parse(open(os.path.join(path, name), "rb").read())
        if not got:
            continue
        _, _, msgs, _ = got
        for m in msgs:
            if not m["data"]:
                continue
            if m["split"] is None:
                singles.append((m["ord"], m["id"], m["data"]))
                continue
            key = (m["kind"], m["ord"], m["split"])
            groups.setdefault(key, {})[m["idx"]] = m["data"]
    out = list(singles)
    for (kind, ordering, sid), parts in groups.items():
        body = b"".join(parts[i] for i in sorted(parts))
        out.append((ordering, body[0], body))
    # Reliable-ordered messages are interleaved with unreliable ones, so the
    # only stable ordering is the ordering index; unreliable stay in place.
    out.sort(key=lambda t: (t[0] is None, t[0] if t[0] is not None else 0))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--label", default="")
    ap.add_argument("--only", default="")
    ap.add_argument("--hex", type=int, default=48)
    ap.add_argument("--write")
    a = ap.parse_args()
    only = {int(x, 16) for x in a.only.split(",") if x} if a.only else None
    if a.write:
        os.makedirs(a.write, exist_ok=True)
    for ordering, mid, body in assemble(a.dir):
        if mid is None:
            continue
        if only and mid not in only:
            continue
        name = NAMES.get(mid, f"{mid:02x}")
        print(f"{a.label}ord={ordering} id=0x{mid:02x} {name:12s} {len(body):5d}B  "
              f"{body[:a.hex].hex()}")
        if a.write:
            stem = f"{mid:02x}_ord{ordering if ordering is not None else 'x'}"
            open(os.path.join(a.write, stem + ".bin"), "wb").write(body)
    return 0


if __name__ == "__main__":
    sys.exit(main())
