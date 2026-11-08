#!/usr/bin/env python3
"""idapy.py — run IDAPython inside the already-open IDA GUI.

The ida-mcp stdio server refuses to attach to a database another session
owns ("no open database instance"), but every GUI instance registers a
nexus record (~/.idapro/nexus/instances/*.json) carrying its port and a
bearer token. This helper posts straight to that instance, so queries run
against the database the user has open and analysed.

    python3 tools/idapy.py script.py     # run a file
    python3 tools/idapy.py -c 'code'     # run inline code
    python3 tools/idapy.py --info        # show the attached instance
"""
import glob
import json
import os
import sys
import urllib.request

INSTANCES = os.path.expanduser("~/.idapro/nexus/instances/*.json")


def live_instance():
    """Newest registered GUI instance that still answers."""
    recs = []
    for path in glob.glob(INSTANCES):
        try:
            with open(path) as f:
                r = json.load(f)
        except (OSError, ValueError):
            continue
        if r.get("backend") != "gui":
            continue
        if not os.path.exists("/proc/%d" % r.get("pid", 0)):
            continue
        recs.append(r)
    recs.sort(key=lambda r: r.get("started_at", 0), reverse=True)
    return recs[0] if recs else None


def execute(code, timeout=600):
    inst = live_instance()
    if not inst:
        raise SystemExit("no live IDA GUI instance registered")
    req = urllib.request.Request(
        "http://127.0.0.1:%d/execute_python" % inst["port"],
        data=json.dumps({"code": code}).encode(),
        headers={"Authorization": "Bearer " + inst["token"],
                 "Content-Type": "application/json"},
        method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:  # 4xx/5xx still carry a JSON body
        return json.loads(e.read().decode())


def main():
    args = sys.argv[1:]
    if args and args[0] == "--info":
        print(json.dumps(live_instance(), indent=2))
        return
    code = args[1] if args and args[0] == "-c" else sys.stdin.read()
    r = execute(code)
    if r.get("ok"):
        print(r.get("output", "").rstrip())
    else:
        err = r.get("error", {})
        print("ERROR %s: %s" % (err.get("code"), err.get("message")),
              file=sys.stderr)
        tb = err.get("traceback")
        if tb:
            print(tb, file=sys.stderr)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
