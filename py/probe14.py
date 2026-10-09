#!/usr/bin/env python3
"""probe14 — game connection + DUMMY (app=6) connection, mirroring the native.

The native client always opens a second QUIC connection ("DummyClient") to the
same server with the same RUPP token. It:
  * opens ctrl + reliable chan1 + unreliable chans 2,3 (wires 2 and 4)
  * pings 77-byte payloads on chan1 every 1s
  * sends 25-byte time sync on chan3 every 5s

Hypothesis: the server pairs the two connections (same NetStackTokenValue) and
only fully accepts the game join when the dummy is present.

Usage: probe14.py --seconds 30 [--dummy skip|basic|full] [--a7 empty|real|skip]
"""
import argparse
import asyncio
import json
import os
import ssl
import struct
import sys
import time

sys.path.insert(0, "/home/john/RobloxInBrowser/py")
from rbx_client import join_game, early_auth_payload, RuppTransport
from probe5 import APP, CTRL, build_8a, frame, stream_header, load_cap
from probe7 import build_90, build_92

from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.connection import QuicConnection

APP6 = 6

# captured dummy payload templates
PING_TMPL = bytes.fromhex("0858fbbded5ddc1840000000") + b"P" * 64   # 77 bytes total
SYNC_TMPL = bytes.fromhex("03fa4715010000000000000000000000000000000000000000")  # 25 bytes


class Dummy:
    """app=6 dummy connection."""

    def __init__(self, js, log, mode="full"):
        self.js = js
        self.log = log
        self.mode = mode
        import base64
        self.udmux_ip = js["UdmuxEndpoints"][0]["Address"]
        self.rcc_ip = js["ServerConnections"][0]["Address"]
        self.port = int(js["NetStackPort"])
        self.token = base64.b64decode(js["NetStackTokenValue"])
        self.conn = None
        self.transport = None
        self.proto = None
        self.live = False
        self.streams = {}
        self.ctrl = None
        self.c1 = None
        self.c3 = None
        self.rx = 0

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
            self.log("*** DUMMY handshake complete ***")
            self.on_connected()
        elif isinstance(ev, StreamDataReceived):
            self.on_stream(ev)
        elif isinstance(ev, StreamReset):
            self.log(f"  DUMMY <<< StreamReset {ev.stream_id} err={ev.error_code}")
        elif isinstance(ev, ConnectionTerminated):
            self.log(f"*** DUMMY terminated: {ev.error_code} {ev.reason_phrase!r}")
            self.live = False
        elif isinstance(ev, DatagramFrameReceived):
            self.log(f"  DUMMY <<< DGRAM {len(ev.data)}B: {ev.data[:40].hex()}")

    def open_stream(self, app, chan):
        sid = self.conn.get_next_available_stream_id()
        self.conn.send_stream_data(sid, stream_header(app, chan))
        self.streams[sid] = {"app": app, "chan": chan}
        self.log(f"  DUMMY -> open stream {sid} app={app} chan={chan}")
        return sid

    def send_ctrl(self, payload, label=""):
        self.conn.send_stream_data(self.ctrl, frame(payload))
        self.log(f"  DUMMY -> ctrl {label} {len(payload)}B")

    def send_framed(self, sid, payload, label=""):
        self.conn.send_stream_data(sid, frame(payload))
        self.log(f"  DUMMY -> send {label} {len(payload)}B")

    def on_connected(self):
        self.ctrl = self.open_stream(APP6, CTRL)
        self.send_ctrl(b"\x00\x00", "hello")
        # native order (from captures): openU c2 wire2, openU c3 wire4
        self.send_ctrl(bytes([2, APP6]) + struct.pack(">I", 2) + struct.pack(">I", 2), "openU c2")
        self.send_ctrl(bytes([2, APP6]) + struct.pack(">I", 3) + struct.pack(">I", 4), "openU c3")
        # reliable chan1: try explicit openReliable
        self.send_ctrl(bytes([1, APP6]) + struct.pack(">I", 1), "openR c1")
        self.c1 = self.open_stream(APP6, 1)
        self.c3 = self.open_stream(APP6, 3)
        if self.mode == "full":
            asyncio.ensure_future(self.ping_loop())
            asyncio.ensure_future(self.sync_loop())

    async def ping_loop(self):
        n = 0
        while self.live:
            n += 1
            payload = PING_TMPL  # TODO: refresh timestamp field
            try:
                self.send_framed(self.c1, payload, f"ping#{n}")
            except Exception as e:
                self.log(f"  DUMMY ping err {e}")
            await asyncio.sleep(1.0)

    async def sync_loop(self):
        n = 0
        while self.live:
            n += 1
            try:
                self.send_framed(self.c3, SYNC_TMPL, f"sync#{n}")
            except Exception as e:
                self.log(f"  DUMMY sync err {e}")
            await asyncio.sleep(5.0)

    def on_stream(self, ev):
        st = self.streams.get(ev.stream_id)
        data = ev.data
        if st is None and len(data) >= 7 and data[0] == 6 and data[1] == 1:
            app = data[2]
            chan = struct.unpack(">I", data[3:7])[0]
            self.streams[ev.stream_id] = {"app": app, "chan": chan}
            st = self.streams[ev.stream_id]
            self.log(f"  DUMMY <<< SERVER STREAM {ev.stream_id} app={app} chan={chan}")
            data = data[7:]
        elif st is None:
            return
        if data:
            self.rx += len(data)
            self.log(f"  DUMMY <<< RX app={st['app']} chan={st['chan']} {len(data)}B: {data[:40].hex()}")

    async def start(self, loop):
        cfg = QuicConfiguration(is_client=True, alpn_protocols=["RbxTransport"])
        cfg.verify_mode = ssl.CERT_NONE
        cfg.max_datagram_frame_size = 65535
        cfg.idle_timeout = 60.0
        conn = QuicConnection(configuration=cfg)
        self.conn = conn
        transport, proto = await loop.create_datagram_endpoint(
            lambda: RuppTransport(conn, self.rcc_ip, self.port, self.token, self.log),
            remote_addr=(self.udmux_ip, self.port))
        proto.peer = (self.udmux_ip, self.port)
        proto.on_events = self.events
        self.transport = transport
        self.proto = proto
        conn.connect(proto.peer, time.monotonic())
        proto.flush()
        self.log(f"  DUMMY connecting to {self.udmux_ip}:{self.port}")

    def close(self):
        try:
            self.conn.close()
            self.proto.flush()
        except Exception:
            pass
        try:
            self.transport.close()
        except Exception:
            pass


