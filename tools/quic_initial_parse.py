#!/usr/bin/env python3
"""quic_initial_parse.py — decrypt QUIC v1 Initial datagrams captured from the
NATIVE client (run/quicdump/udp/tx*.bin) and dump the TLS ClientHello.

The Initial packet's keys derive purely from the DCID in the cleartext header
(public salt), so no secrets are needed. This yields the exact TLS fingerprint
(cipher suites, extension order, ALPN, custom ext 0xFF00 payload) the server
sees from the real client.

Usage: python3 tools/quic_initial_parse.py [tx.bin ...]   (default: run/quicdump/udp)
"""
import glob
import os
import struct
import sys

# decrypted Initial packet numbers, used as `expected_packet_number` hints when
# decoding retransmissions that use a shortened (truncated) packet number field
PN_HINTS = []
# all CIDs seen in cleartext headers. After a Retry the client switches the
# DCID field to server-provided CIDs while KEEPING initial keys derived from
# the post-Retry CID — so decrypting a packet may need a previously seen CID.
SEEN_CIDS = []

# NOTE: aioquic is imported lazily inside decrypt_initial (below), so importing
# this module — and using its pure parsers (parse_frames, handshake_messages,
# parse_clienthello) — needs only the stdlib. Only live decryption needs pip.


def parse_varint(buf: bytes, off: int):
    first = buf[off]
    ln = 1 << (first >> 6)
    val = first & 0x3F
    if ln > 1:
        val = (val << 8 * (ln - 1)) | int.from_bytes(buf[off + 1:off + ln], "big")
    return val, ln


def decrypt_initial(datagram: bytes):
    """Yield dict(dcid, scid, pn, ptype, data) per QUIC packet in datagram.

    Handles coalesced packets via QuicHeader.packet_length. We decrypt the
    client's packets, so we set up keys as the server side.

    NOTE: imported lazily so this module stays stdlib-safe; only live
    decryption needs aioquic (tests use frozen .hs.bin streams instead).
    """
    try:
        from aioquic.buffer import Buffer
        from aioquic.quic.crypto import CryptoPair
        from aioquic.quic.packet import pull_quic_header, QuicPacketType
    except ModuleNotFoundError:
        raise SystemExit(
            "decrypting captures needs aioquic — run tools/setup.sh "
            "(tests use frozen .hs.bin streams and never need it)")
    off = 0
    while off < len(datagram) - 20:
        buf = Buffer(data=datagram[off:])
        try:
            hdr = pull_quic_header(buf)
        except Exception:
            return
        if hdr.packet_type in (QuicPacketType.RETRY, QuicPacketType.VERSION_NEGOTIATION):
            return
        encrypted_offset = buf.tell()  # packet number starts here
        # record CIDs for future candidate key sets (see SEEN_CIDS docstring)
        for c in (hdr.destination_cid, hdr.source_cid):
            if c and c not in SEEN_CIDS:
                SEEN_CIDS.append(c)
        if hdr.packet_type == QuicPacketType.INITIAL:
            header = payload = None
            pn = None
            pkt = datagram[off:off + hdr.packet_length]  # exact packet: coalesced
            # Initial keys: normally the header DCID; but after the client
            # adopts a server-provided DCID the keys stay on the post-Retry
            # CID, so also try every CID seen so far. The Initial pn is a
            # RANDOM 31-bit value (settings.initial_pkt_num): first packets
            # decode with expected=0 but shortened-pn retransmissions need an
            # expected value near the true pn.
            cands = [hdr.destination_cid] + [c for c in SEEN_CIDS
                                             if c != hdr.destination_cid]
            for cid in cands:
                pair = CryptoPair()
                pair.setup_initial(cid, is_client=False, version=hdr.version)
                for exp in (0, *sorted(PN_HINTS)):
                    try:
                        header, payload, pn = pair.decrypt_packet(
                            pkt, encrypted_offset, expected_packet_number=exp)
                        break
                    except Exception:
                        continue
                if payload is not None:
                    break
            if payload is None:
                raise RuntimeError(
                    f"Initial decrypt failed (dcid={hdr.destination_cid.hex()} "
                    f"cands={len(cands)} hints={PN_HINTS})")
            if pn not in PN_HINTS:
                PN_HINTS.append(pn)
            yield dict(dcid=hdr.destination_cid, scid=hdr.source_cid, pn=pn,
                       ptype=hdr.packet_type, data=payload,
                       first=header[0] if header else datagram[off])
        else:
            # 0-RTT / Handshake need handshake keys — report presence only
            yield dict(dcid=hdr.destination_cid, scid=hdr.source_cid, pn=None,
                       ptype=hdr.packet_type, data=b"", first=datagram[off])
        off += hdr.packet_length  # next coalesced packet


