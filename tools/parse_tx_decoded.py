#!/usr/bin/env python3
"""parse_tx_decoded.py — message ids from a gdb capture of the native's TX.

The harness (tools/gdb/rc_capture.gdb) writes one record per sent datagram;
two record shapes exist:

  decoded.txt       `== wire_tx_N.bin rupp R pl P` + `pt: <plaintext hex>`
                    (complete plaintext, or truncated for big datagrams)
  full_decode.txt   `== wire_tx_N.bin ptlen=L` + `head=<first ~48 bytes>`

Both are the *plaintext* RakNet datagram. This walks them like
client/src/raknet/rx_stream.cpp (reliability header, split header, data) and
prints the message id/length of every message, so the native's post-handshake
send sequence can be compared with ours.

usage: tools/parse_tx_decoded.py <decoded.txt|full_decode.txt>
"""
import re
import sys
from collections import Counter


def u24(d, o):
    return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16)


def parse_datagram(raw, complete, name, out, ids):
    if not raw:
        return
    if raw[0] & 0xC0 == 0xC0:
        out.append(f"{name}: ACK")
        return
    if not raw[0] & 0x80:
        out.append(f"{name}: handshake {raw[:8].hex()}")
        return
    dgram = u24(raw, 1)
    off = 4
    while off + 3 <= len(raw):
        rel = raw[off]
        bits = (raw[off + 1] << 8) | raw[off + 2]
        dl = (bits + 7) // 8
        kind = rel >> 5
        o = off + 3
        if kind >= 2:
            o += 3
        ordering = None
        if kind in (1, 3, 4):
            ordering = u24(raw, o)
            o += 4
        split = bool(rel & 0x10)
        cnt = sid = idx = 0
        if split:
            cnt = int.from_bytes(raw[o : o + 4], "big")
            sid = int.from_bytes(raw[o + 4 : o + 6], "big")
            idx = int.from_bytes(raw[o + 6 : o + 10], "big")
            o += 10
        data = raw[o : o + dl]
        if not data:
            break
        ids[data[0]] += 1
        tag = f"ord={ordering}" if ordering is not None else "unordered"
        if split:
            out.append(
                f"{name} dg={dgram}: id=0x{data[0]:02x} frag {idx}/{cnt} sid={sid} "
                f"rel=0x{rel:02x} {tag} body={dl * (cnt)}B"
            )
        else:
            out.append(
                f"{name} dg={dgram}: id=0x{data[0]:02x} len={len(data)} "
                f"{'whole' if complete else 'partial'} rel=0x{rel:02x} {tag}"
            )
        off = o + dl
        if not complete:
            break


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    text = open(sys.argv[1], errors="replace").read()
    out = []
    ids = Counter()
    # pt: form (complete plaintext)
    for name, rupp, pl, hx in re.findall(
        r"== (\S+) rupp (\d+) pl (\d+)\n\s*pt: ([0-9a-f]*)[^\n]*", text
    ):
        raw = bytes.fromhex(hx)
        parse_datagram(raw, len(raw) + 18 == int(pl), name, out, ids)
    # full_decode form (head only)
    for name, l, hx in re.findall(
        r"== (\S+) ptlen=(\d+)\n\s*head=([0-9a-f]*)[^\n]*", text
    ):
        raw = bytes.fromhex(hx)
        parse_datagram(raw, len(raw) == int(l), name, out, ids)
    print("\n".join(out))
    print("\nid histogram:", {f"0x{k:02x}": v for k, v in sorted(ids.items())})
    return 0


if __name__ == "__main__":
    sys.exit(main())