class P14:
    def __init__(self, js, reply, log=print, dummy_mode="full"):
        self.js = js
        self.reply = reply
        import time as _t
        def _flog(*a, **k):
            k["flush"] = True
            log(f"[{_t.monotonic():.3f}]", *a, **k)
        self.log = _flog
        self.dummy_mode = dummy_mode
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
        self.dummy = None
        self.t0 = None

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
            self.log("*** GAME handshake complete ***")
            self.on_connected()
        elif isinstance(ev, StreamDataReceived):
            self.on_stream(ev)
        elif isinstance(ev, StreamReset):
            self.reset.append((ev.stream_id, round(time.monotonic() - self.t0, 4)))
            self.log(f"  <<< StreamReset {ev.stream_id} err={ev.error_code} (+{round(time.monotonic()-self.t0,4)}s)")
        elif isinstance(ev, ConnectionTerminated):
            self.log(f"*** GAME terminated: {ev.error_code} {ev.reason_phrase!r}")
            self.term = True
            self.live = False
        elif isinstance(ev, DatagramFrameReceived):
            self.log(f"  <<< DGRAM {len(ev.data)}B: {ev.data[:40].hex()}")

    def open_stream(self, app, chan):
        sid = self.conn.get_next_available_stream_id()
        self.conn.send_stream_data(sid, stream_header(app, chan))
        self.streams[sid] = {"app": app, "chan": chan}
        return sid

    def send_framed(self, sid, payload, label=""):
        self.conn.send_stream_data(sid, frame(payload))
        self.log(f"  -> send {label} {len(payload)}B")

    def on_connected(self):
        js = self.js
        ctrl = self.open_stream(APP, CTRL)
        self.send_framed(ctrl, b"\x00\x00", "ctrl hello")
        for chan, wire in ((3, 2), (5, 4)):
            self.send_framed(ctrl, bytes([2, APP]) + struct.pack(">I", chan) + struct.pack(">I", wire), f"openU c{chan}")
        ct = js["ClientTicket"]
        version = int(ct.split(";")[-1])
        chan1 = self.open_stream(APP, 1)
        self.chan1 = chan1
        self.send_framed(chan1, early_auth_payload(ct, version), "EARLY-AUTH")
        for chan, wire in ((6, 6), (11, 8)):
            self.send_framed(ctrl, bytes([2, APP]) + struct.pack(">I", chan) + struct.pack(">I", wire), f"openU c{chan}")
        mode = getattr(self, "a7_mode", "empty")
        if mode == "skip":
            self.log("  (A7 skipped)")
        elif mode == "real":
            a7 = load_cap(os.environ.get("RBX_A7", "msg_0006_a4_c1.bin"))
            self.send_framed(chan1, a7, f"A7-real({len(a7)}B)")
        else:
            self.send_framed(chan1, bytes([0xA7, 0x00, 0x00]), "A7-empty")
        self.send_framed(chan1, build_90(js, self.reply), "90")
        self.send_framed(chan1, build_92(), "92")
        self.send_framed(chan1, build_8a(js), "8A")
        self.send_framed(chan1, b"\x8f\x00", "8F")

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
            self.log(f"      <<< RX chan1 {len(data)}B (+{round(time.monotonic()-self.t0,4)}s): {data[:24].hex()}")
            self.try_challenge()
        elif len(data) > 0:
            self.log(f"      <<< RX app={st['app']} chan={st['chan']} {len(data)}B (+{round(time.monotonic()-self.t0,4)}s): {data[:32].hex()}")

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
            if msg[:1] == b"\x9b" and len(msg) >= 13:
                u1 = struct.unpack("<I", msg[1:5])[0]
                u2 = struct.unpack("<I", msg[5:9])[0]
                self.challenge = (u1, u2)
                self.log(f"  *** CHALLENGE u1=0x{u1:08x} u2=0x{u2:08x} (+{round(time.monotonic()-self.t0,4)}s)")
                return


