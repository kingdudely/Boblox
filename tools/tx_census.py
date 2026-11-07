#!/usr/bin/env python3
"""tx_census.py — per-message-id census of a plaintext RakNet TX capture.

Answers the question a parity round actually opens with: for every application
id (and, for 0x83, every subtype), how often does each client send it, how big
are the messages, and what is the gap between them.

Datagrams are ordered by *file mtime*, i.e. wall clock at send time, so the two
captures can be compared over the same elapsed window instead of over "however
long each process happened to run".  That distinction matters: the native
driver captures for the round length plus a warm-up, so a whole-directory diff
reports tens of thousands of phantom "missing" messages that are really just
messages sent before our client had even connected.

Usage:
    python3 tools/tx_census.py DIR [DIR ...] [--align ours]
        [--ids 83,a6,a1] [--min 0.2]

--align ours trims every capture to the wall-clock window that starts at the
first datagram of the first directory and lasts as long as that directory's
capture (so it compares like with like).
"""
import argparse
import glob
import os
import re
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from raknet_tx_table import decode  # noqa: E402


def numeric_key(path):
    m = re.search(r'(\d+)\.bin$', os.path.basename(path))
    return int(m.group(1)) if m else 0


def messages(directory):
    """-> [(mtime, body)] in send order, retransmits kept out."""
    rows = []
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
            rows.append((mt, body))
    rows.sort(key=lambda t: t[0])
    return rows


def label(body):
    """(id, subtype) for a message body; subtype 0 when the id has none."""
    if not body:
        return (-1, 0)
    mid = body[0]
    if mid == 0x83 and len(body) > 1:
        return (mid, body[1])
    return (mid, 0)


def gaps(times):
    if len(times) < 2:
        return []
    return [b - a for a, b in zip(times, times[1:])]


def pct(v, p):
    if not v:
        return 0.0
    s = sorted(v)
    return s[min(len(s) - 1, int(p * len(s)))]


def report(name, rows, t0, t1, ids, min_rate):
    span = max(t1 - t0, 1e-9)
    buckets = defaultdict(list)
    sizes = defaultdict(set)
    for mt, body in rows:
        if mt < t0 or mt > t1:
            continue
        k = label(body)
        if ids and k[0] not in ids:
            continue
        buckets[k].append(mt)
        sizes[k].add(len(body))
    print('\n== %s ==  %.1fs window, %d messages, %.1f/s'
          % (name, span, sum(len(v) for v in buckets.values()),
             sum(len(v) for v in buckets.values()) / span))
    print('id.sub   count    rate/s   gap p50   gap p90   sizes')
    for k in sorted(buckets, key=lambda k: (-len(buckets[k]), k)):
        ts = buckets[k]
        rate = len(ts) / span
        if rate < min_rate:
            continue
        g = gaps(ts)
        sz = ','.join(str(s) for s in sorted(sizes[k])[:6])
        sub = ('%02x.%02x' % k) if k[1] else ('%02x   ' % k[0])
        print('%-7s %6d  %8.2f  %8.1f  %8.1f  %s'
              % (sub, len(ts), rate,
                 pct(g, 0.5) * 1000, pct(g, 0.9) * 1000, sz))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dirs', nargs='+')
    ap.add_argument('--align', action='store_true',
                    help='trim all captures to the first capture window')
    ap.add_argument('--ids', default='')
    ap.add_argument('--min', type=float, default=0.2,
                    help='hide rows slower than this (per second)')
    args = ap.parse_args()

    ids = set()
    for tok in args.ids.replace(' ', ',').split(','):
        if tok:
            ids.add(int(tok, 16))

    per = []
    for d in args.dirs:
        rows = messages(d)
        if not rows:
            print('%s: no messages' % d)
            continue
        per.append((d, rows, rows[0][0], rows[-1][0]))

    if not per:
        return 1

    if args.align and len(per) > 1:
        t0 = per[0][2]
        t1 = per[0][3]
        print('aligned window: %.1fs from %s'
              % (t1 - t0, os.path.basename(per[0][0])))
    else:
        t0, t1 = None, None

    for d, rows, a, b in per:
        report(os.path.basename(d), rows,
               t0 if t0 is not None else a,
               t1 if t1 is not None else b,
               ids, args.min)
    return 0


if __name__ == '__main__':
    sys.exit(main())
