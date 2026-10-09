#!/usr/bin/env python3
"""probe11 — stepwise reset isolation probe.

Tests which client messages trigger the server's chan1 StreamReset (0x106).
Stages (--stage N), cumulative:
  0 = connect + nothing
  1 = ctrl hello only
  2 = ctrl hello + openU c3/c5
  3 = + early-auth (chan1 opened, EARLY-AUTH sent)
  4 = + openU c6/c11
  5 = + A7 (empty or real)
  6 = + 90
  7 = + 92
  8 = + 8A
  9 = + 8F  (full, same as probe10)

Usage:
  python3 probe11.py --stage 3 --seconds 20
"""
import argparse, asyncio, json, os, ssl, struct, sys, time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rbx_client import join_game, early_auth_payload, RuppTransport
from probe5 import APP, CTRL, build_8a, frame, stream_header, load_cap
from probe7 import build_90, build_92

from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.connection import QuicConnection


class P11:
    def __init__(self, js, reply, stage, log=print):
        self.js = js
        self.reply = reply
        self.stage = stage
        import time as _t
        def _flog(*a, **k):
            k["flush"] = True
            log(f"[{_t.monotonic():.3f}]", *a, **k)
        self.log = _flog
        self.udmux_ip = js["UdmuxEndpoints"][0]["Address"]
        self.rcc_ip = js["ServerConnections"][0]["Address"]
        self.port = int(js["NetStackPort"])
        import base64
        self.token = base64.b64decode(js["NetStackTokenValue"])
        self.conn = None
        self.live = False
        self.term = False
        self.streams = {}
        self.chan1_rx = b""
        self.challenge = None
        self.reset = []
        self.t0 = None
        self.uni = False

    def events(self):
        while True:
            ev = self.conn.next_event()
            if ev is None:
                break
            self.on_event(ev)

    def on_event(self, ev):
        from aioquic.quic.events import (
            HandshakeCompleted, StreamDataReceived, StreamReset,
            DatagramFrameReceived, ConnectionTerminated)
        if isinstance(ev, HandshakeCompleted):
            self.live = True
            self.t0 = time.monotonic()
            self.log(f"*** handshake complete ***")
            self.t0 = time.monotonic()
            self.on_connected()
        elif isinstance(ev, StreamDataReceived):
            self.on_stream(ev)
        elif isinstance(ev, StreamReset):
            self.reset.append((ev.stream_id, round(time.monotonic() - self.t0, 4)))
            self.log(f"  <<< StreamReset {ev.stream_id} err={ev.error_code} at +{round(time.monotonic()-self.t0,4)}s")
        elif isinstance(ev, ConnectionTerminated):
            self.log(f"*** terminated: {ev.error_code} {ev.reason_phrase!r}")
            self.term = True
            self.live = False
        elif isinstance(ev, DatagramFrameReceived):
            self.log(f"  <<< DGRAM {len(ev.data)}B: {ev.data[:48].hex()}")

    def open_stream(self, app, chan, label=""):
        sid = self.conn.get_next_available_stream_id(is_unidirectional=self.uni)
        self.conn.send_stream_data(sid, stream_header(app, chan))
        self.streams[sid] = {"app": app, "chan": chan}
        return sid

    def send_framed(self, sid, payload, label=""):
        self.conn.send_stream_data(sid, frame(payload))
        self.log(f"  -> send {label} {len(payload)}B")

    def on_connected(self):
        js = self.js
        if self.stage < 1:
            self.log("  (stage 0: sending nothing)")
            return
        ctrl = self.open_stream(APP, CTRL)
        self.send_framed(ctrl, b"\x00\x00", "ctrl hello")
        if self.stage < 2:
            return
        for chan, wire in ((3, 2), (5, 4)):
            self.send_framed(ctrl, bytes([2, APP]) + struct.pack(">I", chan) + struct.pack(">I", wire), f"openU c{chan}")
        if self.stage < 3:
            return
        ct = js["ClientTicket"]
        version = int(ct.split(";")[-1])
        chan1 = self.open_stream(APP, 1)
        self.send_framed(chan1, early_auth_payload(ct, version), "EARLY-AUTH")
        if self.stage < 4:
            return
        for chan, wire in ((6, 6), (11, 8)):
            self.send_framed(ctrl, bytes([2, APP]) + struct.pack(">I", chan) + struct.pack(">I", wire), f"openU c{chan}")
        if self.stage < 5:
            return
        if getattr(self, "a7_mode", "empty") == "skip":
            self.log("  (A7 skipped entirely)")
        elif getattr(self, "a7_mode", "empty") == "real":
            a7 = load_cap(os.environ.get("RBX_A7", "msg_0006_a4_c1.bin"))
            self.send_framed(chan1, a7, f"A7-real({len(a7)}B)")
        else:
            self.send_framed(chan1, bytes([0xA7, 0x00, 0x00]), "A7-empty")
        if self.stage < 6:
            return
        self.send_framed(chan1, build_90(js, self.reply), "90")
        if self.stage < 7:
            return
        self.send_framed(chan1, build_92(), "92")
        if self.stage < 8:
            return
        self.send_framed(chan1, build_8a(js), "8A")
        if self.stage < 9:
            return
        self.send_framed(chan1, b"\x8f\x00", "8F")

    async def keepalive(self):
        while self.live:
            try:
                self.conn.ping(None)
            except Exception:
                pass
            await asyncio.sleep(1.0)

    def on_stream(self, ev):
        st = self.streams.get(ev.stream_id)
        data = ev.data
        if st is None and len(data) >= 7 and data[0] == 6 and data[1] == 1:
            app = data[2]
            chan = struct.unpack(">I", data[3:7])[0]
            self.streams[ev.stream_id] = {"app": app, "chan": chan}
            st = self.streams[ev.stream_id]
            self.log(f"  <<< SERVER STREAM {ev.stream_id} app={app} chan={chan}")
            data = data[7:]
        elif st is None:
            return
        if len(data) > 0 and st["app"] == APP and st["chan"] == 1:
            self.chan1_rx += data
            self.log(f"      <<< RX chan1 {len(data)}B: {data[:32].hex()}")
            self.try_challenge()
        elif len(data) > 0:
            self.log(f"      <<< RX app={st['app']} chan={st['chan']} {len(data)}B: {data[:48].hex()}")

    def try_challenge(self):
        if self.challenge:
            return
        d = bytes(self.chan1_rx)
        off = 0
        while off < len(d):
            b0 = d[off]
            n = 1 << (b0 >> 6)
            if off + n > len(d):
                return
            v = b0 & 0x3F
            for i in range(1, n):
                v = (v << 8) | d[off + i]
            if off + n + v > len(d):
                return
            msg = d[off + n: off + n + v]
            off += n + v
            if msg[:1] == b"\x9b" and len(msg) >= 9:
                u1 = struct.unpack("<I", msg[1:5])[0]
                u2 = struct.unpack("<I", msg[5:9])[0]
                self.log(f"  *** CHALLENGE u1=0x{u1:08x} u2=0x{u2:08x} at +{round(time.monotonic()-self.t0,4)}s")
                self.challenge = (u1, u2)


