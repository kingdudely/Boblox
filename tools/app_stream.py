#!/usr/bin/env python3
"""app_stream.py — RakNet datagram -> application messages, mirroring
client/src/raknet/protocol.cpp (walk_messages) and rx_stream.cpp (split reassembly).

    app_stream.py DIR [DIR...]

Each DIR holds raw datagram plaintext files (RBX_RAKNET_TXDUMP output, or the
native capture's dgram_*.bin). Prints one line per complete application message:
    idx  id  sub  len  head
"""
import glob
import os
import sys

REL_UNRELIABLE, REL_UNRELIABLE_SEQ, REL_RELIABLE, REL_ORDERED, REL_REL_SEQ = 0, 1, 2, 3, 4


def u16be(b, o):
    return (b[o] << 8) | b[o + 1]


def u24le(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


def u32be(b, o):
    return int.from_bytes(b[o:o + 4], 'big')


def walk(d, out):
    """Yield complete app payloads from one datagram."""
    if len(d) < 4:
        return
    flags = d[0]
    if (flags & 0xC0) == 0xC0 or (flags & 0xE0) == 0xA0:
        return
    if not flags & 0x80:
        return
    off = 4
    splits = {}
    while off + 3 <= len(d):
        rel = d[off]
        reliability = rel >> 5
        is_split = bool(rel & 0x10)
        if rel & 0x0F:
            return
        dlen = (u16be(d, off + 1) + 7) // 8
        off += 3
        if reliability in (REL_RELIABLE, REL_ORDERED, REL_REL_SEQ):
            off += 3
        if reliability in (REL_UNRELIABLE_SEQ, REL_ORDERED, REL_REL_SEQ):
            off += 4
        if is_split:
            if off + 10 > len(d):
                return
            count = u32be(d, off)
            sid = u16be(d, off + 4)
            idx = u32be(d, off + 6)
            off += 10
            if count == 0 or idx >= count or off + dlen > len(d):
                return
            e = splits.setdefault(sid, [count, {}])
            e[1][idx] = d[off:off + dlen]
            off += dlen
            if len(e[1]) == e[0]:
                out.append(b''.join(e[1][i] for i in range(e[0])))
            continue
        if off + dlen > len(d):
            return
        out.append(d[off:off + dlen])
        off += dlen


def stream(paths):
    msgs = []
    for f in paths:
        try:
            d = open(f, 'rb').read()
        except OSError:
            continue
        walk(d, msgs)
    return msgs


def files_in(d):
    fs = glob.glob(os.path.join(d, '*.bin'))
    def key(p):
        base = os.path.basename(p)
        digits = ''.join(ch for ch in base if ch.isdigit())
        return int(digits) if digits else 0
    return sorted(fs, key=key)


def main():
    for d in sys.argv[1:]:
        msgs = stream(files_in(d))
        print('# %s: %d datagrams -> %d app messages' % (d, len(files_in(d)), len(msgs)))
        for i, m in enumerate(msgs[:40]):
            mid = m[0] if m else 0
            sub = m[1] if len(m) > 1 else 0
            print('  %3d  0x%02x  sub=0x%02x  len=%4d  %s'
                  % (i, mid, sub, len(m), m[:12].hex()))


if __name__ == '__main__':
    main()
