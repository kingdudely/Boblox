#!/usr/bin/env python3
"""RbxTransport probe v5 — native join sequence with rebuilt 0x8A.

Based on byte-exact native capture (mocktail 2.739.691, libroblox.so
md5 892d0d3b04222497b4e69c2bc145ec6c) in py/captures:

  ctrl stream (app=4 chan=0x6374726C):
      00 00                        (2B unknown ctrl hello)
      OpenUnreliable 3  <- wire 2
      OpenUnreliable 5  <- wire 4
      OpenUnreliable 6  <- wire 6
      OpenUnreliable 11 <- wire 8
  chan 1 stream (app=4 chan=1):
      a8.. early auth (103B)
      a7 06 00 + 6x16B    (queue replay from capture)
      90.. ~10.7KB        (client info; replay from capture)
      92.. 11B            (replay)
      8a.. REBUILT        (ticket + session JSON + v31 hash + 0xC001CAFE)
      8f 00               (replay)
      9b.. 9B             (replay, 1.5s later)

The 0x8A layout (verified on two captures):
  [0x8A]
  LEB(zigzag64(UserId))
  LEB(len(ClientTicket)) + ClientTicket
  u32le(36)
  LEB(74) + "2e427f51c4dab762fe9e3471c6cfa1650841723b!6e8e47e92778f00efbb13c7bb151ea88."
  LEB(7) + "Android"
  LEB(1) + "?"
  LEB(v31) + LEB(v31 - 0x0BADF00D)
  0x00                                  (empty transformed static string)
  LEB(len(SessionJson)) + SessionJson   (joinScript["SessionId"])
  0xC001CAFE as 4 raw LE bytes          (fe ca 01 c0)

v31 = obfuscated(xxh32(ClientTicket, seed=1)) with compile-time constants
(dword_71941A0 = 0x71375635). Implemented below and validated against captures.

Usage:
  probe5.py --cookie-file cookie.txt [--place 1818] [--seconds 25]
"""
import argparse
import asyncio
import base64
import json
import os
import socket
import ssl
import struct
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rbx_client import join_game, early_auth_payload, RuppTransport, decode_uri

from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.connection import QuicConnection
from aioquic.quic.events import (
    HandshakeCompleted, ProtocolNegotiated,
    StreamDataReceived, StreamReset, DatagramFrameReceived, ConnectionTerminated,
)

APP = 4
CTRL = 0x6374726C
import os
CAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "captures")
M = 0xFFFFFFFF

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


# ---------------------------------------------------------------------------
# client
# ---------------------------------------------------------------------------

