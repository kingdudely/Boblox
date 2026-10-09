#!/usr/bin/env python3
"""probe10 — fully local 0x9B challenge solver client.

1. This client joins (empty or real A7), receives the 0x9B challenge.
2. The answer is computed LOCALLY by py/solve9b.py:
     blob -> XOR-decode + zstd -> wire -> standard Luau bytecode -> rbx_runtime
   (no oracle, no native in the loop; see FINDINGS.md ## BREAKTHROUGH).
3. The 9-byte answer [9b][u2][answer] is sent on chan1 and the server's
   reaction is logged (peer assignment = success, 0x106 reset = failure).

--source local (default) uses solve9b; --source oracle keeps the old
file-pair handshake (run/oracle_challenge.bin -> run/oracle_answer.txt).

Usage: probe10.py --seconds 120
"""
import argparse, asyncio, json, os, socket, ssl, struct, sys, time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rbx_client import join_game, early_auth_payload, RuppTransport
from probe5 import APP, CTRL, build_8a, frame, stream_header, load_cap
from probe7 import build_90, build_92

from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.connection import QuicConnection

CHAL_PATH = os.path.join(ROOT, "run/oracle_challenge.bin")
ANS_PATH = os.path.join(ROOT, "run/oracle_answer.txt")

APP6 = 6
# captured dummy-connection payload templates (native TX, dummycap2)
PING_TMPL = bytes.fromhex("0858fbbded5ddc184000000001") + b"P" * 64   # 77B
SYNC_TMPL = bytes.fromhex("03fa4715010000000000000000000000000000000000000000")  # 25B
# native route declarations on chan11, sent ~60ms AFTER the 9B answer
# (sessioncap s0019-s0022; route ids are per-session — base 0x14 today,
#  0x6c in dummycap2 — server is not known to validate them)
ROUTES = [
    bytes.fromhex("a60101000114000000140000000000"),
    bytes.fromhex("a60201000118000000180000000000040400"),
    bytes.fromhex("a6030100011c0000001c0000000000040400040400"),
    bytes.fromhex("a60401000120000000200000000000040400040400040400"),
]


class Dummy:
    """app=6 'DummyClient' connection — the native always opens a second QUIC
    connection with the SAME RUPP token right after joining. Mirrors the TX
    order captured from the native (dummycap2): ctrl hello, openU c2 wire2,
    openU c3 wire4, chan1 ping 77B/1s, chan3 sync 25B/5s."""

    def __init__(self, js, log, mode="full"):
        import base64
        self.js = js
        self.log = log
        self.mode = mode
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
        self.send_ctrl(bytes([2, APP6]) + struct.pack(">I", 2) + struct.pack(">I", 2), "openU c2")
        self.send_ctrl(bytes([2, APP6]) + struct.pack(">I", 3) + struct.pack(">I", 4), "openU c3")
        self.c1 = self.open_stream(APP6, 1)
        self.c3 = self.open_stream(APP6, 3)
        # native sends both immediately, then loops (dummycap2 idx 0013/0014)
        self.send_framed(self.c3, SYNC_TMPL, "sync#0")
        self.send_framed(self.c1, PING_TMPL, "ping#0")
        if self.mode == "full":
            asyncio.ensure_future(self.ping_loop())
            asyncio.ensure_future(self.sync_loop())

    async def ping_loop(self):
        n = 0
        while self.live:
            n += 1
            try:
                self.send_framed(self.c1, PING_TMPL, f"ping#{n}")
            except Exception as e:
                self.log(f"  DUMMY ping err {e}")
                return
            await asyncio.sleep(1.0)

    async def sync_loop(self):
        n = 0
        while self.live:
            n += 1
            try:
                self.send_framed(self.c3, SYNC_TMPL, f"sync#{n}")
            except Exception as e:
                self.log(f"  DUMMY sync err {e}")
                return
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


