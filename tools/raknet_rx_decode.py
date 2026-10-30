#!/usr/bin/env python3
"""raknet_rx_decode.py — turn the native capture's received UDP frames into the
app-message stream, so the server side of a live join can be diffed against
ours.

Input is a directory written by tools/gdb/rc_capture.gdb with the RX hook
enabled:

    rkc.log          KDF# lines: pub / sec / peer (32 bytes each) per DH
    wire_rx_NNN.bin  one raw received UDP frame per file, in arrival order

Frame shape (both directions, verified against live captures):

    [01][0000][u16be header length][01][11][subtype][token 16B][sealed datagram]

The datagram itself is the AES-256-GCM frame from the native's SessionCrypto:
ciphertext, then the 16-bit counter hint, then the 16-byte tag — exactly what
client/src/raknet/session_crypto.cpp's datagramSeal/datagramOpen implement.

Keys: the KDF is BLAKE2b-512(X25519(sec, peer) || pub || peer) and rx is its
first 32 bytes. The session does two DHs; the *last* KDF entry is the
post-handshake data key (the first is the handshake key), which is what the
datagrams use.

Usage: python3 tools/raknet_rx_decode.py /tmp/rkn_rx [--raw]
"""
import glob
import re
import struct
import sys

from cryptography.hazmat.primitives.asymmetric.x25519 import (
    X25519PrivateKey, X25519PublicKey)
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives import hashes

COUNTER_BASE = 0x754E657571696E55  # LE64("UniqueNu")


