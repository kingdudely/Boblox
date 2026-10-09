#!/usr/bin/env python3
"""align_check.py — validate the decompiled template against the native's decoded
program dump (run/remap).

Steps:
  1. extract v5 constants from the wire program
  2. compile the template at -O0/-O1/-O2 with upstream luau-compile
  3. print proto count + per-proto sizecode
  4. compare with the native's first N proto dumps (from remap.log)
"""
import os
import re
import subprocess
import sys

ROOT = "/home/john/RobloxInBrowser"
sys.path.insert(0, os.path.join(ROOT, "py"))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from local_solve import extract_constants  # noqa: E402
from patch_decoded_bc import varint      # noqa: E402


def parse_sizes(data):
    off = 0
    version = data[off]; off += 1
    tv = data[off]; off += 1
    n_s, off = varint(data, off)
    for _ in range(n_s):
        ln, off = varint(data, off)
        off += ln
    if tv == 3:
        idx = data[off]; off += 1
        while idx != 0:
            _, off = varint(data, off)
            idx = data[off]; off += 1
    pc, off = varint(data, off)
    sizes = []
    for _ in range(pc):
        ps, off = varint(data, off)
        pstart = off
        off += 4 + 1
        if tv in (1, 2, 3):
            ts, off = varint(data, off)
            off += ts
        sc, off = varint(data, off)
        sizes.append(sc)
        off = pstart + ps
    return version, tv, sizes


def native_sizes(logpath, count):
    sizes = []
    started = False
    for line in open(logpath):
        if "LOADER =challenge" in line:
            started = True
            continue
        if not started:
            continue
        m = re.match(r"PROTO#(\d+) sizecode=(\d+)", line)
        if m:
            sizes.append(int(m.group(2)))
            if len(sizes) >= count:
                break
    return sizes


def main():
    wire = os.path.join(ROOT, "run", "remap", "wire.bin")
    consts = extract_constants(wire)
    print("wire constants:", consts)

    src = open(os.path.join(ROOT, "luau_runner", "challenge", "challenge_src.lua")).read()
    for k, v in zip(("__C1__", "__C2__", "__C3__", "__C4__"), consts):
        src = src.replace(k, str(v))
    open("/tmp/opencode/align.lua", "w").write(src)

    NAT = native_sizes(os.path.join(ROOT, "run", "remap", "remap.log"), 24)
    print("native sizes (first 24):", NAT)

    for opt in ("0", "1", "2"):
        r = subprocess.run(
            ["/home/john/luau/build/luau-compile", "--binary", f"-O{opt}",
             "--fflags=LuauEmitCallFeedback=false", "/tmp/opencode/align.lua"],
            capture_output=True)
        if r.returncode != 0:
            print(f"-O{opt}: compile failed:", r.stderr.decode()[:200])
            continue
        data = r.stdout
        open(f"/tmp/opencode/align_O{opt}.luac", "wb").write(data)
        version, tv, sizes = parse_sizes(data)
        print(f"-O{opt}: version={version} tv={tv} protos={len(sizes)} sizes={sizes} bytes={len(data)}")


if __name__ == "__main__":
    main()
