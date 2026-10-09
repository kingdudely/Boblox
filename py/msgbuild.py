#!/usr/bin/env python3
"""msgbuild.py — dependency-free wire message builders (the functional core).

Pure byte builders for the session handshake messages (earlyauth, 0x8A,
0x90, 0x92) plus the varint/hash primitives they need. STDLIB ONLY — this
module must never import third-party packages (or sibling probe modules):
it is what the test battery (message-parity, vectors-py) and any fresh
clone without `aioquic` import. Live-network code lives in probe5/probe7/
rbx_client, which re-export these names for backward compatibility.

Contents are verbatim copies of the originals (probe5.py, probe7.py,
rbx_client.py); behavior is pinned byte-for-byte by tools/msg_parity.py
against the C++ builders.
"""
import base64
import json
import os
import random
import struct

CAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "captures")

M = 0xFFFFFFFF
M64 = (1 << 64) - 1
V9 = 0x63E25F26


# ---------------------------------------------------------------------------
# varint helpers
# ---------------------------------------------------------------------------

def compact_varint(v):
    """Wire framing varint (top-2-bit size scheme)."""
    if v < 0x40:
        return bytes([v])
    if v < 0x4000:
        return struct.pack('>H', 0x4000 | v)
    if v < 0x40000000:
        return struct.pack('>I', 0x80000000 | v)
    return struct.pack('>Q', 0xC000000000000000 | v)


def leb128(v):
    """Inner-field varint (continuation-bit scheme)."""
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            break
    return bytes(out)


def frame(payload):
    return compact_varint(len(payload)) + payload


def stream_header(app, chan):
    return bytes([0x06, 0x01, app]) + struct.pack('>I', chan)


# ---------------------------------------------------------------------------
# 0x8A hash (validated against two native captures)
# ---------------------------------------------------------------------------

S = 0x71375635  # dword_71941A0 runtime value


def _rol32(x, r):
    return ((x << r) | (x >> (32 - r))) & M


def xxh32(data, seed=0):
    P1, P2, P3, P4, P5 = 2654435761, 2246822519, 3266489917, 668265263, 374761393
    n = len(data)
    i = 0
    if n >= 16:
        v1, v2, v3, v4 = (seed + P1 + P2) & M, (seed + P2) & M, seed & M, (seed - P1) & M
        while i <= n - 16:
            vals = [v1, v2, v3, v4]
            for vi in range(4):
                lane = int.from_bytes(data[i:i + 4], 'little')
                i += 4
                vals[vi] = (_rol32((vals[vi] + lane * P2) & M, 13) * P1) & M
            v1, v2, v3, v4 = vals
        h = (_rol32(v1, 1) + _rol32(v2, 7) + _rol32(v3, 12) + _rol32(v4, 18)) & M
    else:
        h = (seed + P5) & M
    h = (h + n) & M
    while i <= n - 4:
        h = (_rol32((h + int.from_bytes(data[i:i + 4], 'little') * P3) & M, 17) * P4) & M
        i += 4
    while i < n:
        h = (_rol32((h + data[i] * P5) & M, 11) * P1) & M
        i += 1
    h ^= h >> 15
    h = (h * P2) & M
    h ^= h >> 13
    h = (h * P3) & M
    h ^= h >> 16
    return h


def ticket_v31(ticket: bytes) -> int:
    v7 = xxh32(ticket, 1)
    v8 = ((-17506 * S) & M) & 0xFFFF
    v9 = 7 if (v8 & 2) else 25
    v12 = (-S) & M if (v8 & 4) else (-1434170839) & M
    v13 = (_rol32((v7 + 1434170839) & M, v9) + v12) & M
    v14 = S if (v8 & 8) else 1434170839
    v15 = S if (v8 & 0x20) else 1434170839
    v16 = S if (v8 & 0x40) else 1434170839
    v17 = S if (v8 & 0x4000) else 1434170839
    v18 = (v13 * v14) & M
    v19 = 13 if (v8 & 0x10) else 19
    v20 = _rol32((v16 ^ ((v15 - _rol32(v18, v19)) & M)) & M,
                 (2 * ((v8 & 0xFF) >> 7)) + 15)
    v11 = S if (v8 & 0x100) else (-S) & M
    v21 = (v11 + 1434170839) & M
    v22 = (v11 - 1434170839) & M
    if v8 & 0x200:
        v22 = v21
    v23 = (v20 + v22) & M
    v24 = 23 if (v8 & 0x400) else 9
    v25 = _rol32(v23, v24)
    v26 = (-v25) & M if (v8 & 0x800) else v25
    v27 = (S + v26) & M
    v28 = (-v27) & M if (v8 & 0x1000) else v27
    v29 = (v28 + 1434170839) & M
    v30 = 29 if (v8 & 0x2000) else 3
    v31 = (v17 ^ _rol32(v29, v30)) & M
    return v31