def parse_frames(data: bytes):
    off = 0
    crypto = []  # (offset, bytes) — QUIC CRYPTO frames may arrive out of order
    frames = []
    while off < len(data):
        t = data[off]; off += 1
        if t == 0x06:  # CRYPTO
            o, n = parse_varint(data, off); off += n
            l, n = parse_varint(data, off); off += n
            crypto.append((o, data[off:off + l])); off += l
            frames.append(("CRYPTO", o, l))
        elif t == 0x00:
            frames.append(("PADDING",))
        elif t == 0x01:
            frames.append(("PING",))
        elif t == 0x02 or t == 0x03:  # ACK
            largest, n = parse_varint(data, off); off += n
            delay, n = parse_varint(data, off); off += n
            range_cnt, n = parse_varint(data, off); off += n
            for _ in range(range_cnt + 1):
                _, n = parse_varint(data, off); off += n
                _, n = parse_varint(data, off); off += n
            if t == 0x03:
                ecn_cnt, n = parse_varint(data, off); off += n
                off += 3 * ecn_cnt
            frames.append(("ACK", largest))
        elif t == 0x04:  # RESET_STREAM
            _, n = parse_varint(data, off); off += n
            _, n = parse_varint(data, off); off += n
            _, n = parse_varint(data, off); off += n
            frames.append(("RESET_STREAM",))
        elif t == 0x05:  # STOP_SENDING
            _, n = parse_varint(data, off); off += n
            _, n = parse_varint(data, off); off += n
            frames.append(("STOP_SENDING",))
        elif t in (0x08, 0x09):  # STREAM (off len bits)
            if t & 0x01:
                _, n = parse_varint(data, off); off += n
            if t & 0x02:
                _, n = parse_varint(data, off); off += n
            l, n = parse_varint(data, off); off += n
            off += l
            frames.append(("STREAM", t, l))
        elif t == 0x0A or t == 0x0B:  # MAX_DATA / MAX_STREAM_DATA
            _, n = parse_varint(data, off); off += n
            frames.append(("MAX",))
        elif t in (0x0C, 0x0D, 0x0E):  # MAX_STREAMS
            _, n = parse_varint(data, off); off += n
            frames.append(("MAX_STREAMS",))
        elif t in (0x0F, 0x10):  # DATA_BLOCKED / STREAM_DATA_BLOCKED
            _, n = parse_varint(data, off); off += n
            frames.append(("BLOCKED",))
        elif t in (0x11, 0x12):  # STREAMS_BLOCKED
            _, n = parse_varint(data, off); off += n
            frames.append(("STREAMS_BLOCKED",))
        elif t == 0x13:  # NEW_CONNECTION_ID
            _, n = parse_varint(data, off); off += n
            off += 1 + 16  # seq + 16B token... (cid len byte precedes)
            frames.append(("NEW_CID?",))
            break  # unusual in handshake; bail safely
        elif t == 0x14 or t == 0x15 or t == 0x16:
            frames.append(("RETIRE/PATH/CONN_CLOSE",))
            break
        elif t == 0x1C:  # CONNECTION_CLOSE (transport)
            _, n = parse_varint(data, off); off += n
            _, n = parse_varint(data, off); off += n
            frames.append(("CLOSE",))
            break
        elif t == 0x1D:  # CONNECTION_CLOSE (app)
            _, n = parse_varint(data, off); off += n
            _, n = parse_varint(data, off); off += n
            _, n = parse_varint(data, off); off += n
            frames.append(("CLOSE_APP",))
            break
        elif t == 0x1E:  # HANDSHAKE_DONE
            frames.append(("HANDSHAKE_DONE",))
        else:
            frames.append(("UNK", hex(t)))
            break
    # reassemble the CRYPTO stream by offset (frames may overlap/repeat)
    stream = {}
    for o, chunk in crypto:
        for i, b in enumerate(chunk):
            stream[o + i] = b
    return frames, stream


