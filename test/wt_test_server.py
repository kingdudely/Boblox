#!/usr/bin/env python3
"""
Local WebTransport game-server simulator for testing the Roblox In Browser
extension's RbxTransport client.

Implements the protocol subset we reconstructed from the Roblox binaries:

  * WebTransport-over-HTTP/3 (extended CONNECT, :protocol=webtransport)
  * Capability check: reads X-Rbx-Capabilities from the request header or the
    query string (matching RtcIoServerPrefixProcessor / sub_63B580C behavior)
  * Reliable stream header  [0x06][0x01][appIndex:u8][channelId:u32 BE]
  * QUIC varint message framing
  * Control channel (app index 0, channel 0) messages:
      OpenReliableChannelControl   [1][app][chan][streamId u32]
      OpenUnreliableChannelControl [2][app][chan][streamId u32][wireId u32]
      CloseUnreliableChannelControl[3][reason][wireId varint][chan varint]
  * Datagrams: svarint(channelId) varint(sendId) varint(fragSize) [varint frag] payload

The server:
  * validates the caps query param and echoes a capability mask in the
    X-Rbx-Capabilities *response header*
  * on ctrl channel open, replies with a synthetic stream (app 1 = replicator)
    carrying a few framed messages, plus a couple of datagrams, so the client
    has every packet type exercised.
"""
import argparse
import asyncio
import logging
import struct
import sys
from email.utils import formatdate

from aioquic.asyncio import QuicConnectionProtocol, serve
from aioquic.h3.connection import H3Connection
from aioquic.h3.events import (
    DatagramReceived,
    HeadersReceived,
    WebTransportStreamDataReceived,
    DataReceived,
)
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import ProtocolNegotiated
from aioquic.tls import SessionTicket

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
LOG = logging.getLogger("wt-test-server")

H3_ALPN = ["h3"]

# ---------------------------------------------------------------------------
# RbxTransport codecs (server side)
# ---------------------------------------------------------------------------

def write_varint(value: int) -> bytes:
    if value < 0:
        raise ValueError("varint must be non-negative")
    if value <= 0x3F:
        return bytes([value])
    if value <= 0x3FFF:
        return struct.pack(">H", value | 0x4000)
    if value <= 0x3FFFFFFF:
        return struct.pack(">I", value | 0x80000000)
    if value <= 0x3FFFFFFFFFFFFFFF:
        return struct.pack(">Q", value | 0xC000000000000000)
    raise ValueError("varint too large")


def read_varint(data: bytes, offset: int = 0):
    first = data[offset]
    length = 1 << (first >> 6)
    value = first & 0x3F
    for i in range(1, length):
        value = (value << 8) | data[offset + i]
    return value, offset + length


def write_svarint(value: int) -> bytes:
    zz = (value << 1) if value >= 0 else ((-value << 1) - 1)
    return write_varint(zz)


def frame(payload: bytes) -> bytes:
    return write_varint(len(payload)) + payload


def stream_header(app_index: int, channel_id: int) -> bytes:
    return bytes([0x06, 0x01, app_index]) + struct.pack(">I", channel_id)


def datagram(channel_id: int, send_id: int, payload: bytes) -> bytes:
    return write_svarint(channel_id) + write_varint(send_id) + write_varint(0) + payload


# ctrl message types (mirrors protocol.js)
CTRL_OPEN_RELIABLE = 1
CTRL_OPEN_UNRELIABLE = 2
CTRL_CLOSE_UNRELIABLE = 3

CTRL_NAMES = {
    1: "OpenReliableChannelControl",
    2: "OpenUnreliableChannelControl",
    3: "CloseUnreliableChannelControl",
}


def decode_ctrl(payload: bytes):
    t = payload[0]
    name = CTRL_NAMES.get(t, f"Unknown({t})")
    if t == CTRL_OPEN_RELIABLE and len(payload) >= 7:
        return {"type": t, "name": name, "app": payload[1], "chan": payload[2],
                "streamId": struct.unpack(">I", payload[3:7])[0]}
    if t == CTRL_OPEN_UNRELIABLE and len(payload) >= 11:
        return {"type": t, "name": name, "app": payload[1], "chan": payload[2],
                "streamId": struct.unpack(">I", payload[3:7])[0],
                "wireId": struct.unpack(">I", payload[7:11])[0]}
    return {"type": t, "name": name, "raw": payload.hex()}