async def run(args):
    cookie = open(os.environ.get("RBX_COOKIE_FILE", os.path.join(ROOT, "run/cookie.txt"))).read().strip()
    js, reply = join_game(args.place, None, cookie, args.job)
    print(f"joined job={js.get('GameId')} udmux={js['UdmuxEndpoints'][0]['Address']}:{js['NetStackPort']} stage={args.stage}", flush=True)
    p = P11(js, reply, args.stage)
    p.a7_mode = args.a7
    p.uni = args.uni
    cfg = QuicConfiguration(is_client=True, alpn_protocols=["RbxTransport"])
    cfg.verify_mode = ssl.CERT_NONE
    cfg.max_datagram_frame_size = 65535
    cfg.idle_timeout = 60.0
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
    asyncio.ensure_future(p.keepalive())
    t0 = time.monotonic()
    while time.monotonic() - t0 < args.seconds and not p.term:
        await asyncio.sleep(0.001)
    print(f"stage={args.stage} challenge={'yes' if p.challenge else 'no'} resets={p.reset} chan1_rx={len(p.chan1_rx)}")
    try:
        conn.close(); proto.flush()
    except Exception:
        pass
    transport.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", type=int, default=9)
    ap.add_argument("--seconds", type=int, default=20)
    ap.add_argument("--place", type=int, default=1818)
    ap.add_argument("--job", default=None)
    ap.add_argument("--a7", default="empty", choices=["empty", "real", "skip"])
    ap.add_argument("--uni", action="store_true", help="open client streams as unidirectional")
    args = ap.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
