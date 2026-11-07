#!/usr/bin/env python3
"""tx_diff.py — align two RakNet TX captures and report every difference.

Both inputs are directories of *plaintext* RakNet datagrams, one file per
datagram:

  ours    RBX_RAKNET_TXDUMP=<dir>          (pre-seal, what we hand the socket)
  native  gdb `DGRAM#` hook                (pre-encryption, dgram_*.bin)

Datagram/internal-packet framing is decoded by `raknet_tx_table.decode`, so a
reliable-ordered message that was split across datagrams is reassembled here
by (channel, ordering index, split id) and keyed by (channel, ordering index).
That key is the message's position in the reliable stream: if the two clients
send the same messages in the same order, the keys line up and each pair is
compared byte-for-byte. Alignment is therefore not positional guessing — a
message the native sends and we do not shows up as a missing key, and a length
difference shows up on the pair itself.

Usage:
    python3 tools/tx_diff.py --ours DIR --native DIR [--no-ack]
                             [--show N] [--hex N] [--summary-only]
                             [--only 83,97,a1]

Exit status is 1 when any difference was found, so a parity round can gate on
it.
"""
import argparse
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from raknet_tx_table import decode  # noqa: E402


def numeric_key(path):
    m = re.search(r'(\d+)\.bin$', os.path.basename(path))
    return int(m.group(1)) if m else 0


def stream(directory, keep_ack=False, since=None):
    """-> (ordered [key], {key: body}, ack count).

    `since` is a wall-clock lower bound on the datagram's mtime. The native
    driver captures for the round length plus a warm-up, so comparing whole
    directories reports every message the native sent before our client had
    even connected as "missing". Pass --align-to OURS to compare only the
    window in which both clients were actually in the job.
    "

    A split packet carries its header in the payload prefix
    ([u32 BE count][u16 BE id][u32 BE index]) and shares a reliable message
    number across fragments, so fragments are grouped on (message number,
    split id) and only surface once every index arrived.
    """
    order = []
    msgs = {}
    acks = 0
    parts = {}          # (msg number, split id) -> [count, {idx: bytes}]
    for path in sorted(glob.glob(os.path.join(directory, '*.bin')),
                       key=numeric_key):
        if since is not None:
            try:
                if os.stat(path).st_mtime < since:
                    continue
            except OSError:
                continue
        r = decode(path)
        if r is None:
            continue
        for p in r['pkts']:
            if p.get('ack'):
                acks += 1
                continue
            body = p.get('payload') or b''
            if not body:
                continue
            split = None
            if p['flags'] & 0x40 and len(body) >= 10:
                count = int.from_bytes(body[0:4], 'big')
                sid = int.from_bytes(body[4:6], 'big')
                idx = int.from_bytes(body[6:10], 'big')
                if 1 < count <= 4096 and idx < count:
                    split = (count, sid, idx)
                    body = body[10:]
            msgno = p.get('msg', -len(order))
            if split:
                slot = parts.setdefault((msgno, split[1]), [split[0], {}])
                slot[1][split[2]] = body
                if len(slot[1]) != slot[0]:
                    continue                            # still incomplete
                body = b''.join(slot[1][i] for i in range(slot[0]))
            chan = p.get('chan', 0)
            if 'ord' in p:
                key = (chan, p['ord'])
            elif 'seq' in p:
                key = ('seq', chan, p['seq'])
            else:
                key = ('unrel', len(order))
            if key in msgs:
                continue                                # retransmit
            msgs[key] = body
            order.append(key)
    return order, msgs, acks


def bodies(directory, keep_ack=False, since=None):
    order, msgs, acks = stream(directory, keep_ack, since)
    return [(k, msgs[k]) for k in order], acks


def hx(b, n):
    return b[:n].hex()


