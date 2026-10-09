#!/usr/bin/env python3
"""probe15 — probe11 with full aioquic QUIC frame logging.

Writes qlog-style frame log to stderr and a JSON file, so we can see the exact
frame sequence around the server's StreamReset.
"""
import argparse
import asyncio
import json
import os
import ssl
import struct
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rbx_client import join_game, early_auth_payload, RuppTransport
from probe5 import APP, CTRL, build_8a, frame, stream_header, load_cap
from probe7 import build_90, build_92

from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.connection import QuicConnection
from aioquic.quic.logger import QuicLogger


class P15:
    def __init__(self, js, reply, log=print):
        self.js = js
        self.reply = reply
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
            self.log("*** handshake complete ***")
            self.on_connected()
        elif isinstance(ev, StreamDataReceived):
            self.on_stream(ev)
        elif isinstance(ev, StreamReset):
            self.reset.append((ev.stream_id, round(time.monotonic() - self.t0, 4)))
            self.log(f"  <<< StreamReset {ev.stream_id} err={ev.error_code} err_hex={ev.error_code:#x} (+{round(time.monotonic()-self.t0,4)}s)")
        elif isinstance(ev, ConnectionTerminated):
            self.log(f"*** terminated: {ev.error_code} {ev.reason_phrase!r}")
            self.term = True
            self.live = False
        elif isinstance(ev, DatagramFrameReceived):
            self.log(f"  <<< DGRAM {len(ev.data)}B: {ev.data[:40].hex()}")

    def open_stream(self, app, chan, label=""):
        sid = self.conn.get_next_available_stream_id()
        self.conn.send_stream_data(sid, stream_header(app, chan))
        self.streams[sid] = {"app": app, "chan": chan}
        self.log(f"  -> OPEN stream {sid} app={app} chan={chan} {label}")
        return sid

    def send_framed(self, sid, payload, label=""):
        self.conn.send_stream_data(sid, frame(payload))
        self.log(f"  -> send {label} {len(payload)}B")

    def on_connected(self):
        js = self.js
        ctrl = self.open_stream(APP, CTRL, "(ctrl)")
        self.send_framed(ctrl, b"\x00\x00", "ctrl hello")
        for chan, wire in ((3, 2), (5, 4)):
            self.send_framed(ctrl, bytes([2, APP]) + struct.pack(">I", chan) + struct.pack(">I", wire), f"openU c{chan}")
        ct = js["ClientTicket"]
        version = int(ct.split(";")[-1])
        chan1 = self.open_stream(APP, 1, "(chan1)")
        self.chan1 = chan1
        self.send_framed(chan1, early_auth_payload(ct, version), "EARLY-AUTH")
        for chan, wire in ((6, 6), (11, 8)):
            self.send_framed(ctrl, bytes([2, APP]) + struct.pack(">I", chan) + struct.pack(">I", wire), f"openU c{chan}")
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
            self.log(f"  <<< SERVER OPEN stream {ev.stream_id} app={app} chan={chan}")
            data = data[7:]
        elif st is None:
            self.log(f"  <<< data on unknown stream {ev.stream_id}: {len(data)}B")
            return
        if len(data) > 0 and st["app"] == APP and st["chan"] == 1:
            self.chan1_rx += data
            self.log(f"      <<< RX chan1 {len(data)}B: {data[:24].hex()}")
        elif len(data) > 0:
            self.log(f"      <<< RX app={st['app']} chan={st['chan']} {len(data)}B: {data[:32].hex()}")


async def run(args):
    cookie = open(os.environ.get("RBX_COOKIE_FILE", os.path.join(ROOT, "run/cookie2.txt"))).read().strip()
    js, reply = join_game(args.place, None, cookie, args.job, args.follow)
    print(f"joined job={js.get('GameId')} udmux={js['UdmuxEndpoints'][0]['Address']}:{js['NetStackPort']}", flush=True)
    p = P15(js, reply)
    cfg = QuicConfiguration(is_client=True, alpn_protocols=["RbxTransport"])
    cfg.verify_mode = ssl.CERT_NONE
    cfg.max_datagram_frame_size = 65535
    cfg.idle_timeout = 60.0
    logger = QuicLogger()
    cfg.quic_logger = logger
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
        await asyncio.sleep(0.001)
    print(f"challenge={'yes' if p.challenge else 'no'} resets={p.reset} chan1_rx={len(p.chan1_rx)}")
    # dump quic logger JSON
    with open("/tmp/opencode/qlog.json", "w") as f:
        json.dump(logger.to_dict(), f, indent=1)
    print("qlog written to /tmp/opencode/qlog.json")
    try:
        conn.close(); proto.flush()
    except Exception:
        pass
    transport.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=8)
    ap.add_argument("--place", type=int, default=1818)
    ap.add_argument("--job", default=None)
    ap.add_argument("--follow", default=None)
    args = ap.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
