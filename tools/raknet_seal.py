#!/usr/bin/env python3
"""raknet_seal.py — reference implementation of the native RakNet message
envelope ("seal") used by the join messages 0x90 and 0x8a.

Reverse-engineered from `sub_664D9B0` (seal) / `sub_664D095` + `sub_6CB8791`
(cipher init) of the 2026-10-27 libroblox.so build; every claim here is checked
byte-for-byte against live native captures (see verify()).

Shape, for a plaintext body P of length L, key K (16 bytes) and PRNG state S0:

    pad   = (~(L + 5)) & 0xF                  # 0..15
    out   = [u32 checksum][r0][r1][pad bytes][P]           (L + pad + 6 bytes)
    r0    = rand(S0)
    S1    = M*S0 + C ;  S2 = M*S1 + C         # M = 0x685F98A2018FADE9
    r1    = pad | (16 * (rand(S1) & 0xF))     # low nibble carries pad
    pad[i]= rand(S_i)                          # S_{i+1} = M*S_i + C
    rand(s)= ror32((s >> 27) ^ (s >> 45), (s >> 59) & 31) & 0xFF
    checksum = fold over out[4:] : v = b ^ ((h >> 8) & 0xFF);
                                        h = (h + v) * -12691 + 22719
                                        acc += v      (stored LE at out[0:4])

    C0 = AES128(K, out[0:16])                  # header block, IV for the rest
    Ci = AES128(K, out[16i:16i+16] ^ C_{i+1})  # i from the last block down to 1

so the ciphertext chains *backwards* from the last block to block 1, with the
header block as the IV.  outlen = L + pad + 6 is always a multiple of 16.

The caller (sub_648C61A) seals only bytes [1:], leaving the message id
(0x90 / 0x8a) cleartext, and the PRNG state comes from the cipher init, so the
whole envelope is deterministic for a given (key, length).
"""
import os
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

M = 0x685F98A2018FADE9
C = 0x399D2694695129DE
M64 = (1 << 64) - 1

# Captured from live native sessions (tools/gdb/rc_capture.gdb SEAL# hook).
KEY90 = bytes.fromhex("fef9f0ebe2ddd4cfc6c1b8b3aaa59c97")
STATE90 = 0xE7DD5C9C5148F144
KEY8A = b"1818-17338116125"          # ("%d%d%d%d" % (placeId, -1733811612, tail[2], 594292182))[:16]
STATE8A = 0xC9AAA1F29BCF8572


def _ror32(x, r):
    r &= 31
    return ((x >> r) | (x << ((32 - r) & 31))) & 0xFFFFFFFF


def _rand_byte(state):
    return _ror32(((state >> 27) ^ (state >> 45)) & 0xFFFFFFFF, (state >> 59) & 31) & 0xFF


def _aes_block(key, block):
    enc = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    return enc.update(block)


def _aes_block_dec(key, block):
    dec = Cipher(algorithms.AES(key), modes.ECB()).decryptor()
    return dec.update(block)


def seal(plain, key=KEY90, state0=STATE90):
    """Encrypt one body exactly like sub_664D9B0."""
    n = len(plain)
    pad = (~(n + 5)) & 0xF
    outlen = n + pad + 6
    assert outlen % 16 == 0, outlen
    r0 = _rand_byte(state0)
    s1 = (M * state0 + C) & M64
    state = (M * s1 + C) & M64
    r1 = (pad + 16 * (_rand_byte(s1) & 0xF)) & 0xFF
    out = bytearray(outlen)
    out[4] = r0
    out[5] = r1
    out[pad + 6:] = plain
    for i in range(pad):
        out[6 + i] = _rand_byte(state)
        state = (M * state + C) & M64
    h = 0xD971   # asm: "mov dx, 0D971h" (decompiler prints -9871 = 0xD971 as a u16)
    acc = 0
    for b in out[4:outlen]:
        v = (b ^ ((h >> 8) & 0xFF)) & 0xFF
        h = (0xFFFFFFFF & (-12691 * (v + h) + 22719))
        acc = (acc + v) & 0xFFFFFFFF
    out[0:4] = struct.pack("<I", acc)
    nblk = outlen // 16
    chain = _aes_block(key, bytes(out[0:16]))
    out[0:16] = chain
    for i in range(nblk - 1, 0, -1):
        blk = bytes(a ^ b for a, b in zip(out[16 * i:16 * i + 16], chain))
        chain = _aes_block(key, blk)
        out[16 * i:16 * i + 16] = chain
    return bytes(out)