async def run(args):
    cookie = open(os.environ.get("RBX_COOKIE_FILE", "/home/john/RobloxInBrowser/run/cookie2.txt")).read().strip()
    js, reply = join_game(args.place, None, cookie, args.job, args.follow)
    print(f"joined job={js.get('GameId')} udmux={js['UdmuxEndpoints'][0]['Address']}:{js['NetStackPort']} dummy={args.dummy}", flush=True)
    p = P14(js, reply)
    p.a7_mode = args.a7
    loop = asyncio.get_event_loop()

    d = None
    if args.dummy != "skip":
        d = Dummy(js, p.log, mode=args.dummy)
        p.dummy = d
        await d.start(loop)
        await asyncio.sleep(0.05)

    cfg = QuicConfiguration(is_client=True, alpn_protocols=["RbxTransport"])
    cfg.verify_mode = ssl.CERT_NONE
    cfg.max_datagram_frame_size = 65535
    cfg.idle_timeout = 60.0
    conn = QuicConnection(configuration=cfg)
    p.conn = conn
    transport, proto = await loop.create_datagram_endpoint(
        lambda: RuppTransport(conn, p.rcc_ip, p.port, p.token, p.log),
        remote_addr=(p.udmux_ip, p.port))
    proto.peer = (p.udmux_ip, p.port)
    proto.on_events = p.events
    conn.connect(proto.peer, time.monotonic())
    proto.flush()
    t0 = time.monotonic()
    while time.monotonic() - t0 < args.seconds and not p.term:
        await asyncio.sleep(0.001)
    print(f"dummy={args.dummy} challenge={'yes' if p.challenge else 'no'} resets={p.reset} chan1_rx={len(p.chan1_rx)} dummy_rx={d.rx if d else 0}")
    try:
        conn.close(); proto.flush()
    except Exception:
        pass
    transport.close()
    if d:
        d.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=20)
    ap.add_argument("--place", type=int, default=1818)
    ap.add_argument("--job", default=None)
    ap.add_argument("--follow", default=None)
    ap.add_argument("--a7", default="empty", choices=["empty", "real", "skip"])
    ap.add_argument("--dummy", default="full", choices=["skip", "basic", "full"])
    args = ap.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