class P10:
    def __init__(self, js, reply, log=print, source="local"):
        self.js = js
        self.reply = reply

        import time as _t
        def _flog(*a, **k):
            k["flush"] = True
            log(f"[{_t.monotonic():.3f}]", *a, **k)
        self.log = _flog
        cfg = json.loads(__import__("urllib.parse", fromlist=["unquote"]).unquote(js["NetStackConfig"]))
        self.cfg = cfg
        self.udmux_ip = js["UdmuxEndpoints"][0]["Address"]
        self.rcc_ip = js["ServerConnections"][0]["Address"]
        self.port = int(js["NetStackPort"])
        self.token = __import__("base64").b64decode(js["NetStackTokenValue"])
        self.conn = None
        self.live = False
        self.term = False
        self.streams = {}
        self.chan1_rx = b""
        self.challenge = None
        self.answered = False
        self.source = source
        self.job = js.get("GameId")
        self.reset = []
        self.peer_assigned = False

    # --- aioquic pump ---
    def events(self):
        while True:
            ev = self.conn.next_event()
            if ev is None:
                break
            self.on_event(ev)

    def on_event(self, ev):
        from aioquic.quic.events import (
            HandshakeCompleted, ProtocolNegotiated, StreamDataReceived,
            StreamReset, DatagramFrameReceived, ConnectionTerminated)
        if isinstance(ev, HandshakeCompleted):
            self.live = True
            self.log("*** QUIC handshake complete ***")
            self.on_connected()
        elif isinstance(ev, StreamDataReceived):
            self.on_stream(ev)
        elif isinstance(ev, StreamReset):
            self.reset.append(ev.stream_id)
            self.log(f"  <<< StreamReset {ev.stream_id} err={ev.error_code}")
        elif isinstance(ev, ConnectionTerminated):
            self.log(f"*** terminated: {ev.error_code} {ev.reason_phrase!r}")
            self.term = True
            self.live = False
        elif isinstance(ev, DatagramFrameReceived):
            self.log(f"  <<< DGRAM {len(ev.data)}B: {ev.data[:48].hex()}")

    def open_stream(self, app, chan, label=""):
        sid = self.conn.get_next_available_stream_id(is_unidirectional=getattr(self, "uni", False))
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
        # native opens ALL four unreliable chans before early-auth
        # (sessioncap idx s0001-s0004, 9.456-9.459)
        for chan, wire in ((3, 2), (5, 4), (6, 6), (11, 8)):
            self.send_framed(ctrl, bytes([2, APP]) + struct.pack(">I", chan) + struct.pack(">I", wire), f"openU c{chan}")
        # native brings the app=6 dummy up right after the game ctrl opens
        # and BEFORE early-auth (sessioncap: dummy 9.464, early-auth 10.187)
        if getattr(self, "dummy_mode", "skip") != "skip":
            d = Dummy(js, self.log, mode=self.dummy_mode)
            self.dummy = d
            asyncio.ensure_future(d.start(asyncio.get_event_loop()))
        ct = js["ClientTicket"]
        version = int(ct.split(";")[-1])
        chan1 = self.open_stream(APP, 1)
        self.chan1 = chan1
        self.send_framed(chan1, early_auth_payload(ct, version), "EARLY-AUTH")
        if getattr(self, "a7_mode", "empty") == "real":
            a7 = load_cap(os.environ.get("RBX_A7", "msg_0006_a4_c1.bin"))
            self.send_framed(chan1, a7, f"A7-real({len(a7)}B)")
        elif getattr(self, "a7_mode", "empty") == "empty":
            self.send_framed(chan1, bytes([0xA7, 0x00, 0x00]), "A7-empty")
        # "skip": send no A7 at all (native does this too — acct2cap)
        self.send_framed(chan1, build_90(js, self.reply), "90")
        self.send_framed(chan1, build_92(), "92")
        self.send_framed(chan1, build_8a(js), "8A")
        self.send_framed(chan1, b"\x8f\x00", "8F")
        asyncio.ensure_future(self.keepalive())
        if self.source == "oracle":
            asyncio.ensure_future(self.watch_answer())

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
            if self.answered and not self.peer_assigned:
                self.peer_assigned = True
                self.log("  *** POST-ANSWER TRAFFIC on chan1 — server kept the session "
                         "(peer assignment / replication)")
            self.try_challenge()
        elif len(data) > 0 and st["app"] in (APP, 0):
            self.log(f"      <<< RX app={st['app']} chan={st['chan']} {len(data)}B: {data[:48].hex()}")
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
                self.log(f"     ctrl OpenUnreliable app={app} chan={chan} wire={wire}")

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
                blen = struct.unpack("<I", msg[9:13])[0]
                blob = msg[13:13 + blen]
                self.challenge = (u1, u2, blob)
                with open(CHAL_PATH, "wb") as f:
                    f.write(msg)
                self.log(f"  *** CHALLENGE u1=0x{u1:08x} u2=0x{u2:08x} blob={len(blob)}B -> {CHAL_PATH}")
                if self.source == "local":
                    asyncio.ensure_future(self.solve_local(bytes(msg)))
                return

    def ensure_chan1(self):
        """Return a usable chan1 stream id; open a fresh one if the old was reset."""
        if self.chan1 and self.chan1 not in self.reset:
            return self.chan1
        sid = self.open_stream(APP, 1, "(chan1-reopen)")
        self.chan1 = sid
        self.log(f"  -> re-opened chan1 as stream {sid} after reset")
        return sid

    async def solve_local(self, msg):
        """Compute the answer from the challenge blob entirely locally."""
        loop = asyncio.get_event_loop()
        t0 = time.monotonic()
        try:
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            from solve9b import solve_message
            ans = await loop.run_in_executor(
                None, lambda: solve_message(msg, self.job))
            dt = time.monotonic() - t0
            resp = bytes([0x9B]) + struct.pack("<II", self.challenge[1], ans)
            if getattr(self, "corrupt", False):
                ans ^= 0xDEADBEEF
                resp = bytes([0x9B]) + struct.pack("<II", self.challenge[1], ans)
                self.log(f"  -> CORRUPTED ANSWER {ans:#010x} (control test), sending")
            else:
                self.log(f"  -> LOCAL ANSWER {ans:#010x} (solved in {dt:.3f}s), sending")
            sid = self.ensure_chan1()
            self.send_framed(sid, resp, "9B-LOCAL")
            self.answered = True
            if getattr(self, "routes_mode", "on") == "on":
                asyncio.ensure_future(self.routes_after_answer())
        except Exception as e:
            self.log(f"  LOCAL SOLVE FAILED: {e!r}")

    async def routes_after_answer(self):
        """mirror the native: declare reliability routes on chan11 ~60ms
        after the answer (sessioncap s0019-s0022 at +66ms)."""
        await asyncio.sleep(0.06)
        try:
            sid = self.open_stream(APP, 11)
            for rb in ROUTES:
                self.send_framed(sid, rb, f"route({len(rb)}B)")
        except Exception as e:
            self.log(f"  route send failed: {e!r}")

    async def watch_answer(self):
        """oracle mode only (--source oracle): poll run/oracle_answer.txt,
        which the gdb oracle writes after computing the native's answer."""
        deadline = time.monotonic() + 600
        while time.monotonic() < deadline and self.live:
            if self.challenge and not self.answered and os.path.exists(ANS_PATH):
                try:
                    raw = open(ANS_PATH).read().strip()
                    if raw:
                        os.remove(ANS_PATH)
                        resp = bytes.fromhex(raw)
                        if len(resp) == 9 and resp[0] == 0x9B:
                            sid = self.ensure_chan1()
                            self.log(f"  -> ORACLE ANSWER send: {resp.hex()}")
                            self.send_framed(sid, resp, "9B-ORACLE")
                            self.answered = True
                except Exception as e:
                    self.log(f"  answer-file err: {e}")
            await asyncio.sleep(0.001)

    async def repl_watch(self):
        pass


