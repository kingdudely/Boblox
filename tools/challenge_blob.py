#!/usr/bin/env python3
"""challenge_blob.py — decode a 0x9B challenge blob into standard Luau bytecode.

Reversed from libroblox.so sub_274FA64 (challenge script loader):

  Blob layout (after decrypt):
    [0:4]   check dword  = xxhash32(plaintext, seed=0x2A)
    [4:8]   u32: 0 => raw payload at [8:]; else = uncompressed size
    [8:]    either raw wire bytecode, or a zstd frame (magic 28 b5 2f fd)

  Decrypt:
    key[i]   = ((blob[i] ^ "RSB1"[i]) + [0x00,0xd7,0xae,0x85][i]) & 0xFF   i<4
    plain[p] = blob[p] ^ ((41*p + key[p & 3]) & 0xFF)

  (the derived 4-byte key IS the expected xxhash32 of the plaintext)

Then the wire program is standardized via tools/standardize_wire.py.

Usage:
  challenge_blob.py <wire_chal.bin|.bin raw-blob> <out.luac>
  (library: decode_blob(bytes) -> wire bytes;  blob_to_standard(bytes) -> bytes)
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from standardize_wire import standardize_bytes  # noqa: E402
from zstd_lib import decompress as zstd_decompress  # noqa: E402

MAGIC = b"RSB1"
ADD = bytes([0x00, 0xD7, 0xAE, 0x85])
ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"


# ----------------------------- xxhash32 (pure python) ----------------------
def xxh32(data, seed=0):
    P1, P2, P3, P4, P5 = 0x9E3779B1, 0x85EBCA77, 0xC2B2AE3D, 0x27D4EB2F, 0x165667B1
    M = 0xFFFFFFFF
    rot = lambda x, r: ((x << r) | (x >> (32 - r))) & M
    n = len(data)
    i = 0
    if n >= 16:
        v1 = (seed + P1 + P2) & M
        v2 = (seed + P2) & M
        v3 = seed & M
        v4 = (seed - P1) & M
        while i + 16 <= n:
            for vi, v in ((1, v1), (2, v2), (3, v3), (4, v4)):
                lane = int.from_bytes(data[i:i + 4], "little"); i += 4
                if vi == 1:
                    v1 = (rot((v1 + lane * P2) & M, 13) * P1) & M
                elif vi == 2:
                    v2 = (rot((v2 + lane * P2) & M, 13) * P1) & M
                elif vi == 3:
                    v3 = (rot((v3 + lane * P2) & M, 13) * P1) & M
                else:
                    v4 = (rot((v4 + lane * P2) & M, 13) * P1) & M
        h = (rot(v1, 1) + rot(v2, 7) + rot(v3, 12) + rot(v4, 18)) & M
    else:
        h = (seed + P5) & M
    h = (h + n) & M
    while i + 4 <= n:
        k = int.from_bytes(data[i:i + 4], "little"); i += 4
        h = (rot((h + k * P3) & M, 17) * P4) & M
    while i < n:
        h = (rot((h + data[i] * P5) & M, 11) * P1) & M
        i += 1
    h ^= h >> 15
    h = (h * P2) & M
    h ^= h >> 13
    h = (h * P3) & M
    h ^= h >> 16
    return h


# ----------------------------- blob decode ---------------------------------
def decode_blob(blob):
    """0x9B blob -> wire program (7226B). Raises on bad checksum."""
    if len(blob) < 13:
        raise ValueError("blob too short")
    key = bytes((blob[i] ^ MAGIC[i]) + ADD[i] & 0xFF for i in range(4))
    dec = bytearray(len(blob))
    for p in range(len(blob)):
        dec[p] = blob[p] ^ (41 * p + key[p & 3]) & 0xFF
    check = int.from_bytes(key, "little")
    got = xxh32(bytes(dec), 0x2A)
    if got != check:
        raise ValueError(f"checksum mismatch: got {got:#010x} want {check:#010x}")
    mode = int.from_bytes(dec[4:8], "little")
    payload = bytes(dec[8:])
    if mode == 0:
        return payload
    if payload[:4] != ZSTD_MAGIC:
        raise ValueError("zstd magic missing")
    out = zstd_decompress(payload, mode)
    if len(out) != mode:
        raise ValueError(f"zstd size {len(out)} != {mode}")
    return out


def extract_blob(chal_msg):
    """[9b][u1][u2][len][blob] -> (u1, u2, blob)."""
    if chal_msg[:1] != b"\x9b" or len(chal_msg) < 13:
        raise ValueError("not a 0x9B message")
    u1, u2, ln = struct.unpack_from("<III", chal_msg, 1)
    return u1, u2, chal_msg[13:13 + ln]


def blob_to_standard(blob):
    """blob -> standard Luau bytecode (ready for luau_load)."""
    wire = decode_blob(blob)
    std, stats = standardize_bytes(wire)
    if stats["bad_walks"]:
        raise ValueError(f"proto walk failed: {stats['bad_walks']}")
    return bytes(std), stats


def main():
    src, out = sys.argv[1], sys.argv[2]
    data = open(src, "rb").read()
    if data[:1] == b"\x9b":
        _, _, blob = extract_blob(data)
    else:
        blob = data
    wire = decode_blob(blob)
    print(f"blob {len(blob)}B -> wire {len(wire)}B")
    std, stats = blob_to_standard(blob)
    print(f"standard: protos={stats['protos']} starts={stats['starts']} "
          f"mismatches={stats['mismatches']} bad_walks={stats['bad_walks']}")
    open(out, "wb").write(std)
    print("wrote", out)


if __name__ == "__main__":
    main()