class ServerSession:
    """Per-WebTransport-session state."""

    def __init__(self, session_id: int, h3: H3Connection, stream_id: int, caps: str):
        self.session_id = session_id
        self.h3 = h3
        self.stream_id = stream_id
        self.caps = caps
        self.ctrl_stream = None
        self.streams = {}       # stream_id -> {"buffer": bytes, "app": int, "chan": int}
        self.unreliable = {}    # wire id -> channel meta
        self.send_id = 0

    # -- helpers ------------------------------------------------------------
    def send_stream(self, stream_id: int, payload: bytes):
        self.h3.send_data(stream_id, frame(payload), end_stream=False)

    def send_datagram(self, channel_id: int, payload: bytes):
        pkt = datagram(channel_id, self.send_id, payload)
        self.send_id += 1
        self.h3.send_datagram(self.stream_id, pkt)

    # -- protocol logic -----------------------------------------------------
    def on_ctrl_stream(self, stream_id: int):
        self.ctrl_stream = stream_id
        LOG.info("[session %d] ctrl stream %d opened", self.session_id, stream_id)

    def on_ctrl_message(self, payload: bytes):
        msg = decode_ctrl(payload)
        LOG.info("[session %d] ctrl msg: %s", self.session_id, msg)
        if msg["type"] == CTRL_OPEN_RELIABLE:
            app, chan = msg["app"], msg["chan"]
            if app == 0 and chan == 0:
                pass  # ctrl itself
            else:
                # Accept the channel: create a server-initiated reliable stream and
                # announce it via ctrl, then push sample framed messages.
                sid = self.h3.create_webtransport_stream(self.stream_id, is_unidirectional=False)
                self.streams[sid] = {"buffer": b"", "app": app, "chan": chan}
                self.h3.send_data(sid, stream_header(app, chan), end_stream=False)
                ack = bytes([CTRL_OPEN_RELIABLE, app, chan]) + struct.pack(">I", sid)
                self.h3.send_data(self.ctrl_stream, frame(ack), end_stream=False)
                LOG.info("[session %d] accepted reliable app=%d chan=%d on stream %d",
                         self.session_id, app, chan, sid)
                # push some sample data packets
                for i in range(3):
                    self.send_stream(sid, bytes([0xAA, i, 0x00]) + f"hello #{i}".encode())
        elif msg["type"] == CTRL_OPEN_UNRELIABLE:
            wire = msg.get("wireId", 0)
            self.unreliable[wire] = {"app": msg["app"], "chan": msg["chan"]}
            ack = bytes([CTRL_OPEN_UNRELIABLE, msg["app"], msg["chan"]]) + \
                  struct.pack(">I", 0) + struct.pack(">I", wire)
            self.h3.send_data(self.ctrl_stream, frame(ack), end_stream=False)
            LOG.info("[session %d] accepted unreliable app=%d chan=%d wire=%d",
                     self.session_id, msg["app"], msg["chan"], wire)
            for i in range(2):
                self.send_datagram(msg["chan"], bytes([0xBB, i]) + f"dg #{i}".encode())
        elif msg["type"] == CTRL_CLOSE_UNRELIABLE:
            LOG.info("[session %d] close unreliable: %s", self.session_id, msg)

    def on_new_stream(self, stream_id: int):
        self.streams[stream_id] = {"buffer": b"", "app": None, "chan": None}
        LOG.info("[session %d] new stream %d", self.session_id, stream_id)

    def on_stream_data(self, stream_id: int, data: bytes, end_stream: bool):
        st = self.streams.setdefault(stream_id, {"buffer": b"", "app": None, "chan": None})
        st["buffer"] += data
        if st["app"] is None and len(st["buffer"]) >= 7:
            magic, kind, app = st["buffer"][0], st["buffer"][1], st["buffer"][2]
            chan = struct.unpack(">I", st["buffer"][3:7])[0]
            st["app"], st["chan"] = app, chan
            st["buffer"] = st["buffer"][7:]
            LOG.info("[session %d] stream %d header: magic=%#x kind=%d app=%d chan=%d",
                     self.session_id, stream_id, magic, kind, app, chan)
            if app == 0 and chan == 0:
                self.on_ctrl_stream(stream_id)
        # varint-framed messages
        if stream_id == self.ctrl_stream or (st["app"] == 0 and st["chan"] == 0):
            while True:
                try:
                    length, off = read_varint(st["buffer"])
                except (IndexError, ValueError):
                    break
                if len(st["buffer"]) - off < length:
                    break
                payload = st["buffer"][off:off + length]
                st["buffer"] = st["buffer"][off + length:]
                self.on_ctrl_message(payload)
        else:
            LOG.info("[session %d] stream %d data: %s", self.session_id, stream_id, data[:64].hex())
        if end_stream:
            LOG.info("[session %d] stream %d ended", self.session_id, stream_id)

    def on_datagram(self, data: bytes):
        try:
            chan, off = read_varint(data)
            zz = chan
            chan = (zz >> 1) if (zz & 1) == 0 else -((zz >> 1) + 1)
            send_id, off = read_varint(data, off)
            frag, off = read_varint(data, off)
            LOG.info("[session %d] datagram chan=%d sendId=%d frag=%d payload=%s",
                     self.session_id, chan, send_id, frag, data[off:off + 32].hex())
        except Exception as e:
            LOG.info("[session %d] undecodable datagram: %s (%s)", self.session_id, data.hex(), e)


