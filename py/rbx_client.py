#!/usr/bin/env python3
"""
RbxTransport Python client — connects to a live Roblox game server.

Protocol (verified live 2026-10-06):
  1. HTTPS join via gamejoin.roblox.com (cookie auth, v2 SSE or v1 JSON)
  2. UDP QUIC to  udmuxIp:NetStackPort
     - ALPN: "RbxTransport"
     - RUPP prefix on every datagram:
         [01][00][u16 BE len]
           TLV1: 01 11 01 <NetStackTokenValue 16B>   (subtype 1!)
           TLV2: 02 06 <rccIp 4B><NetStackPort u16 BE>
         [QUIC packet]
  3. BaseClient early-auth on app=1 channel 0 (ClientTicket fields)
  4. OpenReliable ctrl on app=0 channel 0
  5. Server opens game channels (app=6) toward us; we ACK by ... (WIP)

Usage:
  python rbx_client.py --cookie-file cookie.txt --place 1818
  python rbx_client.py --job <json-with-join-fields>
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
import urllib.request
import uuid

from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.connection import QuicConnection
from aioquic.quic.events import (
    HandshakeCompleted, ConnectionTerminated, ProtocolNegotiated,
    StreamDataReceived, StreamReset, DatagramFrameReceived,
)

UA = "Roblox/WinInet"
GAMEJOIN_V1 = "https://gamejoin.roblox.com/v1/join-game"
GAMEJOIN_V2 = "https://gamejoin.roblox.com/v2/join-game"

# ---------------------------------------------------------------------------
# join (cookie-based)
# ---------------------------------------------------------------------------

def parse_sse(text):
    records = []
    cur = {"event": "message", "data": []}
    for raw in text.split("\n"):
        line = raw.rstrip("\r")
        if line == "":
            if cur["data"] or cur["event"] != "message":
                records.append(cur)
            cur = {"event": "message", "data": []}
            continue
        if line.startswith(":"):
            continue
        idx = line.find(":")
        field = line if idx == -1 else line[:idx]
        value = "" if idx == -1 else line[idx + 1:].lstrip(" ")
        if field == "event":
            cur["event"] = value
        elif field == "data":
            cur["data"].append(value)
    if cur["data"] or cur["event"] != "message":
        records.append(cur)
    return records


def _http(url, data=None, cookie="", extra=None):
    req = urllib.request.Request(url, data=data)
    req.add_header("User-Agent", UA)
    if cookie:
        req.add_header("Cookie", cookie if cookie.startswith(".ROBLOSECURITY=") or "=" in cookie.split(";")[0] else ".ROBLOSECURITY=" + cookie)
    if data is not None:
        req.add_header("Content-Type", "application/json; charset=utf-8")
    for k, v in (extra or {}).items():
        req.add_header(k, v)
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.status, r.read()


def join_game(place_id, universe_id=None, cookie="", job_id=None, follow_user=None):
    body = {
        "placeId": int(place_id),
        "isTeleport": False,
        "gameJoinAttemptId": str(uuid.uuid4()),
        "browserTrackerId": 0,
        "playSessionId": str(uuid.uuid4()),
        "eventId": str(uuid.uuid4()),
        "launchData": "",
        "joinAttemptOrigin": "PlayButton",
    }
    if universe_id:
        body["gameId"] = int(universe_id)
    if job_id:
        body["jobId"] = job_id
    if follow_user:
        body["followUserId"] = int(follow_user)
    payload = json.dumps(body).encode()

    # v2 SSE first (inline joinScript), v1 fallback
    try:
        st, raw = _http(GAMEJOIN_V2, payload, cookie, {"Accept": "text/event-stream"})
        events = parse_sse(raw.decode("utf-8", "replace"))
        for ev in events:
            if ev["event"] == "ResponseReady" and ev["data"]:
                reply = json.loads("\n".join(ev["data"]))
                if reply.get("joinScript"):
                    return reply["joinScript"], reply
    except Exception:
        pass

    st, raw = _http(GAMEJOIN_V1, payload, cookie)
    reply = json.loads(raw)
    if reply.get("joinScript"):
        return reply["joinScript"], reply
    # v1 sometimes only returns joinScriptUrl
    if reply.get("status") == 2 and reply.get("joinScriptUrl"):
        st2, raw2 = _http(reply["joinScriptUrl"], None, cookie)
        js = json.loads(raw2)
        if js.get("joinScript"):
            return js["joinScript"], reply
        return js, reply
    raise RuntimeError(f"join failed: status={reply.get('status')} message={reply.get('message')}")


# ---------------------------------------------------------------------------
# RUPP + QUIC
# ---------------------------------------------------------------------------

class RuppTransport(asyncio.DatagramProtocol):
    """UDP socket with RUPP prefix add/strip, feeding an aioquic connection."""

    def __init__(self, conn, rcc_ip, rcc_port, token, log):
        self.conn = conn
        self.rcc_ip = rcc_ip
        self.rcc_port = rcc_port
        self.token = token
        self.log = log
        self.transport = None
        self.peer = None
        self._timer_handle = None

    # -- datagram wrapping --
    def _wrap(self, payload):
        tlv1 = bytes([1, 17, 1]) + self.token            # subtype 1 = NetStackTokenValue
        tlv2 = bytes([2, 6]) + socket.inet_aton(self.rcc_ip) + struct.pack(">H", self.rcc_port)
        body = tlv1 + tlv2
        return bytes([1, 0]) + struct.pack(">H", 4 + len(body)) + body + payload

    @staticmethod
    def _strip(pkt):
        if len(pkt) < 4 or pkt[0] != 1:
            return None
        total = struct.unpack(">H", pkt[2:4])[0]
        if not (4 <= total <= len(pkt)):
            return None
        return pkt[total:]

    # -- asyncio protocol --
    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        stripped = self._strip(data)
        if stripped is None:
            return
        self.conn.receive_datagram(stripped, self.peer, time.monotonic())
        self._drain()

    def error_received(self, exc):
        pass

    def connection_lost(self, exc):
        pass

    def _drain(self):
        """Process events and send pending QUIC datagrams."""
        handlers = getattr(self, "on_events", None)
        if handlers:
            handlers()
        self.flush()

    def flush(self):
        if self.transport is None or self.peer is None:
            return
        now = time.monotonic()
        for data, _addr in self.conn.datagrams_to_send(now):
            self.transport.sendto(self._wrap(data), self.peer)
        self._arm_timer()

    def _arm_timer(self):
        t = self.conn.get_timer()
        if t is None:
            return
        delay = max(0.0, t - time.monotonic())
        loop = asyncio.get_event_loop()
        if self._timer_handle is not None:
            self._timer_handle.cancel()
        self._timer_handle = loop.call_later(delay, self._on_timer)

    def _on_timer(self):
        self._timer_handle = None
        self.conn.handle_timer(time.monotonic())
        self._drain()


# ---------------------------------------------------------------------------
# RbxTransport framing helpers
# ---------------------------------------------------------------------------

def stream_header(app, chan):
    return bytes([0x06, 0x01, app]) + struct.pack(">I", chan)


def frame(payload):
    n = len(payload)
    if n < 64:
        return bytes([n]) + payload
    if n < 16384:
        return bytes([0x40 | (n >> 8), n & 0xFF]) + payload
    raise ValueError("payload too large for framing helper")


def compact_varint(v):
    """Roblox compact u32 varint (used inside control messages)."""
    out = bytearray()
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def early_auth_payload(client_ticket, version):
    parts = client_ticket.split(";")
    def dec(s):
        if not s:
            return b""
        pad = "=" * (-len(s) % 4)
        try:
            return base64.b64decode(s + pad)
        except Exception:
            return s.encode()
    pre = dec(parts[2]) if len(parts) > 2 else b""
    auth = dec(parts[3]) if len(parts) > 3 else b""
    return bytes([0xA8, version & 0xFF, len(pre) & 0xFF]) + pre + bytes([len(auth) & 0xFF]) + auth


# ---------------------------------------------------------------------------
# main client
# ---------------------------------------------------------------------------

class RbxClient:
    def __init__(self, join_script, log=print):
        self.js = join_script
        self.log = log
        cfg = json.loads(decode_uri(join_script["NetStackConfig"]))
        self.cfg = cfg
        self.udmux_ip = join_script["UdmuxEndpoints"][0]["Address"]
        self.rcc_ip = join_script["ServerConnections"][0]["Address"]
        self.port = int(join_script["NetStackPort"])
        self.token = base64.b64decode(join_script["NetStackTokenValue"])
        self.caps = cfg.get("caps", "0000000000000000")
        self.conn = None
        self.proto = None
        self.streams = {}       # stream_id -> {app, chan}
        self.rx_stream = bytearray()
        self.session_live = False

    def _events(self):
        while True:
            ev = self.conn.next_event()
            if ev is None:
                break
            self._on_event(ev)

    def _on_event(self, ev):
        if isinstance(ev, HandshakeCompleted):
            self.log("*** QUIC handshake complete (ALPN=RbxTransport) ***")
            self.session_live = True
            self._on_connected()
        elif isinstance(ev, ProtocolNegotiated):
            self.log(f"ALPN negotiated: {ev.alpn_protocol}")
        elif isinstance(ev, StreamDataReceived):
            self._on_stream(ev)
        elif isinstance(ev, DatagramFrameReceived):
            self.log(f"  <<< DATAGRAM {len(ev.data)}B: {ev.data[:32].hex()}")
        elif isinstance(ev, StreamReset):
            self.log(f"  <<< StreamReset {ev.stream_id} err={ev.error_code}")
        elif isinstance(ev, ConnectionTerminated):
            self.log(f"*** connection terminated: code={ev.error_code} reason={ev.reason_phrase!r} ***")
            self.session_live = False

    # -- outbound --
    def _open_stream(self, app, chan):
        sid = self.conn.get_next_available_stream_id()
        self.conn.send_stream_data(sid, stream_header(app, chan))
        self.streams[sid] = {"app": app, "chan": chan}
        self.log(f"  -> open stream {sid} app={app} chan={chan}")
        return sid

    def _send_framed(self, sid, payload, label=""):
        self.conn.send_stream_data(sid, frame(payload))
        self.log(f"  -> sent {label or 'payload'} {len(payload)}B on stream {sid}")

    def _on_connected(self):
        # 1. ctrl channel (app 0, chan 0) with OpenReliable for app 1
        ctrl = self._open_stream(0, 0)
        self._send_framed(ctrl, bytes([1, 1, 0, 0, 0, 0]), "ctrl OpenReliable(app=1 chan=0)")

        # 2. early auth (app 1, chan 0)
        ct = self.js.get("ClientTicket", "")
        version = 0
        if ct:
            parts = ct.split(";")
            try:
                version = int(parts[-1])
            except Exception:
                version = 0
        payload = early_auth_payload(ct, version)
        auth_stream = self._open_stream(1, 0)
        self._send_framed(auth_stream, payload, f"early-auth(v{version} {len(payload)}B)")

        # 3. keepalive ping loop (native DummyClient pings at 1s; also keeps QUIC alive)
        self._start_ping_loop()

    def _start_ping_loop(self):
        async def loop():
            while self.session_live:
                try:
                    self.conn.ping(uid=None)
                except Exception:
                    pass
                await asyncio.sleep(1.0)
        self._ping_task = asyncio.ensure_future(loop())

    # -- inbound --
    def _on_stream(self, ev):
        st = self.streams.get(ev.stream_id)
        self.rx_stream.extend(ev.data)
        head = ev.data[:64].hex()
        self.log(f"  <<< stream {ev.stream_id} ({'app=%s chan=%s' % (st['app'], st['chan']) if st else 'server-opened'}) {len(ev.data)}B end={ev.end_stream}: {head}")
        # parse control messages on ctrl stream
        if st and st["app"] == 0 and st["chan"] == 0:
            self._parse_ctrl(ev.data)

    def _parse_ctrl(self, data):
        # frame: [varint len][type u8][app u8][chan u32 be][...]
        try:
            r = Reader(data)
            while r.remaining >= 1:
                ln = r.varint()
                if ln == 0 or ln > r.remaining:
                    break
                body = r.take(ln)
                t = body[0]
                if t in (1, 2) and len(body) >= 6:
                    app = body[1]
                    chan = struct.unpack(">I", body[2:6])[0]
                    extra = body[6:]
                    self.log(f"     ctrl: {'OpenReliable' if t == 1 else 'OpenUnreliable'} app={app} chan={chan} extra={extra.hex()}")
        except Exception:
            pass


class Reader:
    def __init__(self, data):
        self.data = bytes(data)
        self.off = 0

    @property
    def remaining(self):
        return len(self.data) - self.off

    def varint(self):
        v = 0
        shift = 0
        while True:
            b = self.data[self.off]
            self.off += 1
            v |= (b & 0x7F) << shift
            if not (b & 0x80):
                return v
            shift += 7

    def take(self, n):
        out = self.data[self.off:self.off + n]
        self.off += n
        return out


def decode_uri(s):
    import urllib.parse
    return urllib.parse.unquote(s)


async def run(args):
    cookie = ""
    if args.cookie_file:
        cookie = open(args.cookie_file).read().strip()

    if args.join_json:
        print(f"== loading join from {args.join_json} ==")
        js = json.load(open(args.join_json))
        reply = None
    else:
        if not cookie:
            raise SystemExit("provide --cookie-file with a .ROBLOSECURITY value (or --join-json)")
        print("== joining ==")
        js, reply = join_game(args.place, args.universe, cookie, args.job)
    print(f"joined: place={js.get('PlaceId')} job={js.get('GameId')}")

    cb = RbxClient(js, log=print)
    print(f"== connecting udp {cb.udmux_ip}:{cb.port} (rcc {cb.rcc_ip}) ==")
    cfg = QuicConfiguration(is_client=True, alpn_protocols=["RbxTransport"])
    cfg.verify_mode = ssl.CERT_NONE
    cfg.max_datagram_frame_size = 65535      # enable RFC 9221 QUIC DATAGRAMs (unreliable game data)
    cfg.idle_timeout = 30.0
    conn = QuicConnection(configuration=cfg)
    cb.conn = conn

    loop = asyncio.get_event_loop()
    transport, proto = await loop.create_datagram_endpoint(
        lambda: RuppTransport(conn, cb.rcc_ip, cb.port, cb.token, print),
        remote_addr=(cb.udmux_ip, cb.port),
    )
    proto.peer = (cb.udmux_ip, cb.port)
    proto.on_events = cb._events
    cb.proto = proto

    conn.connect(proto.peer, time.monotonic())
    proto.flush()

    deadline = time.monotonic() + (args.seconds or 60)
    while time.monotonic() < deadline:
        await asyncio.sleep(0.5)
        if not cb.session_live and time.monotonic() > deadline - (args.seconds or 60) + 5:
            pass
    print("== session ending ==")
    try:
        conn.close()
        proto.flush()
    except Exception:
        pass
    transport.close()


def main():
    ap = argparse.ArgumentParser(description="Roblox RbxTransport client (Python)")
    ap.add_argument("--cookie-file", help="file containing .ROBLOSECURITY (raw value or header)")
    ap.add_argument("--join-json", help="instead of joining, load a captured join reply (full joinScript JSON)")
    ap.add_argument("--place", type=int, default=1818)
    ap.add_argument("--universe", type=int, default=None)
    ap.add_argument("--job", default=None, help="join a specific jobId")
    ap.add_argument("--seconds", type=int, default=60)
    args = ap.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