def open_(body, key=KEY90):
    """Inverse of seal(); returns (plaintext, pad). Used by the self-tests.

    P0 = D(C0);  Pi = D(Ci) ^ C_{i+1}  (C_{n-1}'s chain is C0, as in seal()).
    """
    nblk = len(body) // 16
    assert nblk * 16 == len(body) and nblk >= 1
    out = bytearray(body)
    chain = bytes(out[0:16])
    for i in range(nblk - 1, 0, -1):
        blk = _aes_block_dec(key, bytes(out[16 * i:16 * i + 16]))
        out[16 * i:16 * i + 16] = bytes(a ^ b for a, b in zip(blk, chain))
        chain = body[16 * i:16 * i + 16]
    out[0:16] = _aes_block_dec(key, bytes(out[0:16]))
    pad = out[5] & 0xF
    return bytes(out[pad + 6:]), pad


def seal_message(msg_id, plain, key=KEY90, state0=STATE90):
    """What sub_648C61A does: seal the body, keep the id byte cleartext."""
    return bytes([msg_id]) + seal(plain, key, state0)


# ---------------------------------------------------------------------------
# capture analysis / verification
# ---------------------------------------------------------------------------

def parse_datagram(b):
    """[flags][u24 dgram] then messages: rel, u16 BE bits, msgnum u24,
    (ordering u24 + channel), (split u32 count, u16 id, u32 index)."""
    if len(b) < 4:
        return None
    out = {"flags": b[0], "dgram": b[1] | (b[2] << 8) | (b[3] << 16), "msgs": []}
    off = 4
    while off + 3 <= len(b):
        rel = b[off]
        bits = (b[off + 1] << 8) | b[off + 2]
        off += 3
        m = {"rel": rel, "bits": bits}
        if rel >> 5 >= 2:
            m["msgnum"] = b[off] | (b[off + 1] << 8) | (b[off + 2] << 16)
            off += 3
        if (rel >> 5) in (1, 3, 4):
            m["ord"] = b[off] | (b[off + 1] << 8) | (b[off + 2] << 16)
            m["chan"] = b[off + 3]
            off += 4
        if rel & 0x10:
            m["split"] = (struct.unpack(">I", b[off:off + 4])[0],
                          struct.unpack(">H", b[off + 4:off + 6])[0],
                          struct.unpack(">I", b[off + 6:off + 10])[0])
            off += 10
        dlen = (bits + 7) // 8
        if off + dlen > len(b):
            m["truncated"] = True
            break
        m["data"] = b[off:off + dlen]
        off += dlen
        out["msgs"].append(m)
    out["tail"] = b[off:]
    return out


def verify(cap_dir):
    log = open(os.path.join(cap_dir, "rkc.log"), "rb").read().decode("latin1")
    keys = {}
    for line in log.splitlines():
        if line.startswith("SEAL#"):
            parts = dict(kv.split("=") for kv in line.split()[1:])
            keys[int(parts["id"], 16)] = (bytes.fromhex(parts["key"]),
                                          int(parts["pcg"], 16), int(parts["len"]))
    print("SEAL captures:", {hex(k): (v[0], hex(v[1]), v[2]) for k, v in keys.items()})

    dgs = []
    for name in sorted(os.listdir(cap_dir)):
        if not name.startswith("dgram_"):
            continue
        p = parse_datagram(open(os.path.join(cap_dir, name), "rb").read())
        if p:
            dgs.append(p)
    dgs.sort(key=lambda d: d["dgram"])
    for d in dgs:
        for m in d["msgs"]:
            print("dgram=%2d flags=0x%02x rel=0x%02x bits=%5d msgnum=%s ord=%s split=%s first=%02x len=%d" % (
                d["dgram"], d["flags"], m["rel"], m["bits"], m.get("msgnum"),
                m.get("ord"), m.get("split"),
                m["data"][0] if m.get("data") else -1, len(m.get("data", b""))))

    # reassemble every split message: id -> {index: data}
    splits = {}
    for d in dgs:
        for m in d["msgs"]:
            if "split" in m:
                count, sid, idx = m["split"]
                splits.setdefault(sid, {})[idx] = m["data"]
    bodies = {}
    for sid, frags in splits.items():
        n = max(frags) + 1
        assert set(frags) == set(range(n)), (sid, sorted(frags))
        body = b"".join(frags[i] for i in range(n))
        mid = body[0]
        bodies[mid] = body
        print("split id=%d fragments=%d reassembled=%dB id=0x%02x" % (sid, n, len(body), mid))

    ok = True
    for mid, body in sorted(bodies.items()):
        if mid not in keys:
            print("  no SEAL capture for id 0x%02x" % mid)
            continue
        key, state0, plen = keys[mid]
        plain = None
        for cand in sorted(f for f in os.listdir(cap_dir) if f.startswith("seal_plain_")):
            data = open(os.path.join(cap_dir, cand), "rb").read()
            if len(data) == plen:
                plain = data
                break
        if plain is None:
            print("  no plaintext of size %d for 0x%02x" % (plen, mid))
            ok = False
            continue
        enc = seal(plain, key, state0)
        same = enc == body[1:]
        print("  seal(plain 0x%02x, %d B) == captured body: %s (%d vs %d)" % (
            mid, plen, same, len(enc), len(body) - 1))
        if not same:
            ok = False
            for i in range(min(len(enc), len(body) - 1)):
                if enc[i] != body[i + 1]:
                    print("    first diff at %d: %02x vs %02x" % (i, enc[i], body[i + 1]))
                    break
        back, pad = open_(enc, key)
        print("    open(seal(p)) == p : %s (pad=%d)" % (back == plain, pad))
        ok = ok and back == plain
    return ok, bodies, keys


