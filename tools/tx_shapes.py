#!/usr/bin/env python3
"""tx_shapes.py — show the exact wire shape of each message id in a capture.

For every (id, subtype) seen in a plaintext RakNet TX capture, print the
transport class, the size histogram, and the most frequent bodies in hex, plus
which byte offsets vary.  This is the tool that turns "the native sends 0x83/0a
at 5/s" into "here are its 16 bytes, they all differ at 3 offsets, so the field
layout is ...".

Usage:
    python3 tools/tx_shapes.py DIR [--id 83] [--sub 0a] [--top 3] [--variants 6]
"""
import argparse
import glob
import os
import re
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from raknet_tx_table import decode  # noqa: E402


def numeric_key(path):
    m = re.search(r'(\d+)\.bin$', os.path.basename(path))
    return int(m.group(1)) if m else 0


def collect(directory):
    """-> {(id, sub): {'bodies': [bytes], 'classes': set, 'times': [mt]}}.

    The transport class string records the RakNet internal-packet flags, so
    "reliable ordered on channel 0" can be told apart from "unreliable".
    """
    out = defaultdict(lambda: {'bodies': [], 'classes': set(), 'times': []})
    seen = set()
    for path in sorted(glob.glob(os.path.join(directory, '*.bin')),
                       key=numeric_key):
        try:
            mt = os.stat(path).st_mtime
        except OSError:
            continue
        r = decode(path)
        if r is None:
            continue
        for p in r['pkts']:
            if p.get('ack'):
                continue
            body = p.get('payload') or b''
            if not body:
                continue
            key = (p.get('chan', 0), p.get('ord'), p.get('msg'))
            if key[1] is not None and key in seen:
                continue
            seen.add(key)
            mid = body[0]
            sub = body[1] if mid == 0x83 and len(body) > 1 else 0
            cls = 'rel' if p['flags'] & 0x40 else 'unrel'
            if p['flags'] & 0x20:
                cls += '+ord%d' % p.get('chan', 0)
            if p['flags'] & 0x10:
                cls += '+seq%d' % p.get('chan', 0)
            e = out[(mid, sub)]
            e['bodies'].append(body)
            e['classes'].add(cls)
            e['times'].append(mt)
    return out


def varying_offsets(bodies, min_len):
    offs = []
    for i in range(min_len):
        vals = {b[i] for b in bodies}
        if len(vals) > 1:
            offs.append(i)
    return offs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--id', default='')
    ap.add_argument('--sub', default='')
    ap.add_argument('--top', type=int, default=3,
                    help='most frequent bodies to print per id')
    ap.add_argument('--variants', type=int, default=6,
                    help='consecutive distinct samples to print')
    args = ap.parse_args()

    want_id = int(args.id, 16) if args.id else None
    want_sub = int(args.sub, 16) if args.sub else None

    data = collect(args.dir)
    keys = sorted(data, key=lambda k: (-len(data[k]['bodies']), k))
    for k in keys:
        if want_id is not None and k[0] != want_id:
            continue
        if want_sub is not None and k[1] != want_sub:
            continue
        e = data[k]
        bodies = e['bodies']
        sizes = Counter(len(b) for b in bodies)
        sub = ('%02x.%02x' % k) if k[1] else ('%02x   ' % k[0])
        print('\n== 0x%s == %d messages, classes=%s'
              % (sub, len(bodies), ','.join(sorted(e['classes']))))
        print('sizes: %s' % ' '.join('%dB x%d' % (s, n)
                                      for s, n in sorted(sizes.items())[:8]))
        common = Counter(bodies).most_common(args.top)
        for body, n in common:
            print('  x%-5d %s' % (n, body.hex()))
        # Distinct consecutive samples, so a counter or clock field is obvious.
        shown = []
        for b in bodies:
            if not shown or shown[-1] != b:
                shown.append(b)
        if len(shown) > 1:
            print('  first distinct samples:')
            for b in shown[:args.variants]:
                print('    %s' % b.hex())
        lens = [len(b) for b in bodies]
        if min(lens) > 1:
            offs = varying_offsets(bodies, min(lens))
            print('  varying offsets (min len %d): %s' % (min(lens), offs[:24]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
