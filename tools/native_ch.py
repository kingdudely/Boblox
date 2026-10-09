#!/usr/bin/env python3
"""native_ch.py — extract the byte-exact native ClientHello from a captured
game Initial and decode its quic_transport_parameters extension.

Outputs:
  run/native_clienthello_<name>.bin   raw handshake message (type+len+body)
  stdout: hex dump, per-extension raw hex, TP param decode (cross-checked
          against the gdb runtime dump run/quicdump_settings/params0.bin)

Usage: python3 tools/native_ch.py [tx.bin ...]   (default: run/quicdump/udp/tx001.bin)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util

spec = importlib.util.spec_from_file_location(
    "qip", os.path.join(os.path.dirname(os.path.abspath(__file__)), "quic_initial_parse.py"))
q = importlib.util.module_from_spec(spec)
spec.loader.exec_module(q)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# RFC 9000 §18 + RFC 9297/9368 transport parameter ids
# (cross-checked against aioquic.quic.packet.PARAMS)
TP_IDS = {
    0x00: "original_destination_connection_id",
    0x01: "max_idle_timeout",
    0x02: "stateless_reset_token",
    0x03: "max_udp_payload_size",
    0x04: "initial_max_data",
    0x05: "initial_max_stream_data_bidi_local",
    0x06: "initial_max_stream_data_bidi_remote",
    0x07: "initial_max_stream_data_uni",
    0x08: "initial_max_streams_bidi",
    0x09: "initial_max_streams_uni",
    0x0A: "ack_delay_exponent",
    0x0B: "max_ack_delay",
    0x0C: "disable_active_migration",
    0x0D: "preferred_address",
    0x0E: "active_connection_id_limit",
    0x0F: "initial_source_connection_id",
    0x10: "retry_source_connection_id",
    0x11: "version_information",
    0x20: "max_datagram_frame_size",
    0x2A97: "grease_quic_bit",
}


def varint(b, o):
    first = b[o]
    n = 1 << (first >> 6)
    v = first & 0x3F
    if n > 1:
        v = (v << 8 * (n - 1)) | int.from_bytes(b[o + 1:o + n], "big")
    return v, n


def decode_tp(ed):
    """Decode QUIC v1 transport params. Scalar values are QUIC varints."""
    out = []
    o = 0
    while o < len(ed):
        pid, n = varint(ed, o); o += n
        ln, n = varint(ed, o); o += n
        val = ed[o:o + ln]; o += ln
        name = TP_IDS.get(pid, f"tp_0x{pid:x}")
        if pid in (0x00, 0x02, 0x0F, 0x10, 0x0D):
            # opaque bytes: CIDs, stateless reset token, preferred address
            desc = val.hex()
        elif pid == 0x11 and len(val) >= 4:
            chosen = int.from_bytes(val[:4], "big")
            avail = [hex(int.from_bytes(val[i:i + 4], "big"))
                     for i in range(4, len(val), 4)]
            desc = f"chosen=0x{chosen:x} available={avail}"
        elif ln:
            v, _ = varint(val, 0)
            if pid == 0x01:
                desc = f"{v} ms = {v / 1000:g} s"
            elif pid == 0x0B:
                desc = f"{v} ms"
            else:
                desc = str(v)
        else:
            desc = ""
        out.append((pid, name, ln, desc, val.hex()))
    return out


def hexdump(b, base=0, width=16):
    lines = []
    for i in range(0, len(b), width):
        chunk = b[i:i + width]
        hx = " ".join(f"{x:02x}" for x in chunk)
        pr = "".join(chr(x) if 32 <= x < 127 else "." for x in chunk)
        lines.append(f"  {base + i:04x}  {hx:<48}  {pr}")
    return "\n".join(lines)


def main():
    files = sys.argv[1:] or [os.path.join(ROOT, "run", "quicdump", "udp", "tx001.bin")]
    done = set()
    for fn in files:
        summ, hs = q.file_stream(fn)
        print(f"== {os.path.basename(fn)}: " + " | ".join(summ))
        if not hs:
            continue
        for mt, msg in q.handshake_messages(hs):
            if mt != 1:
                print(f"  handshake msg type={mt} len={len(msg) - 4}")
                continue
            key = msg[4:64]
            if key in done:
                print("  (ClientHello identical to previous)")
                continue
            done.add(key)
            base = os.path.splitext(os.path.basename(fn))[0]
            out = os.path.join(ROOT, "run", f"native_clienthello_{base}.bin")
            open(out, "wb").write(msg)
            print(f"  saved {out} ({len(msg)} bytes)")
            ch = q.parse_clienthello(msg)
            body = msg[4:]
            print(f"  cipher_suites ({len(ch['ciphers'])}): " +
                  ",".join(f"0x{c:04x}" for c in ch["ciphers"]))
            # walk extensions from the raw body for exact byte offsets
            o = 2 + 32
            sidl = body[o]; o += 1 + sidl
            csl = int.from_bytes(body[o:o + 2], "big"); o += 2 + csl
            compl = body[o]; o += 1 + compl
            ext_total = int.from_bytes(body[o:o + 2], "big"); o += 2
            ext_end = o + ext_total
            idx = 0
            print(f"  extensions block: {ext_total} bytes at body+{o:#x}")
            while o < ext_end:
                et = int.from_bytes(body[o:o + 2], "big")
                el = int.from_bytes(body[o + 2:o + 4], "big")
                ed = body[o + 4:o + 4 + el]
                name = q.TLS_EXT_NAMES.get(et, f"ext_0x{et:04x}")
                print(f"   [{idx:2d}] off=+{o:#06x} type=0x{et:04x} {name:30s} len={el}")
                print(f"        {ed.hex()}")
                if et == 0x0039:
                    print("        -- QUIC transport parameters --")
                    for pid, nm, ln, desc, hx in decode_tp(ed):
                        print(f"           {pid:#06x} {nm:36s} len={ln:<3} = {desc}")
                o += 4 + el
                idx += 1
            print("  full ClientHello hex:")
            print(hexdump(msg))


if __name__ == "__main__":
    main()
