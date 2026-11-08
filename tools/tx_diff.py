#!/usr/bin/env python3
"""tx_diff.py — align two RakNet TX captures and report every difference.

Both inputs are directories of *plaintext* RakNet datagrams, one file per
datagram:

  ours    RBX_RAKNET_TXDUMP=<dir>          (pre-seal, what we hand the socket)
  native  gdb `DGRAM#` hook                (pre-encryption, dgram_*.bin)

One capture is NOT one stream.  The client first opens a RakNet connection for
matchmaking, leaves it (0x8f), and only then connects to the game server; a
failed attempt repeats that cycle.  Reliable-ordered indices restart at 0 for
every connection, so keying only on (channel, ordering index) makes a later
connection look like retransmits of the earlier one and the real game traffic
disappears.  This tool therefore splits a capture into epochs at each 0x09
join request and compares the epoch that carries 0x83 (the game channel)
against the other side's game epoch.

Inside an epoch a reliable-ordered message that was split across datagrams is
reassembled by (message number, split id) and keyed by (channel, ordering
index) — its position in the reliable stream.  Alignment is therefore not
positional guessing: a message the native sends and we do not shows up as a
missing key, and a length difference shows up on the pair itself.

Usage:
    python3 tools/tx_diff.py --ours DIR --native DIR [--no-ack]
                             [--show N] [--hex N] [--summary-only]
                             [--only 83,97,a1] [--epoch N|game|last]
                             [--list-epochs]

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


def epochs(directory, since=None, keep_ack=False):
    """-> [epoch dict] where an epoch is one RakNet connection.

    An epoch starts at a 0x09 join request (unreliable, no ordering index) and
    ends at the next one.  A retransmitted join cannot start a second epoch
    because a new epoch is only opened once the current one has real traffic
    beyond its handshake.
    """
    out = []

    def new_epoch(index):
        e = {'msgs': [], 'keys': set(), 'parts': {}, 'acks': 0,
             'start_file': index, 'files': 0}
        out.append(e)
        return e

    cur = new_epoch(0)
    files = sorted(glob.glob(os.path.join(directory, '*.bin')), key=numeric_key)
    for fi, path in enumerate(files):
        if since is not None:
            try:
                if os.stat(path).st_mtime < since:
                    continue
            except OSError:
                continue
        r = decode(path)
        if r is None:
            continue
        try:
            mt = os.stat(path).st_mtime
        except OSError:
            mt = 0.0
        cur['files'] += 1
        for p in r['pkts']:
            if p.get('ack'):
                cur['acks'] += 1
                if keep_ack:
                    cur['msgs'].append(('ack', p.get('payload') or b'', mt))
                continue
            body = p.get('payload') or b''
            if not body:
                continue
            is_join = (body[0] == 0x09 and p['flags'] & 0x40
                       and not p['flags'] & 0x20)
            if is_join and len(cur['msgs']) > 4:
                cur = new_epoch(fi)
            split = None
            if p['flags'] & 0x40 and len(body) >= 10:
                count = int.from_bytes(body[0:4], 'big')
                sid = int.from_bytes(body[4:6], 'big')
                idx = int.from_bytes(body[6:10], 'big')
                if 1 < count <= 4096 and idx < count:
                    split = (count, sid, idx)
                    body = body[10:]
            msgno = p.get('msg', -len(cur['msgs']))
            if split:
                slot = cur['parts'].setdefault((msgno, split[1]),
                                               [split[0], {}])
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
                key = ('unrel', len(cur['msgs']))
            if key in cur['keys']:
                continue                                # retransmit
            cur['keys'].add(key)
            cur['msgs'].append((key, body, mt))
    return out


def appid(body):
    return body[0] if body else -1


def pick_epoch(eps, want):
    if not eps:
        return None, -1
    if want == 'last':
        return eps[-1], len(eps) - 1
    if want != 'game':
        i = int(want, 0)
        return (eps[i] if -len(eps) <= i < len(eps) else None), i
    # game = the connection that carries client->server replication (0x83)
    best, bi, best_n = None, -1, -1
    for i, e in enumerate(eps):
        n = sum(1 for _, b, _ in e['msgs'] if appid(b) == 0x83)
        if n >= best_n and n > 0:
            best, bi, best_n = e, i, n
    if best is None:
        return eps[-1], len(eps) - 1
    return best, bi


def hx(b, n):
    return b[:n].hex()


def diff_bytes(a, b, limit=8):
    n = min(len(a), len(b))
    offs = [i for i in range(n) if a[i] != b[i]]
    return offs[:limit], len(offs)


def describe(eps):
    lines = []
    for i, e in enumerate(eps):
        c = {}
        for _, b, _ in e['msgs']:
            c[appid(b)] = c.get(appid(b), 0) + 1
        top = ' '.join('%02x:%d' % (k, v) for k, v in
                       sorted(c.items(), key=lambda kv: -kv[1])[:6])
        lines.append('  epoch %d: %4d msgs  files=%-6d acks=%-5d  %s'
                     % (i, len(e['msgs']), e['files'], e['acks'], top))
    return '\n'.join(lines)


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
    ap.add_argument('--epoch', default='game',
                    help="which connection to compare: 'game' (default), "
                         "'last', or an index from --list-epochs")
    ap.add_argument('--list-epochs', action='store_true')
    ap.add_argument('--align-to', default='',
                    help='drop native datagrams older than the first datagram '
                         'in this directory (same wall-clock window)')
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

    our_eps = epochs(args.ours)
    nat_eps = epochs(args.native, since)
    print('ours  : %d connection(s) from %s' % (len(our_eps), args.ours))
    print(describe(our_eps))
    print('native: %d connection(s) from %s' % (len(nat_eps), args.native))
    print(describe(nat_eps))
    if args.list_epochs:
        return 0

    oe, oi = pick_epoch(our_eps, args.epoch)
    ne, ni = pick_epoch(nat_eps, args.epoch)
    if oe is None or ne is None:
        print('no comparable epoch (ours=%s native=%s)' % (oi, ni))
        return 2
    print('\ncomparing ours epoch %d against native epoch %d' % (oi, ni))
    ours, nat = oe['msgs'], ne['msgs']
    print('ours  : %d messages, %d acks' % (len(ours), oe['acks']))
    print('native: %d messages, %d acks' % (len(nat), ne['acks']))

    def census(msgs):
        c = {}
        for _, b, _ in msgs:
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

    o_map = {k: b for k, b, _ in ours}
    n_map = {k: b for k, b, _ in nat}
    keys = list(dict.fromkeys([k for k, _, _ in ours] + [k for k, _, _ in nat]))
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
