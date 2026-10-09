#!/usr/bin/env python3
"""RbxTransport probe v7 — rebuild 0x90 with our joinTicket + fresh 0x92 nonce.

Root cause found (compare 0x90 between sessions): the only session-bound JSON
fields are RandomSeed1, APIsecurityToken and __joinTicket. We now rebuild the
0x90 JSON from OUR join reply's joinTicket.

0x92 = [0x92] + LEB(zigzag64((v8 << 32) | (v8 ^ 0x63E25F26)))
  v8 = random u32 (client PCG), v9 = 0x63E25F26 = sub_35B024B() constant.

Usage:
  probe7.py --seconds 30
"""
import argparse
import asyncio
import base64
import json
import os
import random
import socket
import ssl
import struct
import sys
import time

sys.path.insert(0, "/home/john/RobloxInBrowser/py")
from rbx_client import join_game, early_auth_payload, RuppTransport, decode_uri

from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.connection import QuicConnection
from aioquic.quic.events import (
    HandshakeCompleted, ProtocolNegotiated,
    StreamDataReceived, StreamReset, DatagramFrameReceived, ConnectionTerminated,
)

sys.path.insert(0, "/home/john/RobloxInBrowser/py")
from probe5 import APP, CTRL, build_8a, frame, stream_header, load_cap, compact_varint, leb128

M64 = (1 << 64) - 1
V9 = 0x63E25F26


# ---------------------------------------------------------------------------
# 0x90 rebuild
# ---------------------------------------------------------------------------

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


# ---------------------------------------------------------------------------
# 0x92 fresh nonce
# ---------------------------------------------------------------------------

def build_92():
    v8 = random.getrandbits(32)
    x = ((v8 << 32) | (v8 ^ V9)) & M64
    zz = ((x << 1) & M64) ^ (M64 if (v8 & 0x80000000) else 0)
    return bytes([0x92]) + leb128(zz)


# ---------------------------------------------------------------------------
# client
# ---------------------------------------------------------------------------

class Probe:
    def __init__(self, js, reply, log=print):
        self.js = js
        self.reply = reply
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
        self.rx6 = getattr(self, "rx6", b"")
        self.chan1_rx = b""
        self.sent_9b = False
        self.reset = []

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
            self.log(f"  <<< DGRAM {len(ev.data)}B: {ev.data[:48].hex()}")
        elif isinstance(ev, StreamReset):
            self.reset.append(ev.stream_id)
            self.log(f"  <<< StreamReset {ev.stream_id} err={ev.error_code}")
        elif isinstance(ev, ConnectionTerminated):
            self.log(f"*** terminated: {ev.error_code} {ev.reason_phrase!r}")
            self.term = True
            self.live = False

    def open_stream(self, app, chan, label=""):
        sid = self.conn.get_next_available_stream_id()
        self.conn.send_stream_data(sid, stream_header(app, chan))
        self.streams[sid] = {"app": app, "chan": chan}
        self.log(f"  -> open stream {sid} app={app} chan={chan} {label}")
        return sid

    def send_framed(self, sid, payload, label=""):
        self.conn.send_stream_data(sid, frame(payload))
        self.log(f"  -> send {label} {len(payload)}B")

    def on_connected(self):
        js = self.js
        ctrl = self.open_stream(APP, CTRL, "(ctrl)")
        self.send_framed(ctrl, b"\x00\x00", "ctrl hello")
        for chan, wire in ((3, 2), (5, 4)):
            self.send_framed(ctrl, bytes([2, APP]) + struct.pack(">I", chan) + struct.pack(">I", wire), f"ctrl openU c{chan}")

        ct = js["ClientTicket"]
        version = int(ct.split(";")[-1])
        chan1 = self.open_stream(APP, 1, "(chan1)")
        self.chan1 = chan1
        self.send_framed(chan1, early_auth_payload(ct, version), "EARLY-AUTH")

        for chan, wire in ((6, 6), (11, 8)):
            self.send_framed(ctrl, bytes([2, APP]) + struct.pack(">I", chan) + struct.pack(">I", wire), f"ctrl openU c{chan}")

        self.send_framed(chan1, load_cap("msg_0006_a4_c1.bin"), "A7")
        m90 = build_90(js, self.reply)
        self.send_framed(chan1, m90, f"90-REBUILT({len(m90)}B)")
        m92 = build_92()
        self.send_framed(chan1, m92, f"92-FRESH({m92.hex()})")
        m8a = build_8a(js)
        self.send_framed(chan1, m8a, f"8A-REBUILT({len(m8a)}B)")
        self.send_framed(chan1, b"\x8f\x00", "8F")

        asyncio.ensure_future(self.keepalive())

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
            self.log(f"  <<< stream {ev.stream_id} raw {len(ev.data)}B: {data[:64].hex()}")
            return
        if len(data) > 0:
            self.log(f"      <<< RX app={st['app']} chan={st['chan']} {len(data)}B: {data[:96].hex()}")
            if st["app"] == APP and st["chan"] == 1:
                self.chan1_rx += data
                with open("/home/john/RobloxInBrowser/run/rx7_app4_chan1.bin", "ab") as f:
                    f.write(data)
        if st["app"] in (APP, 0):
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
            else:
                self.log(f"     ctrl {len(body)}B: {body[:24].hex()}")


async def run(args):
    cookie = open("/home/john/RobloxInBrowser/run/cookie.txt").read().strip()
    js, reply = join_game(args.place, None, cookie, None)
    print(f"joined job={js.get('GameId')} udmux={js['UdmuxEndpoints'][0]['Address']}:{js['NetStackPort']}")
    p = Probe(js, reply)
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
    print(f"resets={p.reset} chan1_rx_total={len(p.chan1_rx)}")
    try:
        conn.close()
        proto.flush()
    except Exception:
        pass
    transport.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--place", type=int, default=1818)
    ap.add_argument("--seconds", type=int, default=30)
    args = ap.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
