#!/usr/bin/env python3
"""inspect_raknet_dump.py — decode a RBX_RAKNET_DUMP directory.

RBX_RAKNET_DUMP=<dir> makes the RakNet client write every *authenticated*
datagram (after the data AEAD, still RakNet-framed) as dNNNNNN.bin. This walks
them exactly like client/src/raknet/rx_stream.cpp: skip ACKs, parse reliability
headers, reassemble split messages, then print each complete application
message (id + length + head/tail hex).

usage:
  tools/inspect_raknet_dump.py <dir> [--all]      # RBX_RAKNET_DUMP output
  tools/inspect_raknet_dump.py <glob> --rupp      # captured UDP payloads

--rupp strips the RUPP header (total u16 BE at [2:4]) before walking, which is
what a socket-level capture (tools/gdb/rc_capture.gdb wire_tx_*.bin) needs.
"""
import glob
import struct
import sys
from collections import Counter


def u24(d, o):
    return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16)


def strip_rupp(d):
    """UDP payload -> RUPP body (drops the 23/31-byte client header)."""
    if len(d) < 4 or d[0] != 0x01 or d[1] != 0x00:
        return d
    total = (d[2] << 8) | d[3]
    if total < 4 or total > len(d):
        return d
    return d[total:]


def walk(paths, rupp=False):
    splits = {}
    msgs = []
    for f in sorted(paths):
        d = open(f, "rb").read()
        if rupp:
            d = strip_rupp(d)
        if not d or d[0] & 0xC0 == 0xC0:
            continue
        off = 4
        while off + 3 <= len(d):
            rel = d[off]
            bits = (d[off + 1] << 8) | d[off + 2]
            dl = (bits + 7) // 8
            kind = rel >> 5
            o = off + 3
            if kind >= 2:
                o += 3
            ordering = 0
            if kind in (1, 3, 4):
                ordering = u24(d, o)
                o += 4
            split = bool(rel & 0x10)
            cnt = sid = idx = 0
            if split:
                cnt = struct.unpack(">I", d[o : o + 4])[0]
                sid = struct.unpack(">H", d[o + 4 : o + 6])[0]
                idx = struct.unpack(">I", d[o + 6 : o + 10])[0]
                o += 10
            data = d[o : o + dl]
            o += dl
            off = o
            if not data:
                continue
            if split:
                b = splits.setdefault((sid, cnt), {"n": 0, "p": {}, "rel": rel})
                if idx not in b["p"]:
                    b["p"][idx] = data
                    b["n"] += 1
                if b["n"] == cnt:
                    body = b"".join(b["p"][i] for i in range(cnt))
                    del splits[(sid, cnt)]
                    msgs.append((body, "split", b["rel"], ordering, cnt))
            else:
                msgs.append((data, "whole", rel, ordering, 1))
    return msgs


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    show_all = "--all" in sys.argv
    rupp = "--rupp" in sys.argv
    arg = sys.argv[1]
    if any(c in arg for c in "*?["):
        paths = sorted(glob.glob(arg))
    else:
        paths = sorted(glob.glob(arg.rstrip("/") + "/d*.bin"))
    msgs = walk(paths, rupp)
    print(f"{len(paths)} datagrams -> {len(msgs)} messages")
    print("ids:", {f"0x{k:02x}": v for k, v in sorted(Counter(m[0][0] for m in msgs).items())})
    seen = set()
    for body, kind, rel, ordering, cnt in msgs:
        i = body[0]
        if not show_all and i in seen:
            continue
        seen.add(i)
        print(
            f"id=0x{i:02x} len={len(body)} {kind} rel=0x{rel:02x} "
            f"ord={ordering} frags={cnt}"
        )
        print("   head:", body[:64].hex())
        if len(body) > 64:
            print("   tail:", body[-32:].hex())
    return 0


if __name__ == "__main__":
    sys.exit(main())