def u24(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


def derive(sec, pub, peer):
    shared = X25519PrivateKey.from_private_bytes(sec).exchange(
        X25519PublicKey.from_public_bytes(peer))
    d = hashes.Hash(hashes.BLAKE2b(64))
    d.update(shared + pub + peer)
    return d.finalize()


def kdfs(path):
    out = []
    for line in open(path, errors='ignore'):
        m = re.match(r'KDF#\d+ pub=([0-9a-f]+) sec=([0-9a-f]+) peer=([0-9a-f]+)',
                     line.strip())
        if m:
            out.append(tuple(bytes.fromhex(g) for g in m.groups()))
    return out


def counter_near(hint, last):
    c = (last & ~0xFFFF) | hint
    if c < last and last - c > 0x8000:
        c += 0x10000
    elif c > last and c - last > 0x8000:
        c -= 0x10000
    return c


def open_frame(key, frame, last):
    """Try the counter candidates around the wire hint, like datagramOpen."""
    if len(frame) < 18:
        return None
    ct, hint, tag = frame[:-18], frame[-18:-16], frame[-16:]
    near = counter_near(hint[0] | (hint[1] << 8), last)
    aes = AESGCM(key)
    for c in (near, near - 1, near + 1, near + 2):
        nonce = struct.pack('<Q', c & 0xFFFFFFFFFFFFFFFF) + b'mbeR'
        try:
            return aes.decrypt(nonce, ct + tag, None), c
        except Exception:
            continue
    return None


def u16(b, o):
    return (b[o] << 8) | b[o + 1]


class Framer:
    """RakNet datagram -> complete app messages (same rules as rx_stream.cpp)."""

    def __init__(self, show_raw=False):
        self.parts = {}
        self.show_raw = show_raw

    def feed(self, d, out):
        if len(d) < 4:
            return
        if d[0] & 0x60:          # ACK / NAK datagram: no application payload
            return
        dg = u24(d, 1)
        o = 4
        while o + 3 <= len(d):
            fl, bits = d[o], u16(d, o + 1)
            o += 3
            msg = ordv = seq = chan = None
            if fl & 0x08:
                o += 4
            try:
                if fl & 0x40:
                    msg = u24(d, o)
                    o += 3
                if fl & 0x20:
                    ordv, chan = u24(d, o), d[o + 3]
                    o += 4
                if fl & 0x10:
                    seq = u24(d, o)
                    o += 3
            except IndexError:
                break
            # `bits` covers the split header too, so the readable payload is
            # bits/8 minus the 10 header bytes when the message is split.
            n = bits // 8
            raw = d[o:o + n]
            o += n
            if len(raw) < n:
                break
            split = None
            # Split header, two shapes on this wire:
            #   ordered (0x70): u32be count | u16be id | u32be index  = 10B
            #   sequenced (0x50): u8 count | u16be id | u32be index   = 7B
            # (0x90/0x8a upstream and 0x93/0x9b/0x97 downstream both use them).
            if fl & 0x40 and len(raw) >= 10 and fl & 0x20:
                c = int.from_bytes(raw[0:4], 'big')
                sid = int.from_bytes(raw[4:6], 'big')
                idx = int.from_bytes(raw[6:10], 'big')
                if 1 < c <= 4096 and idx < c:
                    split = (c, sid, idx)
                    raw = raw[10:]
                else:
                    # The RCC writes the same fields with a one-byte count
                    # (`02 0002 00000000` + body) while the client writes the
                    # 32-bit count form. Fall back to the short shape.
                    c, sid, idx = raw[0], int.from_bytes(raw[1:3], 'big'), \
                        int.from_bytes(raw[3:7], 'big')
                    if 1 < c <= 255 and idx < c:
                        split = (c, sid, idx)
                        raw = raw[7:]
            elif fl & 0x40 and len(raw) >= 7 and fl & 0x10:
                c = raw[0]
                sid = int.from_bytes(raw[1:3], 'big')
                idx = int.from_bytes(raw[3:7], 'big')
                if 1 < c <= 255 and idx < c:
                    split = (c, sid, idx)
                    raw = raw[7:]
            if split:
                # Fragments of one split message carry *different* reliable
                # message numbers; they share the ordering index (or sequencing
                # index) and the split id.
                key = (ordv, split[1]) if ordv is not None else (seq, split[1])
                if key not in self.parts:
                    self.parts[key] = [split[0], {}]
                self.parts[key][1][split[2]] = raw
                if len(self.parts[key][1]) == split[0]:
                    whole = b''.join(self.parts[key][1][i]
                                     for i in range(split[0]))
                    out.append((dg, whole[0], len(whole), ordv, seq,
                                'split %d' % split[0], whole))
            else:
                out.append((dg, raw[0] if raw else -1, len(raw), ordv, seq,
                            '', raw))


def main():
    d = sys.argv[1]
    tx = '--tx' in sys.argv
    logs = kdfs(d + '/rkc.log')
    if not logs:
        print('no KDF# lines in %s/rkc.log' % d)
        return 1
    pub, sec, peer = logs[-1]
    keys = derive(sec, pub, peer)
    print('# %d KDF entries; using the last (data) key %s'
          % (len(logs), 'tx' if tx else 'rx'))

    side = 'wire_tx_*.bin' if tx else 'wire_rx_*.bin'
    files = sorted(glob.glob(d + '/' + side),
                   key=lambda s: int(re.search(r'(\d+)\.bin', s).group(1)))
    fr = Framer()
    last = COUNTER_BASE
    idx = 0
    framed = 0
    # rx is digest[0:32] on this build (see session_crypto.h); the other half is
    # tried as a fallback so a flipped layout shows up as data, not as silence.
    candidates = [keys[32:64], keys[:32]] if tx else [keys[:32], keys[32:64]]
    for f in files:
        b = open(f, 'rb').read()
        # RUPP wrapper: 01 00 00 <hdrlen> 01 11 <subtype> <token16>; the header
        # length is 23 on receive and 31 on send, so take it from the frame.
        if len(b) < 8 or b[0] != 0x01 or b[4] != 0x01 or b[5] != 0x11:
            continue
        hdr = u16(b, 2)
        if hdr < 8 or hdr >= len(b):
            continue
        opened = None
        for key in candidates:
            opened = open_frame(key, b[hdr:], last)
            if opened:
                break
        if not opened:
            continue
        plain, last = opened
        framed += 1
        msgs = []
        fr.feed(plain, msgs)
        for dg, mid, sz, ordv, seq, extra, body in msgs:
            where = 'ord%d' % ordv if ordv is not None else (
                'seq%d' % seq if seq is not None else 'unrel')
            print('%s%-4d dg%-5d 0x%02x %-7s %-6dB %s %s'
                  % ('tx' if tx else 'rx', idx, dg, mid & 0xFF, where, sz,
                     extra, body[:16].hex()))
        idx += 1
    print('# %d/%d frames opened, %d datagrams decoded'
          % (framed, len(files), idx))
    return 0


if __name__ == '__main__':
    sys.exit(main())
