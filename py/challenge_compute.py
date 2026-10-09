#!/usr/bin/env python3
"""challenge_compute.py — compute the 0x9B answer locally.

Pipeline:
  1. take the personalized constants (parsed from a decoded =challenge program
     OR captured directly),
  2. substitute into rbx_runtime/challenge/challenge_src.lua,
  3. compile with luau-compile (upstream Luau),
  4. run with challenge_runner (Roblox sandbox: exact Random, game.JobId, ...),
  5. print answer; optionally --check against a native-captured answer.

Usage:
  challenge_compute.py --u1 0x... --u2 0x... --job <jobid> \
      --c1 0x... --c2 0x... --c3 0x... --c4 0x... [--check 0x...]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # RobloxInBrowser
SRC = os.path.join(ROOT, "rbx_runtime", "challenge", "challenge_src.lua")
RUNNER = os.path.join(ROOT, "rbx_runtime", "build", "challenge_runner")
LUAU_COMPILE = os.environ.get("LUAU_COMPILE", os.path.join(ROOT, "third_party", "luau", "build", "luau-compile"))  # needs Luau CLI build (submodule, LUAU_BUILD_CLI=ON)


def build_bytecode(c1, c2, c3, c4, outdir):
    src = open(SRC).read()
    src = src.replace("__C1__", str(c1)).replace("__C2__", str(c2))
    src = src.replace("__C3__", str(c3)).replace("__C4__", str(c4))
    lua = os.path.join(outdir, "challenge.lua")
    luac = os.path.join(outdir, "challenge.luac")
    with open(lua, "w") as f:
        f.write(src)
    r = subprocess.run([LUAU_COMPILE, "--binary", lua], capture_output=True)
    if r.returncode != 0:
        raise RuntimeError(f"luau-compile failed: {r.stderr.decode()}")
    with open(luac, "wb") as f:
        f.write(r.stdout)
    return luac


def run(luac, u1, u2, job, opts):
    cmd = [RUNNER, f"--program={luac}", f"--u1={u1}", f"--u2={u2}", f"--job={job}"]
    for k, v in opts.items():
        cmd.append(f"--{k}={v}")
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if r.returncode != 0:
        raise RuntimeError(f"runner failed rc={r.returncode}: {r.stderr.strip()}")
    m = re.search(r"answer=([0-9a-f]{8})", r.stdout)
    if not m:
        raise RuntimeError(f"no answer in: {r.stdout}")
    return int(m.group(1), 16), r.stdout.strip()


def default_opts():
    return {"usersettings": "error", "os-exit": "missing", "studio": "false", "newproxy": "ok"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--u1", required=True)
    ap.add_argument("--u2", required=True)
    ap.add_argument("--job", required=True)
    ap.add_argument("--c1", required=True)
    ap.add_argument("--c2", required=True)
    ap.add_argument("--c3", required=True)
    ap.add_argument("--c4", required=True)
    ap.add_argument("--check", default=None, help="expected answer (hex) to compare")
    ap.add_argument("--usersettings", default="error")
    ap.add_argument("--os-exit", default="missing")
    ap.add_argument("--studio", default="false")
    ap.add_argument("--newproxy", default="ok")
    args = ap.parse_args()

    def num(s):
        return int(s, 0)

    with tempfile.TemporaryDirectory() as td:
        luac = build_bytecode(num(args.c1), num(args.c2), num(args.c3), num(args.c4), td)
        opts = {"usersettings": args.usersettings, "os-exit": args.os_exit,
                "studio": args.studio, "newproxy": args.newproxy}
        ans, out = run(luac, num(args.u1), num(args.u2), args.job, opts)
        print(f"answer=0x{ans:08x}  ({out})")
        if args.check is not None:
            exp = num(args.check)
            ok = "MATCH" if ans == exp else "MISMATCH"
            print(f"expected=0x{exp:08x} -> {ok}")
            return 0 if ans == exp else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