CONST74 = (b"2e427f51c4dab762fe9e3471c6cfa1650841723b"
           b"!"
           b"6e8e47e92778f00efbb13c7bb151ea88.")


def build_8a(js):
    uid = int(js["UserId"])
    ticket = js["ClientTicket"].encode()
    session_json = js["SessionId"].encode()
    zz = (2 * uid) ^ (uid >> 63)
    v31 = ticket_v31(ticket)
    out = bytearray()
    out.append(0x8A)
    out += leb128(zz)
    out += leb128(len(ticket)) + ticket
    out += struct.pack('<I', 36)
    assert len(CONST74) == 74, len(CONST74)
    out += leb128(74) + CONST74
    out += leb128(7) + b"Android"
    out += leb128(1) + b"?"
    out += leb128(v31)
    out += leb128((v31 - 0x0BADF00D) & M)
    out += b"\x00"
    out += leb128(len(session_json)) + session_json
    out += b"\xfe\xca\x01\xc0"
    return bytes(out)


def load_cap(name):
    return open(f"{CAP}/{name}", "rb").read()


TEMPLATES = ("acct2_90.bin", "msg_0007_a4_c1.bin")   # acct2 (2655886518), acct1 (4656429295)


def _load_90_template(js):
    """Pick the 0x90 template whose identity fields match this session's UserId."""
    env = os.environ.get("RBX_90_TEMPLATE")
    names = (env,) if env else TEMPLATES
    want = int(js.get("UserId", 0) or 0)
    fallback = None
    for name in names:
        try:
            raw = load_cap(name)
        except Exception:
            continue
        i = raw.find(b'{"UserId"')
        if i < 0:
            continue
        try:
            tmpl = json.loads(raw[i:len(raw) - 20])
        except Exception:
            continue
        if fallback is None:
            fallback = (name, raw, i, tmpl)
        if int(tmpl.get("UserId", 0) or 0) == want:
            return name, raw, i, tmpl
    if fallback is None:
        raise RuntimeError("no 0x90 template found")
    return fallback


def build_90(js, reply):
    # Template selection: env RBX_90_TEMPLATE, else auto-match by UserId.
    # Templates are REAL 0x90 captures from native sessions on each account, so
    # all identity fields (UserId/UserName/AccountAge/DomainUserId/...) are set.
    tname, raw, i, tmpl = _load_90_template(js)
    i = raw.find(b'{"UserId"')
    # find start of the LEB length byte(s) preceding the JSON
    start = i - 1
    while start > 0 and (raw[start - 1] & 0x80):
        start -= 1
    prefix = raw[:start]  # includes 0x90 + flags list
    tail = raw[-20:]      # 5 x u32 trailer (contains the v9 constant pair) — REQUIRED
    tmpl = json.loads(raw[i:len(raw) - 20])
    ticket_inner = {
        "SerializedClientFields": reply["joinTicket"]["SerializedClientFields"],
        "EncryptedServerFields": reply["joinTicket"]["EncryptedServerFields"],
    }
    tmpl["RandomSeed1"] = js["RandomSeed1"]
    tmpl["APIsecurityToken"] = js["APIsecurityToken"]
    tmpl["__joinTicket"] = json.dumps(ticket_inner, separators=(",", ":"))
    body = json.dumps(tmpl, separators=(",", ":")).encode()
    out = bytearray(prefix)
    out += leb128(len(body))
    out += body
    out += tail
    return bytes(out)


def build_92():
    v8 = random.getrandbits(32)
    x = ((v8 << 32) | (v8 ^ V9)) & M64
    zz = ((x << 1) & M64) ^ (M64 if (v8 & 0x80000000) else 0)
    return bytes([0x92]) + leb128(zz)


def early_auth_payload(client_ticket, version):
    parts = client_ticket.split(";")

    def dec(s):
        if not s:
            return b""
        pad = "=" * (-len(s) % 4)
        try:
            return base64.b64decode(s + pad)
        except Exception:
            return s.encode()
    pre = dec(parts[2]) if len(parts) > 2 else b""
    auth = dec(parts[3]) if len(parts) > 3 else b""
    return bytes([0xA8, version & 0xFF, len(pre) & 0xFF]) + pre + bytes([len(auth) & 0xFF]) + auth
