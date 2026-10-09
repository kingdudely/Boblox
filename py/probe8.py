#!/usr/bin/env python3
"""probe8 — variant tester for the server 0x9B challenge gate.

Variants (--variant):
  full      A7(stale) + 90(rebuilt) + 92(fresh) + 8A(rebuilt) + 8F      [= probe7]
  a7-empty  A7 with zero records
  no-a7     no A7
  no-92     no 92
  no-8f     no 8F
  order-90  send 90 first, then A7
"""
import argparse, asyncio, sys
sys.path.insert(0, "/home/john/RobloxInBrowser/py")
import probe7
from probe7 import Probe, build_90, build_92, load_cap, APP, CTRL, build_8a, frame, stream_header
from rbx_client import join_game, early_auth_payload, RuppTransport
import json, base64, struct, ssl, time
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.connection import QuicConnection


class P8(Probe):
    variant = "full"

    def on_stream(self, ev):
        # log every app=4 chan=1 chunk separately
        st = self.streams.get(ev.stream_id)
        if st is None and len(ev.data) >= 7 and ev.data[0] == 6 and ev.data[1] == 1:
            app = ev.data[2]
            chan = struct.unpack(">I", ev.data[3:7])[0]
            if app == APP and chan == 1:
                with open("/home/john/RobloxInBrowser/run/rx8_chunks.log", "a") as f:
                    f.write(f"NEWSTREAM sid={ev.stream_id} hdr={ev.data[:7].hex()}\n")
        if st and st.get("app") == APP and st.get("chan") == 1 and ev.data:
            with open("/home/john/RobloxInBrowser/run/rx8_chunks.log", "a") as f:
                f.write(f"sid={ev.stream_id} len={len(ev.data)} head={ev.data[:20].hex()}\n")
        super().on_stream(ev)

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

        v = self.variant
        a7 = load_cap("msg_0006_a4_c1.bin")
        if v == "a7-empty":
            a7 = bytes([0xA7, 0x00, 0x00])
        m90 = build_90(js, self.reply)
        m92 = build_92()
        m8a = build_8a(js)
        m8f = b"\x8f\x00"

        seq = []
        if v == "no-a7":
            seq = [("90", m90), ("92", m92), ("8A", m8a), ("8F", m8f)]
        elif v == "no-92":
            seq = [("A7", a7), ("90", m90), ("8A", m8a), ("8F", m8f)]
        elif v == "no-8f":
            seq = [("A7", a7), ("90", m90), ("92", m92), ("8A", m8a)]
        elif v == "order-90":
            seq = [("90", m90), ("A7", a7), ("92", m92), ("8A", m8a), ("8F", m8f)]
        else:
            seq = [("A7", a7), ("90", m90), ("92", m92), ("8A", m8a), ("8F", m8f)]
        for label, msg in seq:
            self.send_framed(chan1, msg, label)

        asyncio.ensure_future(self.keepalive())


async def run(args):
    cookie = open("/home/john/RobloxInBrowser/run/cookie.txt").read().strip()
    js, reply = join_game(args.place, None, cookie, None)
    print(f"joined job={js.get('GameId')} udmux={js['UdmuxEndpoints'][0]['Address']}:{js['NetStackPort']}")
    p = P8(js, reply)
    p.variant = args.variant
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
    print(f"variant={args.variant} resets={p.reset} chan1_rx={len(p.chan1_rx)}")
    try:
        conn.close(); proto.flush()
    except Exception:
        pass
    transport.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", default="full")
    ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--place", type=int, default=1818)
    args = ap.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