async def run(args):
    cookie = open(os.environ.get("RBX_COOKIE_FILE", os.path.join(ROOT, "run/cookie.txt"))).read().strip()
    js, reply = join_game(args.place, None, cookie, args.job, args.follow)
    print(f"joined job={js.get('GameId')} udmux={js['UdmuxEndpoints'][0]['Address']}:{js['NetStackPort']}", flush=True)
    if args.wait_go:
        # write job info for the orchestrator, then wait before opening the game connection
        with open(args.job_file, "w") as f:
            json.dump({"job": js.get("GameId"), "udmux": js["UdmuxEndpoints"][0], "port": js["NetStackPort"]}, f)
        print(f"prejoined: job written to {args.job_file}; waiting for {args.wait_go}", flush=True)
        deadline = time.time() + (args.go_timeout or 600)
        while not os.path.exists(args.wait_go):
            if time.time() > deadline:
                print("go signal timeout; exiting", flush=True)
                return
            await asyncio.sleep(0.02)
        print("go signal received; connecting now", flush=True)
    for p in (CHAL_PATH, ANS_PATH):
        if os.path.exists(p):
            os.remove(p)
    p = P10(js, reply, source=args.source)
    p.uni = args.uni
    p.a7_mode = args.a7
    p.dummy_mode = args.dummy
    p.routes_mode = args.routes
    p.dummy = None
    loop = asyncio.get_event_loop()
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
    print(f"challenge={'yes' if p.challenge else 'no'} answered={p.answered} "
          f"peer={p.peer_assigned} resets={p.reset} chan1_rx={len(p.chan1_rx)} "
          f"dummy_rx={p.dummy.rx if p.dummy else '-'}")
    try:
        conn.close(); proto.flush()
    except Exception:
        pass
    transport.close()
    if p.dummy:
        p.dummy.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=120)
    ap.add_argument("--place", type=int, default=1818)
    ap.add_argument("--job", default=None)
    ap.add_argument("--follow", default=None)
    ap.add_argument("--wait-go", default=None)
    ap.add_argument("--go-timeout", type=int, default=600)
    ap.add_argument("--job-file", default=os.path.join(ROOT, "run/prejoin.json"))
    ap.add_argument("--uni", action="store_true")
    ap.add_argument("--a7", default="empty", choices=["empty", "real", "skip"])
    ap.add_argument("--dummy", default="skip", choices=["skip", "basic", "full"],
                    help="open a paired app=6 dummy connection like the native")
    ap.add_argument("--routes", default="on", choices=["on", "off"],
                    help="send chan11 route declarations after the answer (native does)")
    ap.add_argument("--corrupt", action="store_true",
                    help="control: flip the answer bits to verify kick semantics")
    ap.add_argument("--source", default="local", choices=["local", "oracle"])
    args = ap.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
