#!/usr/bin/env python3
"""regress9b.py — one-command regression for the full autonomous challenge path.

Runs py/solve9b.solve_message (the exact probe10 import path) over every
captured dataset (blob -> answer) and compares against the native's own sent
answer. No native/gdb needed.

Usage: python3 tools/regress9b.py
"""
import glob
import os
import struct
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "py"))
from solve9b import solve_message  # noqa: E402

DATASETS = ["run/comb_1", "run/comb_2", "run/comb_3", "run/full", "run/rounds/r1"]


def main():
    fails = 0
    total = 0
    for d in DATASETS:
        d = os.path.join(ROOT, d)
        chals = sorted(glob.glob(os.path.join(d, "wire_chal*.bin")))
        anss = sorted(glob.glob(os.path.join(d, "wire_ans*.bin")))
        if not chals or not anss:
            print(f"{d:36s} SKIP (missing capture)")
            continue
        msg = open(chals[0], "rb").read()
        native = int.from_bytes(open(anss[0], "rb").read()[5:9], "little")
        job = open(os.path.join(d, "jobid.txt")).read().strip()
        t0 = time.monotonic()
        try:
            got = solve_message(msg, job)
        except Exception as e:
            print(f"{d:36s} ERROR {e!r}")
            fails += 1
            total += 1
            continue
        ms = (time.monotonic() - t0) * 1000
        ok = got == native
        total += 1
        fails += 0 if ok else 1
        print(f"{d:36s} native={native:#010x} got={got:#010x} "
              f"{'MATCH' if ok else 'FAIL'} ({ms:.0f}ms)")
    print(f"\n{total - fails}/{total} match")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
