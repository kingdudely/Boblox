#!/usr/bin/env python3
"""raknet_timeline.py — ordered per-datagram decode of a RakNet session.

Two inputs are supported so the native capture and our own client can be
compared with the *same* parser:

  --native DIR     a /tmp/rkc* capture dir with decoded.txt (wire_tx_N.bin + `pt:` hex)
  --dump DIR       a directory of raw authenticated datagrams (rNNNNNN.bin)

Message layout (matches client/src/raknet/rx_stream.cpp):
  datagram: [flags][dgram id u24 LE][message]*
  flags   : bit7 valid, bit6 ACK/NAK
  message : [rel][u16 BE bit length][if kind>=2: msg number u24 LE]
            [if kind in 1,3,4: ordering u24 LE][channel u8]
            [if rel&0x10: split count u32 BE][split id u16 BE][index u32 BE]
            [data]
  kind = rel >> 5   (0 unreliable, 1 unreliable sequenced, 2 reliable,
                     3 reliable ordered, 4 reliable sequenced)
"""
import argparse
import os
import re
import sys

NAMES = {
    0x00: "PAD?", 0x03: "PING3", 0x09: "CONN_REQ", 0x10: "ACCEPTED",
    0x13: "NEW_INCOMING", 0x15: "CONN_ATTEMPT_FAIL", 0x1C: "DISCONNECT",
    0x19: "OPEN_CONN_REQ1", 0x1A: "OPEN_CONN_REPLY1", 0x1B: "OPEN_CONN_REQ2",
    0x83: "REPL", 0x8A: "TICKET", 0x8D: "8D", 0x8F: "8F",
    0x90: "JOIN90", 0x92: "92", 0x93: "FLAGS93", 0x97: "JOINDATA97",
    0x9B: "CHALLENGE9B", 0xA1: "A1", 0xA6: "INPUT_A6", 0xC0: "ACK",
}



def u24(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


def parse_datagram(d):
    """-> (flags, dgram_id, [(id, kind, ord, chan, split, payload_len)], acks)"""
    if len(d) < 4:
        return None
    flags = d[0]
    if (flags & 0xc0) == 0xc0:
        # ACK/NAK body: u16 BE range count, then [single u8, min u24, max u24]
        acks = []
        n = int.from_bytes(d[4:6], "big") if len(d) >= 6 else 0
        off = 6
        for _ in range(n):
            if off + 7 > len(d):
                break
            single = d[off]
            lo = u24(d, off + 1)
            hi = u24(d, off + 4)
            acks.append((single, lo, hi))
            off += 7
        return flags, u24(d, 1), [], acks
    msgs = []
    off = 4
    n = len(d)
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
        ordering = None
        chan = None
        if kind in (1, 3, 4):
            ordering = u24(d, off)
            chan = d[off + 3]
            off += 4
        split = None
        if rel & 0x10:
            count = int.from_bytes(d[off:off + 4], "big")
            sid = int.from_bytes(d[off + 4:off + 6], "big")
            idx = int.from_bytes(d[off + 6:off + 10], "big")
            split = (count, sid, idx)
            off += 10
        if off + dlen > n:
            break
        payload = d[off:off + dlen]
        off += dlen
        if not payload:
            continue
        msgs.append((payload[0], kind, ordering, chan, split, len(payload)))
    return flags, u24(d, 1), msgs, []


def load_native(path):
    txt = open(os.path.join(path, "decoded.txt"), "r", errors="replace").read()
    out = []
    for m in re.finditer(r"== (wire_tx_\d+\.bin)[^\n]*\n\s+pt: ([0-9a-f]+)", txt):
        out.append((m.group(1), bytes.fromhex(m.group(2))))
    return out


def load_dump(path):
    out = []
    for name in sorted(os.listdir(path)):
        if not name.endswith(".bin"):
            continue
        out.append((name, open(os.path.join(path, name), "rb").read()))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--native")
    ap.add_argument("--dump")
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()
    if a.native:
        items = load_native(a.native)
    elif a.dump:
        items = load_dump(a.dump)
    else:
        ap.error("need --native DIR or --dump DIR")
    if a.limit:
        items = items[:a.limit]
    for name, d in items:
        r = parse_datagram(d)
        if not r:
            print(f"{name:20s} <short {len(d)}B>")
            continue
        flags, dgid, msgs, acks = r
        if acks:
            print(f"{name:20s} flags={flags:02x} dgram={dgid} ACK {acks}")
            continue
        desc = []
        for mid, kind, ordering, chan, split, ln in msgs:
            label = NAMES.get(mid, f"{mid:02x}")
            sp = f" split={split[2]}/{split[0]}" if split else ""
            od = f" ord={ordering} ch={chan}" if ordering is not None else ""
            desc.append(f"{label}({ln}B kind={kind}{od}{sp})")
        print(f"{name:20s} flags={flags:02x} dgram={dgid} " + " + ".join(desc))


if __name__ == "__main__":
    sys.exit(main())