def dump_90(plain):
    """prefix(9) + flags blob + LEB(JSON) + JSON + 24-byte tail."""
    j = plain.find(b'{"UserId"')
    start = j - 1
    while start > 0 and (plain[start - 1] & 0x80):
        start -= 1
    ln = 0
    shift = 0
    for b in plain[start:j]:
        ln |= (b & 0x7F) << shift
        shift += 7
    print("0x90: prefix=%dB flags_blob=%dB leb_len=%d json=%dB tail=%dB total=%d" % (
        start, start - 9, ln, len(plain) - j - 24, 24, len(plain)))
    print("  head: %s" % plain[:16].hex())
    print("  tail: %s" % plain[-24:].hex())
    u = struct.unpack("<6I", plain[-24:])
    print("  tail u32: %s" % [hex(x) for x in u])
    print("  signed  : %s" % [x - (1 << 32) if x >= 1 << 31 else x for x in u])
    key8a = ("%d%d%d%d" % (1818, -1733811612, u[2], 594292182))[:16]
    print("  0x8a key from tail[2]: %r  (matches capture: %s)" % (key8a, key8a == KEY8A))
    return u


def dump_8a(plain):
    """[0x8a] leb zigzag(UserId) str(ClientTicket) u32 36 str(74) str(Android)
    str(?) leb v31 leb v31-0x0BADF00D str("") str(SessionId) u32 0xC001CAFE."""
    i = 1

    def leb():
        nonlocal i
        v = 0
        shift = 0
        while True:
            b = plain[i]
            i += 1
            v |= (b & 0x7F) << shift
            shift += 7
            if not (b & 0x80):
                return v

    def s():
        nonlocal i
        n = leb()
        out = plain[i:i + n]
        i += n
        return out

    print("0x8a: id=0x%02x total=%d" % (plain[0], len(plain)))
    zz = leb()
    print("  zigzag(UserId) leb=%d -> UserId=%d" % (zz, (zz >> 1) ^ -(zz & 1)))
    t = s()
    print("  ClientTicket %dB: %s...%s" % (len(t), t[:28].decode("latin1"), t[-14:].decode("latin1")))
    print("  u32 @%d = %d" % (i, struct.unpack("<I", plain[i:i + 4])[0]))
    i += 4
    c74 = s()
    print("  const74 %dB: %s" % (len(c74), c74.hex()))
    print("  str=%r str=%r" % (s(), s()))
    v31 = leb()
    v31b = leb()
    print("  leb=%d (v31)  leb=%d (v31-0x0BADF00D)" % (v31, v31b))
    print("  str=%r" % s())
    print("  str=%r" % s())
    print("  u32 @%d = 0x%08x" % (i, struct.unpack("<I", plain[i:i + 4])[0]))
    print("  consumed %d / %d bytes" % (i + 4, len(plain)))


if __name__ == "__main__":
    cap = sys.argv[1] if len(sys.argv) > 1 else "/tmp/rkc32"
    good, bodies, keys = verify(cap)
    print("VERIFY:", "OK" if good else "FAILED")
    sys.exit(0 if good else 1)