def assemble(stream):
    """Merge {offset: byte} fragments; holes become 0x00."""
    if not stream:
        return b""
    return bytes(stream.get(i, 0) for i in range(max(stream) + 1))


def file_stream(path):
    """Decrypt one capture file; return (packet_summaries, CRYPTO stream bytes).

    Handles GSO batches (multiple of 1200 bytes) by splitting into datagrams.
    """
    raw = open(path, "rb").read()
    if len(raw) % 1200 == 0 and len(raw) > 1200:
        chunks = [raw[i:i + 1200] for i in range(0, len(raw), 1200)]
    else:
        chunks = [raw]
    stream = {}
    summ = []
    for ci, dg in enumerate(chunks):
        if not (dg[0] & 0x80) or dg[1:5] != b"\x00\x00\x00\x01":
            summ.append(f"[{ci}] not QUIC v1 long header ({dg[0]:02x})")
            continue
        for pkt in decrypt_initial(dg):
            try:
                frames, sd = parse_frames(pkt["data"])
            except Exception as e:
                # tiny ACK-only packets can end mid-frame — not fatal
                frames, sd = [(f"TRUNC:{type(e).__name__}",)], {}
            kinds = [f[0] for f in frames]
            s = []
            for k in kinds:
                if not s or s[-1][0] != k:
                    s.append([k, 1])
                else:
                    s[-1][1] += 1
            summ.append(
                f"{pkt['ptype'].name} pn={pkt['pn']} " +
                " ".join(f"{k}x{n}" if n > 1 else k for k, n in s))
            for o, b in sd.items():
                stream.setdefault(o, b)
    if stream:
        hi = max(stream)
        gaps = [i for i in range(hi + 1) if i not in stream]
        if gaps:
            summ.append(f"CRYPTO-HOLES {len(gaps)}B first_at={gaps[0]}")
    return summ, assemble(stream)


def handshake_messages(hs: bytes):
    """Split a CRYPTO stream into raw TLS handshake messages.

    QUIC carries RAW handshake messages (no record layer), but tolerate a
    record-wrapped stream (type 0x16) too.
    """
    msgs = bytearray()
    if hs and hs[0] == 0x16:
        o = 0
        while o + 5 <= len(hs):
            ct = hs[o]
            rl = struct.unpack_from("!H", hs, o + 3)[0]
            if ct == 22:
                msgs += hs[o + 5:o + 5 + rl]
            o += 5 + rl
    else:
        msgs = bytearray(hs)
    out = []
    ho = 0
    while ho + 4 <= len(msgs):
        mt = msgs[ho]
        ml = int.from_bytes(msgs[ho + 1:ho + 4], "big")
        out.append((mt, bytes(msgs[ho:ho + 4 + ml])))
        ho += 4 + ml
    return out


TLS_EXT_NAMES = {
    0x0000: "server_name", 0x0001: "max_fragment_length", 0x0005: "status_request",
    0x000A: "supported_groups", 0x000B: "ec_point_formats", 0x000D: "signature_algorithms",
    0x0010: "ALPN", 0x0015: "padding", 0x0017: "extended_master_secret",
    0x001B: "compress_certificate", 0x001C: "record_size_limit",
    0x0022: "encrypt_then_mac", 0x0023: "extended_master_secret?",
    0x0029: "pre_shared_key", 0x002A: "early_data", 0x002B: "supported_versions",
    0x002D: "psk_key_exchange_modes", 0x0030: "signature_algorithms_cert",
    0x0031: "key_share", 0x0033: "renegotiation_info",
    0x0039: "quic_transport_parameters", 0x0036: "quic_early_data?",
    0x3374: "next_protocols", 0xFE0D: "encrypted_client_hello",
    0xFF00: "RBX_CUSTOM_0xFF00", 0x0A0A: "grease",
}


