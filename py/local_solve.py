#!/usr/bin/env python3
"""local_solve.py — compute the 0x9B answer locally from a captured program.

Steps:
  1. extract the v5 constants from the =challenge program file
  2. substitute them into the decompiled template
  3. compile with upstream luau-compile (LuauEmitCallFeedback=false to match our VM)
  4. run challenge_runner (Roblox sandbox: exact Random, game.JobId)
  5. print answer; --check compares against a native-captured answer

Usage:
  local_solve.py --program run/full/chal_0_s7226.bin \
      --u1 0x8029dd95 --u2 0x29553a8e --job <jobid> [--check 0x82bfe8be]
"""
import argparse
import math
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEMPLATE = os.path.join(ROOT, "luau_runner", "challenge", "challenge_src.lua")
RUNNER = os.path.join(ROOT, "luau_runner", "build", "challenge_runner")
LUAU_COMPILE = "/home/john/luau/build/luau-compile"


def extract_constants(path):
    """Find 4 consecutive double constants (v5 = {c1,c2,c3,c4})."""
    d = open(path, "rb").read()
    best = None
    for i in range(len(d) - 4 * 9):
        if d[i] != 2:
            continue
        off = i
        vals = []
        ok = True
        for _ in range(4):
            if d[off] != 2:
                ok = False
                break
            try:
                v = struct.unpack("<d", d[off + 1:off + 9])[0]
            except Exception:
                ok = False
                break
            if not math.isfinite(v) or v != int(v) or abs(v) > 0xFFFFFFFF:
                ok = False
                break
            vals.append(int(v))
            off += 9
        if ok:
            best = (i, vals)
            break
    if best is None:
        raise RuntimeError("no v5 constant cluster found")
    return best[1]


def build(constants, outdir):
    src = open(TEMPLATE).read()
    assert "__C1__" in src, "template was already substituted!"
    src = src.replace("__C1__", str(constants[0]))
    src = src.replace("__C2__", str(constants[1]))
    src = src.replace("__C3__", str(constants[2]))
    src = src.replace("__C4__", str(constants[3]))
    lua = os.path.join(outdir, "challenge.lua")
    luac = os.path.join(outdir, "challenge.luac")
    open(lua, "w").write(src)
    r = subprocess.run(
        [LUAU_COMPILE, "--binary", "--fflags=LuauEmitCallFeedback=false", lua],
        capture_output=True)
    if r.returncode != 0:
        raise RuntimeError("luau-compile: " + r.stderr.decode())
    open(luac, "wb").write(r.stdout)
    return luac


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--program", required=True)
    ap.add_argument("--u1", required=True)
    ap.add_argument("--u2", required=True)
    ap.add_argument("--job", required=True)
    ap.add_argument("--check", default=None)
    ap.add_argument("--trace", action="store_true")
    ap.add_argument("--usersettings", default="error")
    ap.add_argument("--os-exit", default="missing")
    ap.add_argument("--studio", default="false")
    ap.add_argument("--newproxy", default="ok")
    args = ap.parse_args()

    c = extract_constants(args.program)
    print(f"v5 constants: {c}")
    u1 = int(args.u1, 0)
    u2 = int(args.u2, 0)
    with tempfile.TemporaryDirectory() as td:
        luac = build(c, td)
        cmd = [RUNNER, "--program=" + luac, f"--u1={u1:#x}", f"--u2={u2:#x}",
               "--job=" + args.job,
               "--usersettings=" + args.usersettings, "--os-exit=" + args.os_exit,
               "--studio=" + args.studio, "--newproxy=" + args.newproxy]
        env = dict(os.environ)
        if args.trace:
            env["RBX_RNG_TRACE"] = "1"
        r = subprocess.run(cmd, capture_output=True, text=True, env=env)
        if r.returncode != 0:
            print("runner rc", r.returncode, r.stderr[-2000:])
            return 2
        print(r.stdout.strip())
        if args.trace:
            print(r.stderr.strip())
        if args.check:
            want = int(args.check, 0)
            m = re.search(r"answer=([0-9a-f]{8})", r.stdout)
            got = int(m.group(1), 16) if m else None
            ok = got == want
            print(f"expected 0x{want:08x} got 0x{got:08x} -> {'MATCH' if ok else 'MISMATCH'}")
            return 0 if ok else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
