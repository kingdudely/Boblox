#!/usr/bin/env python3
"""raknet_native_rx.py — decrypt a native client's received datagrams.

The gdb capture (tools/gdb/rc_capture.gdb) writes every `recvfrom` payload to
`wire_rx_NNN.bin` and logs the second X25519 exchange (`KDF#1 pub=.. sec=..
peer=..`). That is everything needed to reproduce the client's data-layer keys:

    data = BLAKE2b-512(shared || our_pub || peer_pub)
    rx   = data[0:32]   (server -> client)
    tx   = data[32:64]  (client -> server)

Each wire file is a RUPP header followed by an AES-256-GCM frame:

    header: 01 00 <u16 BE total> 01 11 <sub> <16B token> [02 06 <ip4> <u16 BE port>]
    frame : <ciphertext> <u16 LE counter hint> <16B tag>
            nonce = LE64(counter) || "mbeR", no AAD, counter starts at
            LE64("UniqueNu") and increments per datagram received.

Output is a directory of plaintext datagrams (`rNNNNNN.bin`) ready for
`tools/raknet_timeline.py --dump`.
"""
import argparse
import hashlib
import os
import sys

from cryptography.hazmat.primitives.asymmetric.x25519 import (X25519PrivateKey,
                                                             X25519PublicKey)
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

COUNTER_BASE = 0x754E657571696E55  # LE64("UniqueNu")
NONCE_TAIL = b"mbeR"

# RUPP header sizes: with the RCC TLV the native writes 31 bytes, without 23.
HDR_PLAIN, HDR_RCC = 23, 31


def parse_kdf(path):
    """First `KDF#1 pub=.. sec=.. peer=..` line -> (pub, sec, peer)."""
    with open(path, "r", errors="replace") as f:
        for line in f:
            if "KDF#1" not in line:
                continue
            fields = {}
            for part in line.split():
                if "=" in part:
                    k, v = part.split("=", 1)
                    fields[k] = v
            if not {"pub", "sec", "peer"} <= set(fields):
                continue
            try:
                return (bytes.fromhex(fields["pub"]),
                        bytes.fromhex(fields["sec"]),
                        bytes.fromhex(fields["peer"]))
            except ValueError:
                continue
    return None


def derive(pub, sec, peer):
    """`sec` is the client's X25519 *private* key (the log's `sec=` field)."""
    shared = X25519PrivateKey.from_private_bytes(sec).exchange(
        X25519PublicKey.from_public_bytes(peer))
    digest = hashlib.blake2b(shared + pub + peer, digest_size=64).digest()
    return digest[:32], digest[32:]


def header_len(d):
    if len(d) < 4:
        return 0
    total = (d[2] << 8) | d[3]
    if total in (HDR_PLAIN, HDR_RCC) and len(d) > total:
        return total
    return 0


def nonce(counter):
    return counter.to_bytes(8, "little") + NONCE_TAIL


def near_hint(last, hint):
    """Full counter closest to `last` whose low 16 bits are `hint`.

    The wire carries only a 16-bit truncation (native sub_449BDFE), so the base
    of a direction is *not* fixed: the first datagram of a session can sit tens
    of thousands of counts above LE64("UniqueNu") and is recovered from the
    hint, exactly like the client does.
    """
    base = (last & ~0xFFFF) | hint
    if base + 0x8000 <= last:
        base += 0x10000
    elif base > last + 0x8000:
        base -= 0x10000
    return base


def open_frame(key, frame, last):
    """Try counters around the hinted one. -> (pt, ctr)."""
    if len(frame) < 18:
        return None
    ct, hint = frame[:-18], int.from_bytes(frame[-18:-16], "little")
    tag = frame[-16:]
    near = near_hint(last + 1, hint)
    for cand in range(near - 3, near + 6):
        if cand < 0:
            continue
        try:
            return AESGCM(key).decrypt(nonce(cand), ct + tag, None), cand
        except Exception:
            continue
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True, help="capture dir with wire_rx_*.bin")
    ap.add_argument("--log", help="gdb log with the KDF line (default: <dir>/rkc.log)")
    ap.add_argument("--out", required=True, help="output dir for plaintext datagrams")
    ap.add_argument("--direction", choices=["rx", "tx"], default="rx")
    a = ap.parse_args()

    log = a.log or os.path.join(a.dir, "rkc.log")
    kdf = parse_kdf(log)
    if not kdf:
        print(f"no KDF#1 line in {log}", file=sys.stderr)
        return 2
    pub, sec, peer = kdf
    rx, tx = derive(pub, sec, peer)
    key = rx if a.direction == "rx" else tx

    prefix = "wire_rx_" if a.direction == "rx" else "wire_tx_"
    files = sorted(f for f in os.listdir(a.dir)
                   if f.startswith(prefix) and f.endswith(".bin"))
    os.makedirs(a.out, exist_ok=True)
    counter = COUNTER_BASE
    ok = fail = hdr_fail = 0
    for i, name in enumerate(files):
        d = open(os.path.join(a.dir, name), "rb").read()
        h = header_len(d)
        if not h:
            hdr_fail += 1
            continue
        got = open_frame(key, d[h:], counter)
        if not got:
            fail += 1
            continue
        pt, counter = got
        counter += 1
        open(os.path.join(a.out, f"r{i:06d}.bin"), "wb").write(pt)
        ok += 1
    print(f"{a.direction}: {ok} decrypted, {fail} failed, {hdr_fail} bad header "
          f"of {len(files)}; out={a.out}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
