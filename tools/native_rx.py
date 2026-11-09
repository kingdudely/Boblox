#!/usr/bin/env python3
"""native_rx.py — decrypt the native client's received datagrams.

`tools/gdb/rc_capture.gdb` with `RC_HOOKS=dgram,kdf,rx` records both halves of
what is needed: every `KDF#n pub=.. sec=.. peer=..` line (the X25519 inputs the
engine feeds its BLAKE2b session KDF) and every received datagram verbatim in
`wire_rx_<n>.bin` (RUPP header + sealed RakNet datagram).

The receive direction is sealed with AES-256-GCM under
`digest[0:32] = BLAKE2b-512(shared || our_pub || peer_pub)`, nonce
`LE64(counter) || "mbeR"`, frame `ct || LE16(counter) || tag` — see
`client/src/raknet/session_crypto.h`. Only the low 16 bits of the counter are on
the wire and the counters advance by one per datagram (both directions start at
`LE64("UniqueNu")`), so the counter is tracked across the capture and a small
window around the expected value is tried; GCM authenticates, so a wrong key or
counter fails to open instead of returning garbage.

    python3 tools/native_rx.py run/a21rx              # summary + app messages
    python3 tools/native_rx.py run/a21rx --apps        # only app messages
    python3 tools/native_rx.py run/a21rx --dump 52     # hex of one datagram

Every opened datagram is written to `<dir>/rx_plain/rx_<i>.bin`.
"""
import argparse
import glob
import hashlib
import os
import re
import struct
import sys

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

BASE = 0x754E657571696E55  # LE64("UniqueNu")
MBER = b"mbeR"


def kdf_keys(log_path):
    """(rx, tx) key pairs from the captured KDF calls, in call order."""
    out = []
    pat = re.compile(r"KDF#(\d+) pub=([0-9a-f]{64}) sec=([0-9a-f]{64}) peer=([0-9a-f]{64})")
    try:
        text = open(log_path, "r", errors="replace").read()
    except FileNotFoundError:
        return out
    for _n, pub, sec, peer in pat.findall(text):
        pub, peer = bytes.fromhex(pub), bytes.fromhex(peer)
        try:
            shared = X25519PrivateKey.from_private_bytes(bytes.fromhex(sec)).exchange(
                X25519PublicKey.from_public_bytes(peer))
        except Exception:
            continue
        d = hashlib.blake2b(shared + pub + peer, digest_size=64).digest()
        out.append((d[0:32], d[32:64]))
    return out


def rupp(pkt):
    """(payload, token_subtype, header_len) or None."""
    if len(pkt) < 4 or pkt[0] != 0x01 or pkt[1] != 0x00:
        return None
    total = (pkt[2] << 8) | pkt[3]
    if total < 4 or total > len(pkt):
        return None
    sub = 0
    p, end = 4, total
    while p + 2 <= end:
        t, ln = pkt[p], pkt[p + 1]
        if p + 2 + ln > end:
            break
        if t == 1 and ln == 17:
            sub = pkt[p + 2]
        p += 2 + ln
    return pkt[total:], sub, total


def try_open(key, frame, expected):
    """Plaintext + counter, or (None, None). ``expected`` may be None."""
    if len(frame) < 19:
        return None, None
    ct, hint, tag = frame[:-18], frame[-18:-16], frame[-16:]
    low = struct.unpack("<H", hint)[0]
    if expected is None:
        cands = [(BASE + i) for i in range(0, 256)]
    else:
        base = (expected & ~0xFFFF) | low
        cands = [base, base - 1, base + 1, base + 2, base - 2]
    gcm = AESGCM(key)
    for ctr in cands:
        if ctr < 0:
            continue
        try:
            return gcm.decrypt(struct.pack("<Q", ctr) + MBER, ct + tag, None), ctr
        except Exception:
            continue
    return None, None


def app_messages(plain):
    """(offset, id) for every Roblox application message in a datagram."""
    found = []
    for i in range(len(plain) - 1):
        b0, b1 = plain[i], plain[i + 1]
        if b0 == 0x83 and 0x20 <= b1 <= 0x3a:
            found.append((i, "0x83/%02x" % b1, plain[i:i + 14].hex()))
        elif b0 == 0x15 and len(plain) > i + 4:
            found.append((i, "0x15", plain[i:i + 8].hex()))
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--dump", type=int, default=None)
    ap.add_argument("--apps", action="store_true", help="only print app messages")
    args = ap.parse_args()

    keys = kdf_keys(os.path.join(args.dir, "rkc.log"))
    print("kdf pairs:", len(keys))
    if not keys:
        return 1

    files = sorted(glob.glob(os.path.join(args.dir, "wire_rx_*.bin")),
                   key=lambda p: int(re.search(r"_(\d+)\.bin$", p).group(1)))
    print("wire_rx files:", len(files))
    outdir = os.path.join(args.dir, "rx_plain")
    os.makedirs(outdir, exist_ok=True)

    # One key gets used for the whole capture; the data-phase KDF is the last one.
    order = list(range(len(keys)))[::-1]
    state = {k: None for k in order}
    opened = 0
    apps = {}
    for path in files:
        idx = int(re.search(r"_(\d+)\.bin$", path).group(1))
        r = rupp(open(path, "rb").read())
        if not r:
            continue
        payload, sub, total = r
        if sub != 2 or len(payload) < 19:
            continue
        plain = ctr = used = None
        for ki in order:
            plain, ctr = try_open(keys[ki][0], payload, state[ki])
            if plain is not None:
                used = ki
                break
        if plain is None:
            continue
        state[used] = ctr + 1
        opened += 1
        open(os.path.join(outdir, "rx_%05d.bin" % idx), "wb").write(plain)
        found = app_messages(plain)
        for _off, tag, _h in found:
            apps[tag] = apps.get(tag, 0) + 1
        if args.dump == idx:
            print("rx#%d kdf=%d ctr=%#x len=%d hdr_total=%d" % (idx, used, ctr, len(plain), total))
            print(plain.hex())
        elif found:
            print("rx#%-6d len=%-5d ctr=%#x apps=%s" % (
                idx, len(plain), ctr, " ".join("%s@%d:%s" % (t, o, h) for o, t, h in found)))

    print("opened %d datagrams" % opened)
    if apps:
        print("application messages:")
        for tag, n in sorted(apps.items()):
            print("  %-8s x%d" % (tag, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
