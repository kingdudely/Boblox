#!/usr/bin/env python3
"""Byte-diff our C++ ClientHello vs the native game ClientHello.

Usage:
    python3 tools/ch_diff.py run/cpp_tx/tx001.bin [run/native_clienthello_tx001.bin]

Inputs:
  - ours: a raw QUIC datagram dump from hs1818 --tx-dump (Initial is decrypted)
  - native: the extracted raw handshake message run/native_clienthello_tx001.bin

Masks (must differ per session/connection by design):
  - random (32B), key_share key (32B), TP initial_scid (20B),
    SNI string (each join lands on a udmux IP; structure must match).
Everything else must be byte-identical: legacy version, session_id_len,
cipher list, compression, extension count/order/bodies.
"""
import os
import struct
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from quic_initial_parse import handshake_messages  # noqa: E402 (stdlib-safe import)

EXT_NAMES = {
    0x0000: "server_name", 0x000B: "ec_point_formats", 0x000A: "supported_groups",
    0x0023: "session_ticket", 0x0010: "alpn", 0x0016: "encrypt_then_mac",
    0x0017: "extended_master_secret", 0x0014: "server_cert_type",
    0x000D: "sigalgs", 0x002B: "supported_versions", 0x002D: "psk_ke_modes",
    0x0033: "key_share", 0x0039: "quic_tp", 0xFF00: "caps", 0x001B: "cert_comp",
}


def our_stream(src):
    """Decrypted CRYPTO stream for our datagram dump.

    Prefers the frozen `<stem>.hs.bin` next to the capture (checked in —
    the stdlib-only path ctest uses). Falls back to live decrypt, which
    needs aioquic: run tools/setup.sh.
    """
    if src.endswith(".bin"):
        frozen = src[:-4] + ".hs.bin"
        if os.path.exists(frozen):
            return open(frozen, "rb").read()
    from quic_initial_parse import file_stream  # noqa: E402 (lazy: needs aioquic)
    _, hs = file_stream(src)
    return hs


def parse_ch(src, raw_msg):
    if raw_msg:
        data = open(src, "rb").read()
    else:
        hs = our_stream(src)
        data = next((d for mt, d in handshake_messages(hs) if mt == 1), None)
    if not data or data[0] != 1:
        raise SystemExit(f"no ClientHello in {src}")
    body, o = data[4:], 0
    ver = body[o:o + 2]; o += 2
    rnd = body[o:o + 32]; o += 32
    sidl = body[o]; sid = body[o + 1:o + 1 + sidl]; o += 1 + sidl
    cl = struct.unpack(">H", body[o:o + 2])[0]
    ciph = body[o + 2:o + 2 + cl]; o += 2 + cl
    compl = body[o]; o += 1 + compl
    ext_total = struct.unpack(">H", body[o:o + 2])[0]; o += 2
    end = o + ext_total
    exts = []
    while o < end:
        et, el = struct.unpack(">HH", body[o:o + 4]); o += 4
        exts.append((et, body[o:o + el])); o += el
    return dict(ver=ver.hex(), sidl=sidl, ciphers=[ciph[i:i + 2].hex()
                for i in range(0, len(ciph), 2)], compl=compl,
                total=len(data), exts=exts)


def masked(ch):
    b = bytearray()
    b += bytes.fromhex(ch["ver"])
    b += b"\x00" * 32                      # random
    b.append(ch["sidl"]); b += b"\x00" * ch["sidl"]
    b += struct.pack(">H", len(ch["ciphers"]) * 2)
    b += bytes.fromhex("".join(ch["ciphers"]))
    b.append(ch["compl"])
    parts = []
    for e, ed in ch["exts"]:
        if e == 0x0033:                    # key_share: mask ephemeral key
            ed = ed[:4] + b"\x00" * 32
        elif e == 0x0039:                  # quic_tp: mask initial_scid (22B hdr+id)
            ed = ed[:2] + b"\x00" * 20 + ed[22:]
        elif e == 0x0000:                  # server_name: canonicalize (udmux
            # IP differs per join; structure checked separately)
            ed = b"\x00\x0f\x00\x00\x00\x0c" + b"\x00" * 12
        parts.append(struct.pack(">HH", e, len(ed)) + ed)
    b += struct.pack(">H", sum(map(len, parts)))
    b += b"".join(parts)
    return bytes(b)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    ours = parse_ch(sys.argv[1], False)
    nat = parse_ch(sys.argv[2], True) if len(sys.argv) > 2 else parse_ch(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                     "run", "native_clienthello_tx001.bin"), True)

    ok = True
    for key in ("ver", "sidl", "ciphers", "compl", "total"):
        a, b = nat[key], ours[key]
        status = "OK" if a == b else "DIFF"
        # total may differ if the SNI string length differs (udmux IP)
        if key == "total" and a != b:
            sni_n = dict(nat["exts"])[0x0000]
            sni_o = dict(ours["exts"]).get(0x0000, b"")
            if b == a - len(sni_n) + len(sni_o):
                status = "OK(sni-ip-len)"
        ok &= status.startswith("OK")
        print(f"  {key:8s} native={a} ours={b} {status}")
    order_n = [e for e, _ in nat["exts"]]
    order_o = [e for e, _ in ours["exts"]]
    print(f"  ext_order {'OK' if order_n == order_o else 'DIFF'}")
    print(f"           {' '.join(hex(e) for e in order_n)}")
    ok &= order_n == order_o
    for e, ed in nat["exts"]:
        od = dict(ours["exts"]).get(e)
        if e in (0x0033, 0x0039, 0x0000):
            continue                        # masked above
        if od != ed:
            ok = False
            print(f"  DIFF {EXT_NAMES.get(e, hex(e))}: native {ed.hex()}"
                  f" ours {od.hex() if od is not None else 'MISSING'}")
    # SNI structure: list_len == 3+name_len, type 0, ASCII IP literal
    for label, ch in (("native", nat), ("ours", ours)):
        sni = dict(ch["exts"]).get(0x0000, b"")
        ln = struct.unpack(">H", sni[3:5])[0]
        struct_ok = (len(sni) >= 5 and struct.unpack(">H", sni[0:2])[0]
                     == len(sni) - 2 and sni[2:4] == b"\x00\x00"
                     and struct.unpack(">H", sni[3:5])[0] == len(sni) - 5
                     and all(48 <= c <= 57 or c == ord(".") for c in sni[5:]))
        print(f"  sni_structure_{label}={'OK' if struct_ok else 'DIFF'} "
              f"({sni[5:].decode(errors='replace')})")
        ok &= struct_ok
    bm, cm = masked(nat), masked(ours)
    exact = bm == cm
    ok &= exact
    print(f"  masked_byte_exact={'OK' if exact else 'DIFF'} "
          f"(native={len(bm)}B ours={len(cm)}B)")
    print("CH MATCH" if ok else "CH MISMATCH")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
