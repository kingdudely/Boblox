#!/usr/bin/env python3
"""physics_trace.py — compare the client->server physics stream by value.

`tx_census.py` answers "how often is 0x83/05 sent"; this answers "what transform
did it carry, and did the stream move like the native's". It reads the same
plaintext RakNet datagram captures (ours from RBX_RAKNET_TXDUMP, native from the
gdb DGRAM hook), keeps the game connection, and decodes the native 45-byte
0x83/05 layout:

    83 05 16 | clock_a u32 | 0 | clock_b u32 | 0
             | x f32 | y f32 | z f32 | flags u32 | 01
             | clock_c u32 | f f32 | 00

The position is at byte 19. A capture with no local character has no 0x83/05
and is reported as such rather than treated as a zero-position sample.

Usage:
    python3 tools/physics_trace.py DIR [DIR ...] [--epoch game|last|N]
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tx_diff import epochs, pick_epoch  # noqa: E402


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def f32(b, o):
    return struct.unpack_from('<f', b, o)[0]


def load(directory, epoch):
    """One decode pass -> (samples, id counts, epoch index)."""
    eps = epochs(directory)
    e, idx = pick_epoch(eps, epoch)
    if e is None:
        return [], {}, idx
    rows = sorted(((mt, body) for _, body, mt in e['msgs']),
                  key=lambda r: r[0])
    samples = []
    counts = {}
    for mt, body in rows:
        if not body:
            continue
        key = body[0]
        if key == 0x83 and len(body) > 1:
            key = 0x8300 | body[1]
        counts[key] = counts.get(key, 0) + 1
        if len(body) >= 45 and body[0] == 0x83 and body[1] == 0x05 and \
                body[2] == 0x16:
            samples.append({
                't': mt,
                'clk': u32(body, 3),
                'x': f32(body, 19),
                'y': f32(body, 23),
                'z': f32(body, 27),
                'f': f32(body, 40),
                'size': len(body),
            })
    return samples, counts, idx


def name_id(key):
    if key & 0xff00 == 0x8300:
        return '83.%02x' % (key & 0xff)
    return '%02x   ' % key


def span(v):
    return (min(v), max(v)) if v else (0.0, 0.0)


def report(directory, epoch):
    ss, counts, idx = load(directory, epoch)
    print('\n== %s (epoch %d) ==' % (os.path.basename(directory.rstrip('/')), idx))
    if not ss:
        print('0x83/05 samples: 0 (no local-character physics stream)')
        print('counts: ' + ' '.join('%s=%d' % (name_id(k), v)
                                    for k, v in sorted(counts.items(),
                                                       key=lambda kv: -kv[1])[:10]))
        return 1

    t0, t1 = ss[0]['t'], ss[-1]['t']
    duration = max(t1 - t0, 1e-9)
    xs = [s['x'] for s in ss]
    ys = [s['y'] for s in ss]
    zs = [s['z'] for s in ss]
    sizes = sorted(set(s['size'] for s in ss))
    print('0x83/05 samples: %d over %.2fs (%.2f/s), sizes=%s'
          % (len(ss), duration, len(ss) / duration,
             ','.join(str(v) for v in sizes)))
    print('first: t=%.3f clk=%u pos=(%.3f, %.3f, %.3f) f=%.3f'
          % (ss[0]['t'] - t0, ss[0]['clk'], ss[0]['x'], ss[0]['y'],
             ss[0]['z'], ss[0]['f']))
    print('last : t=%.3f clk=%u pos=(%.3f, %.3f, %.3f) f=%.3f'
          % (ss[-1]['t'] - t0, ss[-1]['clk'], ss[-1]['x'], ss[-1]['y'],
             ss[-1]['z'], ss[-1]['f']))
    print('range: x %.3f..%.3f  y %.3f..%.3f  z %.3f..%.3f'
          % (span(xs) + span(ys) + span(zs)))
    steps = []
    for a, b in zip(ss, ss[1:]):
        steps.append(((b['x'] - a['x']) ** 2 +
                      (b['y'] - a['y']) ** 2 +
                      (b['z'] - a['z']) ** 2) ** 0.5)
    if steps:
        print('step : max=%.3f mean=%.3f  (position deltas between samples)'
              % (max(steps), sum(steps) / len(steps)))
    clk_span = ss[-1]['clk'] - ss[0]['clk']
    print('clock: delta=%d (%.1f/s)' % (clk_span, clk_span / duration))
    print('counts: ' + ' '.join('%s=%d' % (name_id(k), v)
                                for k, v in sorted(counts.items(),
                                                   key=lambda kv: -kv[1])[:10]))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dirs', nargs='+')
    ap.add_argument('--epoch', default='game')
    args = ap.parse_args()
    rc = 0
    for d in args.dirs:
        rc |= report(d, args.epoch)
    return rc


if __name__ == '__main__':
    sys.exit(main())