def diff_bytes(a, b, limit=8):
    n = min(len(a), len(b))
    offs = [i for i in range(n) if a[i] != b[i]]
    return offs[:limit], len(offs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ours', required=True)
    ap.add_argument('--native', required=True)
    ap.add_argument('--no-ack', action='store_true')
    ap.add_argument('--show', type=int, default=40,
                    help='max differing pairs to print in full')
    ap.add_argument('--hex', type=int, default=24,
                    help='payload bytes shown per message')
    ap.add_argument('--summary-only', action='store_true')
    ap.add_argument('--only', default='',
                    help='comma-separated app ids to restrict the diff to')
    ap.add_argument('--align-to', default='',
                    help='drop datagrams older than the first datagram in '
                         'this directory (same wall-clock window)')
    args = ap.parse_args()

    only = set()
    for tok in args.only.replace(' ', ',').split(','):
        if tok:
            only.add(int(tok, 16))

    since = None
    if args.align_to:
        starts = [os.stat(p).st_mtime for p in
                  glob.glob(os.path.join(args.align_to, '*.bin'))]
        if starts:
            since = min(starts)
            print('aligned to %s from %.3f' % (args.align_to, since))
    ours, ours_acks = bodies(args.ours, not args.no_ack)
    nat, nat_acks = bodies(args.native, not args.no_ack, since)

    def appid(body):
        return body[0] if body else -1

    print('ours  : %d messages from %s' % (len(ours), args.ours))
    print('native: %d messages from %s' % (len(nat), args.native))
    print('acks  : ours %d / native %d' % (ours_acks, nat_acks))

    # Per-id census answers "what does the native send that we never do" even
    # when the streams are shifted, which is the question a parity round opens
    # with.
    def census(msgs):
        c = {}
        for _, b in msgs:
            c[appid(b)] = c.get(appid(b), 0) + 1
        return c

    co, cn = census(ours), census(nat)
    ids = sorted(set(co) | set(cn))
    if only:
        ids = [i for i in ids if i in only]
    print('\nid      ours  native  verdict')
    for i in ids:
        a, b = co.get(i, 0), cn.get(i, 0)
        verdict = 'same count' if a == b else ('native only' if not a else
                                               'ours only' if not b else
                                               'count differs')
        print('0x%02x  %6d  %6d  %s' % (i, a, b, verdict))

    if args.summary_only:
        return 0

    o_map = dict(ours)
    n_map = dict(nat)
    keys = list(dict.fromkeys([k for k, _ in ours] + [k for k, _ in nat]))
    same = diff_len = diff_byte = missing = 0
    shown = 0
    print()
    for k in keys:
        a, b = o_map.get(k), n_map.get(k)
        if only and appid(a or b) not in only:
            continue
        if a is None:
            missing += 1
            if shown < args.show:
                print('MISSING ours   key=%s native id=0x%02x len=%d %s'
                      % (k, appid(b), len(b), hx(b, args.hex)))
                shown += 1
            continue
        if b is None:
            missing += 1
            if shown < args.show:
                print('MISSING native key=%s ours id=0x%02x len=%d %s'
                      % (k, appid(a), len(a), hx(a, args.hex)))
                shown += 1
            continue
        if a == b:
            same += 1
            continue
        if len(a) != len(b):
            diff_len += 1
            if shown < args.show:
                print('LEN   key=%s id=0x%02x ours=%dB native=%dB'
                      % (k, appid(a), len(a), len(b)))
                print('        ours   %s' % hx(a, 48))
                print('        native %s' % hx(b, 48))
                shown += 1
            continue
        diff_byte += 1
        offs, total = diff_bytes(a, b)
        if shown < args.show:
            print('BYTES key=%s id=0x%02x len=%d  %d byte(s) differ at %s'
                  % (k, appid(a), len(a), total, offs))
            print('        ours   %s' % hx(a, max(args.hex, (offs[-1] + 4)
                                                  if offs else args.hex)))
            print('        native %s' % hx(b, max(args.hex, (offs[-1] + 4)
                                                  if offs else args.hex)))
            shown += 1

    print('\nidentical=%d  byte-diff=%d  length-diff=%d  missing=%d'
          % (same, diff_byte, diff_len, missing))
    return 0 if (diff_byte or diff_len or missing) == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