class Probe:
    def __init__(self, js, log=print):
        self.js = js
        self.log = log
        self.cfg = json.loads(decode_uri(js["NetStackConfig"]))
        self.udmux_ip = js["UdmuxEndpoints"][0]["Address"]
        self.rcc_ip = js["ServerConnections"][0]["Address"]
        self.port = int(js["NetStackPort"])
        self.token = base64.b64decode(js["NetStackTokenValue"])
        self.conn = None
        self.live = False
        self.term = False
        self.streams = {}
        self.server_wires = {}
        self.recv_chan = {}
        self.got_game_data = False
        self.sent_9b = False

    def events(self):
        while True:
            ev = self.conn.next_event()
            if ev is None:
                break
            self.on_event(ev)

    def on_event(self, ev):
        if isinstance(ev, HandshakeCompleted):
            self.live = True
            self.log("*** QUIC handshake complete ***")
            self.on_connected()
        elif isinstance(ev, ProtocolNegotiated):
            self.log(f"ALPN: {ev.alpn_protocol}")
        elif isinstance(ev, StreamDataReceived):
            self.on_stream(ev)
        elif isinstance(ev, DatagramFrameReceived):
            self.on_datagram(ev)
        elif isinstance(ev, StreamReset):
            self.log(f"  <<< StreamReset {ev.stream_id} err={ev.error_code}")
        elif isinstance(ev, ConnectionTerminated):
            self.log(f"*** terminated: {ev.error_code} {ev.reason_phrase!r}")
            self.term = True
            self.live = False

    # ---- send helpers ----
    def open_stream(self, app, chan, label=""):
        sid = self.conn.get_next_available_stream_id()
        self.conn.send_stream_data(sid, stream_header(app, chan))
        self.streams[sid] = {"app": app, "chan": chan, "hdr": True}
        self.log(f"  -> open stream {sid} app={app} chan={chan} {label}")
        return sid

    def send_framed(self, sid, payload, label=""):
        self.conn.send_stream_data(sid, frame(payload))
        self.log(f"  -> send {label} {len(payload)}B on stream {sid}")

    def on_connected(self):
        js = self.js
        ctrl = self.open_stream(APP, CTRL, "(ctrl)")

        # exact native ctrl sequence
        self.send_framed(ctrl, b"\x00\x00", "ctrl hello")
        for chan, wire in ((3, 2), (5, 4)):
            self.send_framed(ctrl,
                             bytes([2, APP]) + struct.pack(">I", chan) + struct.pack(">I", wire),
                             f"OpenUnreliable c{chan} w{wire}")

        # auth
        ct = js.get("ClientTicket", "")
        version = int(ct.split(";")[-1]) if ct else 0
        chan1 = self.open_stream(APP, 1, "(chan1)")
        self.send_framed(chan1, early_auth_payload(ct, version), "EARLY-AUTH")

        # remaining native ctrl opens
        for chan, wire in ((6, 6), (11, 8)):
            self.send_framed(ctrl,
                             bytes([2, APP]) + struct.pack(">I", chan) + struct.pack(">I", wire),
                             f"OpenUnreliable c{chan} w{wire}")

        # native join messages
        self.send_framed(chan1, load_cap("msg_0006_a4_c1.bin"), "A7")
        self.send_framed(chan1, load_cap("msg_0007_a4_c1.bin"), "90-BIG")
        self.send_framed(chan1, load_cap("msg_0009_a4_c1.bin"), "92")
        m8a = build_8a(js)
        self.send_framed(chan1, m8a, f"8A-REBUILT({len(m8a)}B)")
        self.send_framed(chan1, load_cap("msg_0013_a4_c1.bin"), "8F")
        self.m8a = m8a
        self.chan1 = chan1

        # 9B ~1.5s later (native sent it after server's first reply)
        asyncio.ensure_future(self.later_9b())

        async def ka():
            while self.live:
                try:
                    self.conn.ping(None)
                except Exception:
                    pass
                await asyncio.sleep(1.0)
        asyncio.ensure_future(ka())

    async def later_9b(self):
        await asyncio.sleep(1.5)
        if self.live and not self.sent_9b:
            self.sent_9b = True
            try:
                self.send_framed(self.chan1, load_cap("msg_0016_a4_c1.bin"), "9B(delayed)")
            except Exception as e:
                self.log(f"9B send error {e}")

    # ---- recv ----
    def on_stream(self, ev):
        st = self.streams.get(ev.stream_id)
        data = ev.data
        if st is None and len(data) >= 7 and data[0] == 6 and data[1] == 1:
            app = data[2]
            chan = struct.unpack(">I", data[3:7])[0]
            self.streams[ev.stream_id] = {"app": app, "chan": chan, "hdr": True}
            st = self.streams[ev.stream_id]
            self.log(f"  <<< SERVER STREAM {ev.stream_id} app={app} chan={chan} hdr={data[:7].hex()}")
            data = data[7:]
            if app == APP:
                self.recv_chan[(app, chan)] = ev.stream_id
        elif st is None:
            self.log(f"  <<< stream {ev.stream_id} raw {len(ev.data)}B: {data[:64].hex()}")
            return
        if len(data) > 0:
            tag = f"app={st['app']} chan={st['chan']} sid={ev.stream_id}"
            self.log(f"      <<< RX {tag} {len(data)}B end={ev.end_stream}: {data[:80].hex()}")
            # accumulate full payloads for interesting channels
            if st["app"] == APP and st["chan"] == 1:
                self.chan1_rx = getattr(self, "chan1_rx", b"") + data
                self.got_game_data = True
                with open(os.path.join(ROOT, "run/rx_app4_chan1.bin"), "ab") as f:
                    f.write(data)
            elif st["app"] == 6 and st["chan"] == 1:
                self.app6_rx = getattr(self, "app6_rx", b"") + data
                with open(os.path.join(ROOT, "run/rx_app6_chan1.bin"), "ab") as f:
                    f.write(data)
        if st["app"] == APP and st["chan"] == CTRL:
            self.parse_ctrl(data)
        elif st["app"] == 0:
            self.parse_ctrl(data)

    def parse_ctrl(self, data):
        off = 0
        while off < len(data):
            b0 = data[off]
            n = 1 << (b0 >> 6)
            if off + n > len(data):
                break
            ln = b0 & 0x3F
            for i in range(1, n):
                ln = (ln << 8) | data[off + i]
            off += n
            if ln == 0 or off + ln > len(data):
                break
            body = data[off:off + ln]
            off += ln
            if len(body) >= 10 and body[0] == 2:
                app = body[1]
                chan = struct.unpack(">I", body[2:6])[0]
                wire = struct.unpack(">I", body[6:10])[0]
                self.server_wires[wire] = (app, chan)
                self.log(f"     ctrl OpenUnreliable app={app} chan={chan} wire={wire}")
            elif len(body) >= 6 and body[0] in (1, 3):
                self.log(f"     ctrl type={body[0]} app={body[1]} chan={struct.unpack('>I', body[2:6])[0]}")
            else:
                self.log(f"     ctrl msg {len(body)}B: {body[:32].hex()}")

    def on_datagram(self, ev):
        d = ev.data
        try:
            b0 = d[0]
            v = b0 & 0x7F
            off = 1
            while b0 & 0x80:
                b0 = d[off]
                off += 1
                v = (v << 7) | (b0 & 0x7F)
            mapping = self.server_wires.get(v, "?")
            self.log(f"  <<< DGRAM {len(d)}B wire={v} {mapping}: {d[:48].hex()}")
        except Exception:
            self.log(f"  <<< DGRAM {len(d)}B: {d[:48].hex()}")


