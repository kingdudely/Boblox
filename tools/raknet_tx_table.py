#!/usr/bin/env python3
"""Decode captured RakNet *datagrams* (one file per datagram, pre-seal plaintext
as the client handed them to its socket) into the ordered application-message
stream, so our TX can be diffed against the native's message for message.

Datagram:  u8 flags | u24le number | internal packet*
Internal:  u8 flags | u16be bit length | [u24le message number]   (0x40 set)
           [u24le ordering index | u8 channel]                   (0x20 set)
           [u24le sequencing index]                              (0x10 set)
           [u32be split count | u16be split id | u32be split index]
           payload

Usage: python3 tools/raknet_tx_table.py <dir> [--from N] [--to N]
"""
import sys
import glob
import re

REL, SEQ, ORD = 0x40, 0x10, 0x20


def u24(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


def decode(path):
    b = open(path, 'rb').read()
    if len(b) < 4:
        return None
    out = {'flags': b[0], 'dgram': None, 'pkts': []}
    if b[0] & 0x60:                                  # ACK / NAK datagram
        out['dgram'] = b[1]
        out['pkts'].append({'ack': True, 'payload': b[4:]})
        return out
    out['dgram'] = u24(b, 1)
    o = 4
    while o + 3 <= len(b):
        fl, bits = b[o], (b[o + 1] << 8) | b[o + 2]
        o += 3
        p = {'flags': fl, 'bits': bits}
        try:
            if fl & 0x08:                            # ACK receipt: u32 msg no
                o += 4
            if fl & REL:
                p['msg'] = u24(b, o)
                o += 3
            if fl & ORD:
                p['ord'] = u24(b, o)
                p['chan'] = b[o + 3]
                o += 4
            if fl & SEQ:
                p['seq'] = u24(b, o)
                o += 3
        except IndexError:
            break
        n = (bits + 7) // 8
        p['payload'] = b[o:o + n]
        o += n
        out['pkts'].append(p)
    return out


def tag(p):
    if p.get('ack'):
        return 'ack'
    t = []
    if p['flags'] & REL:
        t.append('rel')
    if p['flags'] & ORD:
        t.append('ord%d' % p['ord'])
    if p['flags'] & SEQ:
        t.append('seq%d' % p['seq'])
    if not p['flags'] & REL:
        t.append('unrel')
    if p['flags'] & 0x08:
        t.append('resend')
    return ' '.join(t)


def main():
    d = sys.argv[1]
    lo, hi = 0, 10 ** 9
    if '--from' in sys.argv:
        lo = int(sys.argv[sys.argv.index('--from') + 1])
    if '--to' in sys.argv:
        hi = int(sys.argv[sys.argv.index('--to') + 1])
    files = sorted(glob.glob(d + '/*.bin'),
                   key=lambda s: int(re.search(r'(\d+)\.bin', s).group(1)))
    parts = {}      # msg number -> list of (index, bytes, count)
    order = []
    for f in files:
        n = int(re.search(r'(\d+)\.bin', f).group(1))
        if not (lo <= n <= hi):
            continue
        r = decode(f)
        if not r:
            continue
        for p in r['pkts']:
            if p.get('ack'):
                pl = p['payload']
                pairs = ['%d..%d' % (u24(pl, i), u24(pl, i + 3))
                         for i in range(0, len(pl) - 5, 6)]
                print('#%-3d dg%-6d ACK %s' % (n, r['dgram'], ' '.join(pairs)))
                continue
            body = p['payload']
            split = None
            if p['flags'] & REL and len(body) >= 10:
                count = int.from_bytes(body[0:4], 'big')
                sid = int.from_bytes(body[4:6], 'big')
                idx = int.from_bytes(body[6:10], 'big')
                if 1 < count <= 4096 and idx < count:
                    split = (count, sid, idx)
                    body = body[10:]
            key = (p.get('msg'), p.get('ord', p.get('seq')))
            if split:
                if key not in parts:
                    parts[key] = [split[0], {}]
                    order.append(key)
                parts[key][1][split[2]] = body
                if len(parts[key][1]) == split[0]:
                    whole = b''.join(parts[key][1][i]
                                     for i in range(split[0]))
                    print('#%-3d dg%-6d %-20s MSG len=%-6d %s  [split %d id %d]'
                          % (n, r['dgram'], tag(p), len(whole),
                             whole[:20].hex(), split[0], split[1]))
                else:
                    print('#%-3d dg%-6d %-20s frag %d/%d id%-3d %5dB'
                          % (n, r['dgram'], tag(p), split[2] + 1, split[0],
                             split[1], len(body)))
            else:
                print('#%-3d dg%-6d %-20s MSG len=%-6d %s'
                      % (n, r['dgram'], tag(p), len(body), body[:20].hex()))


if __name__ == '__main__':
    main()
