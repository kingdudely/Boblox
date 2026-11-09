#!/usr/bin/env python3
"""wire_census.py — strict per-message census of a plaintext RakNet TX capture.

`tools/tx_diff.py` keys messages by (channel, ordering index) and splits epochs
on a 0x09 body; that works for aligned sessions, but on an unaligned capture the
epoch picker lands on the wrong connection and the census fills with
misparsed ids. This tool instead parses each datagram exactly as the dialect is
laid out — `81 <u24 dgram> | <flags> <u16 BE bits> [u24 msgno] [u24 ord + chan]
[u24 seq] [10B split] <bits/8 payload>` — and splits connections on the join
ladder (0x09 request, 0x9b accept, 0x90 join body), so the game connection is
identified by carrying 0x9b rather than by a heuristic count.

    python3 tools/wire_census.py DIR [--secs 60] [--json out.json]

Prints, for the game connection: message count by id and (for 0x83) subtype,
the time span covered, and — with --json — a compact per-message list
`[t, id, sub, len]` for diffing against another capture.
"""
import argparse
import glob
import json
import os
import re
import sys


def numeric(path):
    m = re.search(r'(\d+)\.bin$', os.path.basename(path))
    return int(m.group(1)) if m else 0


def parse_datagram(d):
    """-> [(id, sub, payload_len, flags, split)] in wire order, or []."""
    if len(d) < 5 or d[0] != 0x81:
        return []
    o = 4
    out = []
    while o + 3 <= len(d):
        flags = d[o]
        o += 1
        bits = (d[o] << 8) | d[o + 1]
        o += 2
        blen = bits // 8
        split = None
        if flags & 0x40:
            o += 3
        if flags & 0x20:
            o += 4
        if flags & 0x10:
            o += 3
        if flags & 0x08:
            o += 4
        if flags & 0x04:
            if o + 10 > len(d):
                break
            count = int.from_bytes(d[o:o + 4], 'big')
            sid = int.from_bytes(d[o + 4:o + 6], 'big')
            idx = int.from_bytes(d[o + 6:o + 10], 'big')
            split = (count, sid, idx)
            o += 10
        if o + blen > len(d) or blen == 0:
            break
        payload = d[o:o + blen]
        o += blen
        out.append((payload[0], payload[1] if len(payload) > 1 else None,
                    len(payload), flags, split))
        if not flags & 0x80 and blen == 0:
            break
    return out


def load(directory):
    files = sorted(glob.glob(os.path.join(directory, '*.bin')), key=numeric)
    rows = []
    for i, p in enumerate(files):
        try:
            st = os.stat(p)
        except OSError:
            continue
        if st.st_size < 5:
            continue
        try:
            d = open(p, 'rb').read()
        except OSError:
            continue
        for mid, sub, ln, flags, split in parse_datagram(d):
            rows.append({'i': i, 't': st.st_mtime, 'id': mid, 'sub': sub,
                         'len': ln, 'flags': flags,
                         'split': split})
    return rows


def split_connections(rows):
    """Group into connections; the game one is the group carrying 0x9b."""
    groups = [[]]
    for r in rows:
        if r['id'] == 0x09 and len(groups[-1]) > 8:
            groups.append([])
        groups[-1].append(r)
    best, best_n = None, 0
    for g in groups:
        n = sum(1 for r in g if r['id'] == 0x9b)
        if n > best_n:
            best, best_n = g, n
    return best or []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--secs', type=float, default=0,
                    help='only the first N seconds of the connection')
    ap.add_argument('--json', default='')
    ap.add_argument('--show-ids', default='')
    args = ap.parse_args()

    rows = load(args.dir)
    if not rows:
        print('no parseable datagrams in', args.dir)
        return 2
    game = split_connections(rows)
    if not game:
        print('no connection carrying 0x9b (join accept)')
        return 2
    t0 = game[0]['t']
    if args.secs:
        game = [r for r in game if r['t'] - t0 <= args.secs]
    census = {}
    for r in game:
        key = (r['id'], r['sub'] if r['id'] == 0x83 else None)
        c = census.setdefault(key, {'n': 0, 'bytes': 0, 'first': 1e9, 'last': -1e9})
        c['n'] += 1
        c['bytes'] += r['len']
        c['first'] = min(c['first'], r['t'] - t0)
        c['last'] = max(c['last'], r['t'] - t0)
    print('%s: game connection, %d messages, %d by id, span %.1fs'
          % (args.dir, len(game), len(census), game[-1]['t'] - t0))
    print(' id/sub     n   bytes   first    last')
    for (mid, sub), c in sorted(census.items()):
        tag = '0x%02x' % mid + ('/%02x' % sub if sub is not None else '    ')
        print(' %-9s %5d %7d  %6.2f  %6.2f' % (tag, c['n'], c['bytes'],
                                               c['first'], c['last']))
    if args.json:
        out = [[round(r['t'] - t0, 3), r['id'], r['sub'], r['len']] for r in game]
        with open(args.json, 'w') as f:
            json.dump(out, f)
        print('wrote', args.json, len(out), 'messages')
    return 0


if __name__ == '__main__':
    sys.exit(main())