async def run(args):
    cookie = open(args.cookie_file).read().strip() if args.cookie_file else open(os.path.join(ROOT, "run/cookie.txt")).read().strip()
    js, _ = join_game(args.place, None, cookie, None)
    print(f"joined job={js.get('GameId')} udmux={js['UdmuxEndpoints'][0]['Address']}:{js['NetStackPort']}")
    p = Probe(js)
    cfg = QuicConfiguration(is_client=True, alpn_protocols=["RbxTransport"])
    cfg.verify_mode = ssl.CERT_NONE
    cfg.max_datagram_frame_size = 65535
    cfg.idle_timeout = 30.0
    conn = QuicConnection(configuration=cfg)
    p.conn = conn
    loop = asyncio.get_event_loop()
    transport, proto = await loop.create_datagram_endpoint(
        lambda: RuppTransport(conn, p.rcc_ip, p.port, p.token, p.log),
        remote_addr=(p.udmux_ip, p.port))
    proto.peer = (p.udmux_ip, p.port)
    proto.on_events = p.events
    conn.connect(proto.peer, time.monotonic())
    proto.flush()
    t0 = time.monotonic()
    while time.monotonic() - t0 < args.seconds and not p.term:
        await asyncio.sleep(0.5)
    print(f"game_data={p.got_game_data} server_wires={p.server_wires}")
    try:
        conn.close()
        proto.flush()
    except Exception:
        pass
    transport.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cookie-file")
    ap.add_argument("--place", type=int, default=1818)
    ap.add_argument("--seconds", type=int, default=25)
    args = ap.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
