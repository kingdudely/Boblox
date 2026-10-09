#!/usr/bin/env python3
"""find_job.py — query the presence API for the job (server) an account is in."""
import json, sys, urllib.request

COOKIE = open(sys.argv[1] if len(sys.argv) > 1 else "/home/john/RobloxInBrowser/run/cookie.txt").read().strip()
USERID = sys.argv[2] if len(sys.argv) > 2 else "4656429295"

req = urllib.request.Request("https://presence.roblox.com/v1/presence/users",
                             data=json.dumps({"userIds": [int(USERID)]}).encode())
req.add_header("Content-Type", "application/json")
req.add_header("User-Agent", "Roblox/WinInet")
req.add_header("Cookie", ".ROBLOSECURITY=" + COOKIE)
with urllib.request.urlopen(req, timeout=20) as r:
    d = json.load(r)
pres = d["userPresences"][0]
print(json.dumps(pres, indent=1))
print("JOB=" + str(pres.get("gameId")))
