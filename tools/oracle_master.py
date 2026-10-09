#!/usr/bin/env python3
"""oracle_master.py — end-to-end 0x9B challenge oracle driver.

Order (freeze-first, same-server):
  1. Start mocktail (account 1) fresh; attach tools/oracle_patch.gdb.
  2. Trigger a native join -> native receives its 0x9B challenge -> gdb FREEZES it
     (waiting for our challenge file).
  3. Python client (account 2) joins -> receives ITS 0x9B challenge from the same
     fleet build (same blob) -> writes run/oracle_challenge.bin immediately.
  4. gdb (already polling) patches the native's u1/u2 to ours -> native computes
     the answer -> gdb captures it into run/oracle_answer.txt.
  5. Python client sends the answer to its server; we log the outcome.

All paths live under /home/john/RobloxInBrowser so reboots are safe.

Usage: python3 tools/oracle_master.py [--place 1818]
"""
import argparse
import os
import signal
import subprocess
import sys
import time

ROOT = "/home/john/RobloxInBrowser"
RUN = os.path.join(ROOT, "run")
GLOG = os.path.join(RUN, "oracle_gdb.log")
CHAL = os.path.join(RUN, "oracle_challenge.bin")
ANS = os.path.join(RUN, "oracle_answer.txt")
P10LOG = os.path.join(RUN, "p10_master.log")
MOCKTAIL = "/home/john/mocktail/build/mocktail"
SDL_LIB = os.path.join(ROOT, "lib/sdl3ttf/lib")
GDB_SCRIPT = os.path.join(ROOT, "tools/oracle_patch.gdb")
COOKIE2 = os.path.join(RUN, "cookie2.txt")

def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)

def run(cmd, **kw):
    return subprocess.run(cmd, **kw)

def pids_named(name):
    out = subprocess.run(["pgrep", "-x", name], capture_output=True, text=True).stdout
    return [int(x) for x in out.split()]

def kill_mocktail():
    for p in pids_named("mocktail"):
        try:
            os.kill(p, signal.SIGKILL)
        except ProcessLookupError:
            pass
    time.sleep(2)

def mocktail_real_pid():
    for p in pids_named("mocktail"):
        try:
            with open(f"/proc/{p}/maps") as f:
                if "libroblox.so" in f.read():
                    return p
        except OSError:
            continue
    return None

def start_mocktail():
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = SDL_LIB
    env["DISPLAY"] = ":0"
    subprocess.Popen([MOCKTAIL], cwd="/home/john/mocktail/build", env=env,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     start_new_session=True)

def trigger_join(uri=None):
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = SDL_LIB
    env["DISPLAY"] = ":0"
    if uri is None:
        uri = "roblox://placeId=1818"
    subprocess.Popen([MOCKTAIL, "--launch-uri", uri], cwd="/home/john/mocktail/build",
                     env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     start_new_session=True)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--place", type=int, default=1818)
    args = ap.parse_args()

    os.makedirs(RUN, exist_ok=True)
    for f in (CHAL, ANS, GLOG, P10LOG):
        if os.path.exists(f):
            os.remove(f)

    log("killing old mocktail")
    kill_mocktail()

    log("starting mocktail (idle)")
    start_mocktail()
    rp = None
    for _ in range(120):
        rp = mocktail_real_pid()
        if rp:
            break
        time.sleep(1)
    if not rp:
        log("FAIL: mocktail never loaded")
        return 1
    log(f"mocktail pid={rp}")

    log("attaching oracle gdb (freeze-first)")
    gdb = subprocess.Popen(["gdb", "-p", str(rp), "-batch", "-x", GDB_SCRIPT],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           start_new_session=True)
    for _ in range(60):
        if os.path.exists(GLOG) and "oracle bp set" in open(GLOG).read():
            break
        time.sleep(1)
    log("gdb armed")

    log("triggering native join (will freeze at challenge)")
    trigger_join()
    frozen = False
    for _ in range(120):
        if os.path.exists(GLOG) and "native challenge msg at" in open(GLOG).read():
            frozen = True
            break
        time.sleep(1)
    if not frozen:
        log("FAIL: native never reached its challenge")
        return 1
    log("NATIVE FROZEN at challenge")

    log("starting python client (account 2) -> challenge -> answer roundtrip")
    p10 = subprocess.Popen([sys.executable, "-u", os.path.join(ROOT, "py/probe10.py"),
                            "--seconds", "180", "--follow", "4656429295"],
                           env={**os.environ, "RBX_COOKIE_FILE": COOKIE2},
                           stdout=open(P10LOG, "w"), stderr=subprocess.STDOUT,
                           start_new_session=True)

    answered = False
    for _ in range(180):
        if os.path.exists(ANS):
            answered = True
            break
        if p10.poll() is not None:
            break
        time.sleep(1)
    if answered:
        log(f"ORACLE ANSWER: {open(ANS).read().strip()}")
    else:
        log("no oracle answer captured")

    # wait a little for the python client to report the outcome
    deadline = time.time() + 60
    while time.time() < deadline and p10.poll() is None:
        time.sleep(1)

    log("--- python client outcome ---")
    if os.path.exists(P10LOG):
        for line in open(P10LOG, errors="replace").read().splitlines()[-25:]:
            print("   ", line[:160])
    log("--- gdb oracle trace ---")
    if os.path.exists(GLOG):
        for line in open(GLOG, errors="replace").read().splitlines():
            if any(k in line for k in ("blobcheck", "patching", "captured", "killing", "native challenge")):
                print("   ", line[:160])
    return 0


if __name__ == "__main__":
    sys.exit(main())
