#!/usr/bin/env python3
"""tx_buckets.py — compare two RakNet TX captures bucket by bucket in time.

`tx_census.py` gives one rate per message kind for a whole connection, which is
what a join or a short round needs. Over minutes that hides the shape: a kind
that only starts at minute 4, or a stream that dies at minute 6, averages out.
This tool keeps the same epoch scoping (the game connection only, via
tx_census.messages) and buckets every message by its capture time, so two
captures of different lengths can be compared minute by minute.

Usage:
    python3 tools/tx_buckets.py --native DIR --ours DIR
        [--bucket 60] [--subs] [--native-only] [--top 6]

Both inputs are directories of *plaintext* RakNet datagrams, one file per
datagram, timestamped when written:

  ours    RBX_RAKNET_TXDUMP=<dir>   tNNNNNN.bin
  native  gdb DGRAM# hook           dgram_N.bin

With --native-only, every kind the native sent and we never did is listed with
the buckets it appeared in — the shortest path to a missing-message diagnosis.
A kind that only appears late is a candidate for a scheduled exchange; a flat
mix means the difference is rate or content, not a missing message.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tx_census import label, messages  # noqa: E402


def buckets_of(rows, width):
    if not rows:
        return {}, None
    t0 = rows[0][0]
    out = {}
    for mt, body in rows:
        b = int((mt - t0) // width)
        k = label(body)
        d = out.setdefault(b, {})
        d[k] = d.get(k, 0) + 1
    return out, t0


def name(key):
    mid, sub = key
    return '0x%02x' % mid if not sub else '0x%02x/%02x' % (mid, sub)


def show(title, buckets, width, top):
    total = sum(sum(d.values()) for d in buckets.values())
    last = max(buckets) if buckets else -1
    print('%s: %d messages over %d bucket(s) of %ds'
          % (title, total, last + 1, width))
    for b in sorted(buckets):
        d = buckets[b]
        items = sorted(d.items(), key=lambda kv: -kv[1])[:top]
        print('  %4ds %6d  %s'
              % (b * width, sum(d.values()),
                 ' '.join('%s:%d' % (name(k), v) for k, v in items)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--native', required=True)
    ap.add_argument('--ours', required=True)
    ap.add_argument('--bucket', type=int, default=60)
    ap.add_argument('--subs', action='store_true',
                    help='split 0x83 by its subtype byte')
    ap.add_argument('--native-only', action='store_true')
    ap.add_argument('--top', type=int, default=6)
    a = ap.parse_args()

    nrows, _ = messages(a.native, 'game')
    orows, _ = messages(a.ours, 'game')
    nb, _ = buckets_of(nrows, a.bucket)
    ob, _ = buckets_of(orows, a.bucket)
    if not nb:
        print('%s: no game-epoch messages' % a.native)
    if not ob:
        print('%s: no game-epoch messages' % a.ours)

    show('native', nb, a.bucket, a.top)
    print()
    show('ours', ob, a.bucket, a.top)

    nspan = max(nb) * a.bucket if nb else 1
    ospan = max(ob) * a.bucket if ob else 1
    keys = set(k for d in list(nb.values()) + list(ob.values()) for k in d)
    rows = []
    for k in keys:
        if not a.subs and k[1]:
            continue
        n = sum(d.get(k, 0) for d in nb.values())
        o = sum(d.get(k, 0) for d in ob.values())
        rows.append((k, n, o))
    rows.sort(key=lambda r: -(r[1] + r[2]))
    print()
    print('%-12s %8s %8s %8s %8s  %s'
          % ('id', 'native', 'ours', 'nat/s', 'our/s', 'verdict'))
    for k, n, o in rows:
        verdict = ('same count' if n == o else 'count differs') if n and o \
            else ('native only' if n else 'ours only')
        print('%-12s %8d %8d %8.2f %8.2f  %s'
              % (name(k), n, o, n / nspan, o / ospan, verdict))

    if a.native_only:
        have = set(k for d in ob.values() for k in d)
        miss = []
        for k in set(k for d in nb.values() for k in d):
            if k in have or (not a.subs and k[1]):
                continue
            where = sorted(b for b, d in nb.items() if k in d)
            miss.append((k, sum(nb[b].get(k, 0) for b in where), where))
        miss.sort(key=lambda r: -r[1])
        print()
        print('native-only kinds: %d' % len(miss))
        for k, n, where in miss[:30]:
            print('  %-12s n=%-6d buckets %s%s'
                  % (name(k), n, ','.join(str(b) for b in where[:12]),
                     '...' if len(where) > 12 else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
