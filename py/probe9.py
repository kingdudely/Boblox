#!/usr/bin/env python3
"""probe9 — full join + 0x9B challenge auto-answer.

Sends the full native join sequence with an EMPTY A7 (no script hashes), waits
for the server's 0x9B challenge, parses [u32#1][u32#2][blob], and replies
[0x9B][u32#1][answer] with a configurable answer strategy.

Usage: probe9.py --seconds 30 [--answer zero|echo2|len|blobhash]
"""
import argparse, asyncio, hashlib, json, os, random, socket, ssl, struct, sys, time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rbx_client import join_game, early_auth_payload, RuppTransport
import probe5, probe7
from probe5 import APP, CTRL, build_8a, frame, stream_header, load_cap, compact_varint, leb128, ticket_v31
from probe7 import build_90, build_92

from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.connection import QuicConnection

M32 = 0xFFFFFFFF


class P9(probe7.Probe):
    answer_mode = "zero"

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

        # EMPTY A7 (no records) -> server sends the 0x9B challenge
        self.send_framed(chan1, bytes([0xA7, 0x00, 0x00]), "A7-empty")
        self.send_framed(chan1, build_90(js, self.reply), "90-REBUILT")
        self.send_framed(chan1, build_92(), "92-FRESH")
        self.send_framed(chan1, build_8a(js), "8A-REBUILT")
        self.send_framed(chan1, b"\x8f\x00", "8F")

        self.challenge = None
        self.answered = False
        asyncio.ensure_future(self.keepalive())

    def on_stream(self, ev):
        super().on_stream(ev)
        # after processing, scan accumulated chan1 bytes for a complete 0x9B frame
        self.try_parse_challenge()

    def try_parse_challenge(self):
        if self.answered:
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
                self.handle_challenge(msg)
                return

    def handle_challenge(self, msg):
        u1 = struct.unpack("<I", msg[1:5])[0]
        u2 = struct.unpack("<I", msg[5:9])[0]
        blen = struct.unpack("<I", msg[9:13])[0]
        blob = msg[13:13 + blen]
        self.challenge = (u1, u2, blob)
        self.log(f"  *** 0x9B CHALLENGE: u32#1=0x{u1:08x} u32#2=0x{u2:08x} blob={len(blob)}B ***")
        # correlate with our own ticket-derived values
        try:
            ct = self.js["ClientTicket"].encode()
            v31 = ticket_v31(ct)
            v7 = None
            from probe5 import xxh32
            v7 = xxh32(ct, 1)
            self.log(f"      ticket: v7=0x{v7:08x} v31=0x{v31:08x}")
            self.log(f"      rel: u1^v7=0x{u1 ^ v7:08x} u1^v31=0x{u1 ^ v31:08x} u2^v7=0x{u2 ^ v7:08x} u2^v31=0x{u2 ^ v31:08x}")
            with open(os.path.join(ROOT, "run/p9_corr.log"), "a") as f:
                f.write(f"TICKET u1=0x{u1:08x} u2=0x{u2:08x} v7=0x{v7:08x} v31=0x{v31:08x}\n")
        except Exception as e:
            self.log(f"      corr error: {e}")
        with open(os.path.join(ROOT, "run/p9_challenge.bin"), "wb") as f:
            f.write(msg)
        with open(os.path.join(ROOT, "run/p9_blob.bin"), "wb") as f:
            f.write(blob)
        ans = self.compute_answer(u1, u2, blob)
        resp = bytes([0x9B]) + struct.pack("<I", u2) + struct.pack("<I", ans & M32)
        self.log(f"  -> 0x9B ANSWER ({self.answer_mode}): 0x{ans & M32:08x} resp={resp.hex()}")
        self.send_framed(self.chan1, resp, "9B-ANSWER")
        self.answered = True

    def compute_answer(self, u1, u2, blob):
        mode = self.answer_mode
        if mode == "zero":
            return 0
        if mode == "echo2":
            return u2
        if mode == "len":
            return len(blob)
        if mode == "blobhash":
            return int.from_bytes(hashlib.sha256(blob).digest()[:4], "little")
        if mode == "md5_1_2":
            return int.from_bytes(hashlib.md5(struct.pack("<II", u1, u2)).digest()[:4], "little")
        return 0


async def run(args):
    cookie = open(os.environ.get("RBX_COOKIE_FILE", os.path.join(ROOT, "run/cookie.txt"))).read().strip()
    js, reply = join_game(args.place, None, cookie, None)
    print(f"joined job={js.get('GameId')} udmux={js['UdmuxEndpoints'][0]['Address']}:{js['NetStackPort']}")
    p = P9(js, reply)
    p.answer_mode = args.answer
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
    ch = "yes" if p.challenge else "no"
    print(f"challenge={ch} answered={p.answered} resets={p.reset} chan1_rx={len(p.chan1_rx)}")
    try:
        conn.close(); proto.flush()
    except Exception:
        pass
    transport.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--answer", default="zero", choices=["zero", "echo2", "len", "blobhash", "md5_1_2"])
    ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--place", type=int, default=1818)
    args = ap.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
