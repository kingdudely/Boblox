#!/usr/bin/env python3
"""probe_webjoin.py — compare gamejoin v1 and v2 responses for a place.

The browser client (play.roblox.com/games/start) needs `GameFqdn` from the
join script: its WebTransport transport refuses to start without it
("WASM WebTransport requires ConnectConfiguration::gameFqdn") and builds
`{}-{}-{}.{}.qdmux.roblox.com` from it. This probe prints, for both API
versions, the status, the content type, and every join-script field related to
the server endpoint (gameFqdn / qdmux / NetStack / Udmux), so the difference
between a place that works in the browser and one that does not is visible.

usage:
  tools/probe_webjoin.py --cookie-file ~/mizg.txt --place 1818 [--universe N]
  tools/probe_webjoin.py --cookie-file ~/mizg.txt --place 6597976680

Nothing is written to disk; the cookie is only sent in the request header.
"""
import argparse
import json
import sys
import urllib.error
import urllib.request
import uuid

GAMEJOIN = "https://gamejoin.roblox.com"
UA_BROWSER = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/141.0 Safari/537.36"
UA_CLIENT = "Roblox/WinInet"


def post(url, body, cookie, token=None, ua=UA_BROWSER, browser_headers=True):
    data = json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Cookie", ".ROBLOSECURITY=" + cookie)
    req.add_header("User-Agent", ua)
    if browser_headers:
        req.add_header("Origin", "https://www.roblox.com")
        req.add_header("Referer", "https://www.roblox.com/")
    if token:
        req.add_header("X-CSRF-TOKEN", token)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def join(cookie, version, place, universe, job_id, ua=UA_BROWSER, browser_headers=True):
    body = {
        "placeId": place,
        "isTeleport": False,
        "gameJoinAttemptId": str(uuid.uuid4()),
        "browserTrackerId": 0,
        "playSessionId": str(uuid.uuid4()),
        "eventId": str(uuid.uuid4()),
        "launchData": "",
        "joinAttemptOrigin": "PlayButton",
    }
    if universe:
        body["gameId"] = universe
    if job_id:
        body["jobId"] = job_id
    url = f"{GAMEJOIN}/{version}/join-game"
    status, headers, data = post(url, body, cookie, ua=ua, browser_headers=browser_headers)
    if status == 403:
        token = headers.get("x-csrf-token") or headers.get("X-CSRF-Token")
        if token:
            status, headers, data = post(url, body, cookie, token, ua, browser_headers)
    return status, headers, data


def report(label, status, headers, data):
    ctype = headers.get("Content-Type", headers.get("content-type", ""))
    print(f"--- {label}: HTTP {status}, {ctype}, {len(data)}B")
    if "json" not in ctype.lower():
        print("   ", data[:200])
        return
    try:
        doc = json.loads(data)
    except Exception as e:  # noqa: BLE001 - probe
        print("    unparsable:", e)
        return
    js = doc.get("joinScript")
    if isinstance(js, str):
        try:
            js = json.loads(js)
        except Exception:  # noqa: BLE001 - probe
            js = None
    if not isinstance(js, dict):
        print("    keys:", sorted(doc.keys()))
        for k in ("status", "message", "errors", "queuePosition"):
            if k in doc:
                print(f"    {k}: {str(doc[k])[:160]}")
        return
    keys = [k for k in js if any(t in k.lower() for t in
                                 ("fqdn", "qdmux", "netstack", "udmux", "port",
                                  "rccversion", "gameid", "placeid"))]
    for k in sorted(keys):
        print(f"    {k} = {str(js[k])[:150]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cookie-file", required=True)
    ap.add_argument("--place", type=int, required=True)
    ap.add_argument("--universe", type=int, default=0)
    ap.add_argument("--job-id", default="")
    ap.add_argument("--version", default="v1,v2")
    ap.add_argument("--ua", default="browser", choices=("browser", "client"))
    ap.add_argument("--browser-headers", default="yes", choices=("yes", "no"))
    a = ap.parse_args()
    cookie = open(a.cookie_file).read().strip()
    ua = UA_BROWSER if a.ua == "browser" else UA_CLIENT
    bh = a.browser_headers == "yes"
    for v in a.version.split(","):
        status, headers, data = join(cookie, v, a.place, a.universe, a.job_id, ua, bh)
        report(f"{v} place={a.place} ua={a.ua} origin={'yes' if bh else 'no'}",
               status, headers, data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
