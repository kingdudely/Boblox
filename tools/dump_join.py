#!/usr/bin/env python3
"""Fetch a real join reply for a place and dump joinScript + NetStackConfig keys.

Usage: RBX_COOKIE_FILE=run/cookie2.txt python3 tools/dump_join.py [place_id] [out.json]
"""
import base64
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "py"))
from rbx_client import join_game, decode_uri  # noqa: E402

PLACE = int(sys.argv[1]) if len(sys.argv) > 1 else 1818
OUT = sys.argv[2] if len(sys.argv) > 2 else "run/join_1818.json"

cookie_file = os.environ.get("RBX_COOKIE_FILE", "run/cookie2.txt")
with open(cookie_file) as f:
    cookie = f.read().strip()

js, reply = join_game(PLACE, cookie=cookie)
cfg = json.loads(decode_uri(js["NetStackConfig"]))

print("joinScript keys:", sorted(js.keys()))
print()
print("NetStackConfig keys:", sorted(cfg.keys()))
for k in sorted(cfg.keys()):
    v = cfg[k]
    if isinstance(v, str) and len(v) > 80:
        v = v[:80] + "..."
    print(f"  {k} = {v!r}")
print()
for extra in ("ephemeralEarlyPublicKey", "GameFqdn", "ClientTicket"):
    print(f"joinScript[{extra}] present: {extra in js}")

with open(OUT, "w") as f:
    json.dump({"joinScript": js, "netStackConfig": cfg}, f, indent=1)
print("saved ->", OUT)
