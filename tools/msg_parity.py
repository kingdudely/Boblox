#!/usr/bin/env python3
"""Byte-compare C++ session messages against the Python probe5/probe7 builders.

  ./build/client/rbxplay --join-json run/join_full.json --dump-msgs run/cpp_msgs
  python3 tools/msg_parity.py run/join_full.json run/cpp_msgs

Compares (byte-exact): earlyauth.bin, m8a.bin, m90.bin.
m92.bin is random per session — checked structurally instead (un-zigzag the
LEB and verify the v8/V9 relation of probe7.build_92).
"""
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "py"))
# Functional core (stdlib-only): the builders live in msgbuild so the test
# battery never needs aioquic; probes re-export the same names.
from msgbuild import early_auth_payload, build_8a, build_90  # noqa: E402

V9 = 0x63E25F26
M64 = (1 << 64) - 1


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    join_path, msgs_dir = sys.argv[1], sys.argv[2]
    j = json.load(open(join_path))
    js = j["joinScript"]
    reply = j.get("reply") or {}
    ct = js.get("ClientTicket", "") or ""
    version = int(ct.split(";")[-1]) if ct else 0

    py = {
        "earlyauth.bin": early_auth_payload(ct, version),
        "m8a.bin": build_8a(js),
    }
    if isinstance(reply, dict) and reply.get("joinTicket"):
        py["m90.bin"] = build_90(js, reply)
    else:
        print("  (no reply.joinTicket — skipping m90)")

    ok = True
    for name, ref in sorted(py.items()):
        p = os.path.join(msgs_dir, name)
        if not os.path.exists(p):
            print(f"  MISSING {name}")
            ok = False
            continue
        got = open(p, "rb").read()
        match = got == ref
        ok = ok and match
        print(f"  {name}: py={len(ref)}B cpp={len(got)}B {'MATCH' if match else 'DIFF'}")
        if not match:
            n = min(len(ref), len(got))
            for i in range(n):
                if ref[i] != got[i]:
                    print(f"    first diff @ {i}: py={ref[i:i + 16].hex()} "
                          f"cpp={got[i:i + 16].hex()}")
                    break
            else:
                print(f"    length differs: py_tail={ref[n:].hex()} cpp_tail={got[n:].hex()}")

    # m92: structural (random nonce)
    p = os.path.join(msgs_dir, "m92.bin")
    if os.path.exists(p):
        got = open(p, "rb").read()
        good = False
        if got and got[0] == 0x92:
            v, s, i = 0, 0, 1
            while i < len(got):
                b = got[i]
                v |= (b & 0x7F) << s
                s += 7
                i += 1
                if not (b & 0x80):
                    break
            x = ((v >> 1) ^ (M64 if v & 1 else 0)) & M64
            v8 = x >> 32
            good = x == (((v8 << 32) | (v8 ^ V9)) & M64)
        print(f"  m92.bin: {len(got)}B structure={'OK' if good else 'BAD'}")
        ok = ok and good
    else:
        print("  MISSING m92.bin")
        ok = False

    print("MSG PARITY OK" if ok else "MSG PARITY DIFF")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
