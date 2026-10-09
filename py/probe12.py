#!/usr/bin/env python3
"""probe12 — immediate answer timing test.

Answers the 0x9B challenge within ~1ms of receiving it, with mode:
  --mode echo   : [9b][u2][u1]  (wrong; echoes values)
  --mode zeros  : [9b][u2][0]
  --mode file   : send run/oracle_answer.txt content (oracle pipeline)
  --mode none   : do not answer (timeout behavior baseline)

Logs kick/connection-close events with timestamps so we can compare
"wrong answer kick" vs "no answer reset" behavior.
"""
import argparse, asyncio, json, os, ssl, struct, sys, time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rbx_client import join_game, early_auth_payload, RuppTransport
from probe5 import APP, CTRL, build_8a, frame, stream_header, load_cap
from probe7 import build_90, build_92

from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.connection import QuicConnection

ANS_PATH = os.path.join(ROOT, "run/oracle_answer.txt")


class P12:
    def __init__(self, js, reply, mode, log=print):
        self.js = js
        self.reply = reply
        self.mode = mode
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
        self.term = None
        self.streams = {}
        self.chan1_rx = b""
        self.challenge = None
        self.reset = []
        self.answered_at = None
        self.t0 = None
        self.a7_mode = "empty"

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
            self.log(f"  <<< StreamReset {ev.stream_id} err={ev.error_code} (+{round(time.monotonic()-self.t0,4)}s)")
        elif isinstance(ev, ConnectionTerminated):
            self.term = (ev.error_code, ev.reason_phrase)
            self.log(f"*** TERMINATED code={ev.error_code} reason={ev.reason_phrase!r} (+{round(time.monotonic()-self.t0,4)}s)")
            self.live = False
        elif isinstance(ev, DatagramFrameReceived):
            self.log(f"  <<< DGRAM {len(ev.data)}B: {ev.data[:40].hex()}")

    def open_stream(self, app, chan, label=""):
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
        if self.a7_mode == "real":
            a7 = load_cap("msg_0006_a4_c1.bin")
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
            self.try_challenge()
        elif len(data) > 0:
            self.log(f"      <<< RX app={st['app']} chan={st['chan']} {len(data)}B: {data[:40].hex()}")

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
                self.answer_immediately(u1, u2)
                return

    def answer_immediately(self, u1, u2):
        if self.mode == "none":
            self.log("  (mode=none: not answering)")
            return
        if self.mode == "echo":
            resp = bytes([0x9B]) + struct.pack("<I", u2) + struct.pack("<I", u1)
        elif self.mode == "zeros":
            resp = bytes([0x9B]) + struct.pack("<I", u2) + b"\x00\x00\x00\x00"
        elif self.mode == "file":
            if not os.path.exists(ANS_PATH):
                self.log("  (mode=file: no answer file yet; will poll)")
                asyncio.ensure_future(self.poll_file())
                return
            resp = bytes.fromhex(open(ANS_PATH).read().strip())
            os.remove(ANS_PATH)
        else:
            return
        self.send_framed(self.chan1, resp, f"9B-{self.mode.upper()}")
        self.answered_at = time.monotonic()

    async def poll_file(self):
        while self.live and self.challenge and self.answered_at is None:
            if os.path.exists(ANS_PATH):
                raw = open(ANS_PATH).read().strip()
                if raw:
                    os.remove(ANS_PATH)
                    resp = bytes.fromhex(raw)
                    self.send_framed(self.chan1, resp, "9B-FILE")
                    self.answered_at = time.monotonic()
                    return
            await asyncio.sleep(0.001)

    async def keepalive(self):
        while self.live:
            try:
                self.conn.ping(None)
            except Exception:
                pass
            await asyncio.sleep(1.0)


async def run(args):
    cookie = open(os.environ.get("RBX_COOKIE_FILE", os.path.join(ROOT, "run/cookie.txt"))).read().strip()
    js, reply = join_game(args.place, None, cookie, args.job)
    print(f"joined job={js.get('GameId')} udmux={js['UdmuxEndpoints'][0]['Address']}:{js['NetStackPort']} mode={args.mode}", flush=True)
    if os.path.exists(ANS_PATH):
        os.remove(ANS_PATH)
    p = P12(js, reply, args.mode)
    p.a7_mode = args.a7
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
    print(f"mode={args.mode} challenge={'yes' if p.challenge else 'no'} answered={p.answered_at is not None} resets={p.reset} term={p.term} chan1_rx={len(p.chan1_rx)}")
    try:
        conn.close(); proto.flush()
    except Exception:
        pass
    transport.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", default="echo", choices=["echo", "zeros", "file", "none"])
    ap.add_argument("--seconds", type=int, default=15)
    ap.add_argument("--place", type=int, default=1818)
    ap.add_argument("--job", default=None)
    ap.add_argument("--a7", default="empty", choices=["empty", "real"])
    args = ap.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