def parse_clienthello(ch: bytes):
    """ch = full handshake message (type byte + 3-byte len included)."""
    assert ch[0] == 1, f"not a ClientHello: {ch[0]}"
    ln = int.from_bytes(ch[1:4], "big")
    body = ch[4:4 + ln]
    o = 0
    legacy_ver = body[o:o+2]; o += 2
    random = body[o:o+32]; o += 32
    sid_len = body[o]; o += 1
    sid = body[o:o+sid_len]; o += sid_len
    cs_len = struct.unpack_from("!H", body, o)[0]; o += 2
    ciphers = [struct.unpack_from("!H", body, o + i)[0] for i in range(0, cs_len, 2)]; o += cs_len
    comp_len = body[o]; o += 1
    o += comp_len
    ext_total = struct.unpack_from("!H", body, o)[0]; o += 2
    ext_end = o + ext_total
    exts = []
    while o < ext_end:
        et = struct.unpack_from("!H", body, o)[0]; o += 2
        el = struct.unpack_from("!H", body, o)[0]; o += 2
        ed = body[o:o+el]; o += el
        exts.append((et, ed))
    return dict(legacy_ver=legacy_ver, random=random, sid_len=sid_len,
                ciphers=ciphers, exts=exts)


def describe_ext(et, ed, out):
    name = TLS_EXT_NAMES.get(et, f"ext_0x{et:04x}")
    extra = ""
    if et == 0x0010:  # ALPN
        pl = []
        i = 2
        while i < len(ed):
            l = ed[i]; i += 1
            pl.append(ed[i:i+l].decode(errors="replace")); i += l
        extra = " protocols=" + ",".join(pl)
    elif et == 0x002B:  # supported_versions
        i = 1
        vs = []
        while i < len(ed):
            vs.append(ed[i:i+2].hex()); i += 2
        extra = " versions=" + ",".join(vs)
    elif et == 0x0031:  # key_share
        i = 2
        ks = []
        while i < len(ed):
            g = struct.unpack_from("!H", ed, i)[0]; i += 2
            l = struct.unpack_from("!H", ed, i)[0]; i += 2
            ks.append(f"group=0x{g:04x}({l}B)"); i += l
        extra = " " + " ".join(ks)
    elif et == 0x000A:  # supported_groups
        i = 2
        gs = []
        while i < len(ed):
            g = struct.unpack_from("!H", ed, i)[0]; i += 2
            gs.append(f"0x{g:04x}")
        extra = " groups=" + ",".join(gs)
    elif et == 0x0000:  # SNI
        extra = " " + repr(ed[5:]) if len(ed) > 5 else " <empty>"
    elif et == 0x0039:  # quic TP
        extra = f" ({len(ed)}B)"
    elif et == 0xFF00:
        extra = f" payload={ed.hex()}"
    out.append(f"    0x{et:04x} {name:32s} len={len(ed):4d}{extra}")


def main():
    files = sys.argv[1:] or sorted(glob.glob(
        os.path.join(os.path.dirname(__file__), "..", "run", "quicdump", "udp", "tx*.bin")))
    if not files:
        print("no captured datagrams found")
        return
    seen_ch = set()
    for fn in files:
        try:
            summ, hs = file_stream(fn)
        except Exception as e:
            print(f"{os.path.basename(fn)}: decrypt/parse failed: {type(e).__name__}: {e}")
            continue
        if not hs:
            if summ:
                print(f"== {os.path.basename(fn)}: " + " | ".join(summ) + " (no CRYPTO)")
            continue
        print(f"== {os.path.basename(fn)}: " + " | ".join(summ))
        for mt, msg in handshake_messages(hs):
            if mt == 1:
                key = msg[4:64]
                if key in seen_ch:
                    print("    (ClientHello identical to previous — abbreviating)")
                else:
                    seen_ch.add(key)
                    ch = parse_clienthello(msg)
                    print(f"    ClientHello legacy_ver={ch['legacy_ver'].hex()} "
                          f"sid_len={ch['sid_len']} random={ch['random'].hex()}")
                    print(f"    cipher_suites ({len(ch['ciphers'])}): " +
                          ",".join(f"0x{c:04x}" for c in ch["ciphers"]))
                    print(f"    extensions ({len(ch['exts'])}):")
                    for et, ed in ch["exts"]:
                        lines = []
                        describe_ext(et, ed, lines)
                        for l in lines:
                            print(l)
            else:
                print(f"    handshake msg type={mt} len={len(msg) - 4}")


if __name__ == "__main__":
    main()