class WebTransportServerProtocol(QuicConnectionProtocol):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._http = None
        self._sessions = {}
        self._session_seq = 0

    def quic_event_received(self, event):
        if isinstance(event, ProtocolNegotiated):
            LOG.info("negotiated ALPN: %s", event.alpn_protocol)
            self._http = H3Connection(self._quic, enable_webtransport=True)
        if self._http is not None:
            for h3_event in self._http.handle_event(event):
                self._h3_event(h3_event)

    def _h3_event(self, event):
        if isinstance(event, HeadersReceived):
            headers = dict(event.headers)
            method = headers.get(b":method", b"").decode()
            path = headers.get(b":path", b"").decode()
            protocol = headers.get(b":protocol", b"").decode()
            caps = headers.get(b"x-rbx-capabilities", b"").decode()
            if not caps and "X-Rbx-Capabilities=" in path:
                from urllib.parse import urlparse, parse_qs
                q = parse_qs(urlparse(path).query)
                caps = (q.get("X-Rbx-Capabilities") or [""])[0]
            LOG.info("request: %s %s protocol=%s caps=%s", method, path, protocol, caps)
            if method == "CONNECT" and protocol == "webtransport":
                self._session_seq += 1
                sid = self._session_seq
                # reply with 200 + capabilities echo header
                self._http.send_headers(
                    stream_id=event.stream_id,
                    headers=[
                        (b":status", b"200"),
                        (b"x-rbx-capabilities", caps.encode() or b"0000000000000000"),
                    ],
                )
                self._sessions[event.stream_id] = ServerSession(sid, self._http, event.stream_id, caps)
                LOG.info("[session %d] WebTransport session established (stream %d)", sid, event.stream_id)
        elif isinstance(event, WebTransportStreamDataReceived):
            sess = self._sessions.get(event.session_id)
            if sess:
                sess.on_stream_data(event.stream_id, event.data, event.stream_ended)
        elif isinstance(event, DatagramReceived):
            sess = self._sessions.get(event.stream_id)
            if sess:
                sess.on_datagram(event.data)
        elif isinstance(event, DataReceived):
            pass

    def _create_webtransport_stream(self, *a, **k):
        return self._http.create_webtransport_stream(*a, **k)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4433)
    ap.add_argument("--cert", default=None)
    ap.add_argument("--key", default=None)
    args = ap.parse_args()

    if args.cert and args.key:
        cert, key = args.cert, args.key
    else:
        # generate a short-lived self-signed P-256 certificate
        import datetime
        import subprocess
        import tempfile
        import os
        d = tempfile.mkdtemp(prefix="wtcert")
        cert = os.path.join(d, "cert.pem")
        key = os.path.join(d, "key.pem")
        subprocess.check_call([
            "openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
            "-keyout", key, "-out", cert, "-days", "13", "-nodes", "-subj", "/CN=localhost",
            "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
        ], stderr=subprocess.DEVNULL)
        LOG.info("generated test certificate: %s", cert)

    configuration = QuicConfiguration(
        alpn_protocols=H3_ALPN,
        is_client=False,
        max_datagram_frame_size=65536,
    )
    configuration.load_cert_chain(cert, key)

    async def run():
        await serve(args.host, args.port, configuration=configuration,
                    create_protocol=WebTransportServerProtocol)
        LOG.info("listening on %s:%d (WebTransport/h3)", args.host, args.port)
        await asyncio.Future()

    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
