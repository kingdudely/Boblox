#!/usr/bin/env python3
"""rx_dump_census.py — enumerate app messages in rxopen_*.bin dumps.

`tools/gdb/rc_capture.gdb` with `RC_HOOKS=rxopen` dumps the plaintext of every
datagram the engine *accepted* (the argument to the AEAD open is the output
buffer, so what lands on disk is exactly what passed authentication). Unlike an
offline decryptor this has full coverage by construction — no key material and
no stream resync involved — which is what makes it able to answer "does the RCC
ever challenge the native client?".

The framing mirrored here is `walk_messages()` in `client/src/raknet/protocol.cpp`:

    datagram  [flags][u24 datagram id]                 (ACK/NAK: no id)
    message   [rel][u16 BE bit length]
              [u24 LE message number]   if reliable
              [u24 LE ordering][u8 ch]  if ordered
              [u32 BE count][u16 BE id][u32 BE index]  if split
              [payload]

`rel >> 5` is the reliability class, `rel & 0x10` is the split bit and
`rel & 0x0f` must be zero. Every datagram is checked to consume exactly its
length, so a parse error shows up as `bad` instead of as a silent misread.

    python3 tools/rx_dump_census.py run/rxopen1
    python3 tools/rx_dump_census.py run/rxopen1 --grep 20
"""
import argparse
import glob
import os
import re
import sys


def walk(d):
    """Yield (rel, payload) or raise ValueError on a framing error."""
    out, off, n = [], 0, len(d)
    while off + 3 <= n:
        rel = d[off]
        if rel & 0x0F:
            raise ValueError("reserved low nibble at %d" % off)
        bits = (d[off + 1] << 8) | d[off + 2]
        dlen = (bits + 7) // 8
        cls = rel >> 5
        off += 3
        if cls in (2, 3, 4):          # reliable classes carry a message number
            off += 3
        if cls in (1, 3, 4):          # ordered classes carry ordering + channel
            off += 4
        if rel & 0x10:                # split
            off += 10
        if off + dlen > n:
            raise ValueError("payload overruns at %d" % off)
        out.append((rel, d[off:off + dlen]))
        off += dlen
    if off != n:
        raise ValueError("trailing %d bytes" % (n - off))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--grep", default=None,
                    help="only print 0x83 messages whose sub id is this hex")
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.dir, "rxopen_*.bin")),
                   key=lambda p: int(re.search(r"_(\d+)\.bin$", p).group(1)))
    if not files:
        print("no rxopen_*.bin in %s" % args.dir)
        return 1

    hist, ack, nak, bad, msgs = {}, 0, 0, 0, 0
    found = []
    for path in files:
        idx = int(re.search(r"_(\d+)\.bin$", path).group(1))
        d = open(path, "rb").read()
        if len(d) < 4:
            bad += 1
            continue
        flags = d[0]
        if (flags & 0xC0) == 0xC0:
            ack += 1
            continue
        if (flags & 0xE0) == 0xA0:
            nak += 1
            continue
        if not flags & 0x80:
            bad += 1
            continue
        try:
            packets = walk(d[4:])
        except ValueError:
            bad += 1
            continue
        for rel, payload in packets:
            msgs += 1
            if not payload:
                continue
            tag = "0x%02x" % payload[0]
            if payload[0] == 0x83 and len(payload) > 1:
                tag = "0x83/%02x" % payload[1]
            hist[tag] = hist.get(tag, 0) + 1
            if payload[0] == 0x83 and len(payload) > 1:
                sub = "%02x" % payload[1]
                if args.grep and sub == args.grep:
                    found.append((idx, payload))

    print("datagrams: %d  ack: %d  nak: %d  unparsed: %d  messages: %d" % (
        len(files), ack, nak, bad, msgs))
    print("app message kinds:")
    for tag, n in sorted(hist.items(), key=lambda kv: (-kv[1], kv[0]))[:40]:
        print("  %-10s x%d" % (tag, n))
    if args.grep:
        print("0x83/%s messages: %d" % (args.grep, len(found)))
        for idx, payload in found[:20]:
            print("  rxopen#%-6d len=%-5d %s" % (idx, len(payload), payload[:48].hex()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
