#!/usr/bin/env python3
"""solve9b.py — local 0x9B challenge solver for the Python client.

Full autonomous path (no native/oracle):
  blob --(RSB1-xor keystream + xxhash32 check + zstd)--> wire program
      --(REMAP/OPLEN opcode standardize)--> standard Luau bytecode
      --(challenge_runner: exact PCG Random + sandbox)--> answer

Usage:
  solve = ChallengeSolver()
  answer = solve.solve(u1, u2, blob, jobid)   -> int (32-bit)
  resp = solve.response(u1, u2, blob, jobid)  -> bytes b'\\x9b' + u2 + answer

The server expects the answer promptly after the challenge arrives; the whole
path runs in ~50ms (bytecode is cached per blob hash).
"""
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from challenge_blob import blob_to_standard, extract_blob  # noqa: E402

# RBX_RUNNER overrides the binary path (used by the CTest superbuild, which
# builds challenge_runner into its own build dir).
RUNNER = os.environ.get("RBX_RUNNER") or os.path.join(
    ROOT, "rbx_runtime", "build", "challenge_runner")
# Sandbox profile bit-exact vs native (see FINDINGS.md):
#   tostring(UserSettings()) byte-sum 1924, os.exit missing (+9001),
#   IsStudio false (+1024), newproxy namecall ok (+52)
US_STRING = "a" * 19 + "Q"
SANDBOX = ["--usersettings=ok", f"--us-string={US_STRING}",
           "--os-exit=missing", "--studio=false", "--newproxy=ok"]


class ChallengeSolver:
    def __init__(self, cache_dir=None, log=print):
        self.cache_dir = cache_dir or os.path.join(ROOT, "run", "std_cache")
        os.makedirs(self.cache_dir, exist_ok=True)
        self.log = log

    def standardize(self, blob):
        """blob -> standard bytecode bytes, cached by blob hash."""
        h = hashlib.sha256(blob).hexdigest()[:32]
        path = os.path.join(self.cache_dir, h + ".luac")
        if os.path.exists(path):
            return path
        std, stats = blob_to_standard(blob)
        if stats["bad_walks"]:
            raise ValueError(f"proto walk failed: {stats['bad_walks']}")
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(std)
        os.replace(tmp, path)
        self.log(f"  [solve9b] standardized blob {len(blob)}B -> {len(std)}B "
                 f"({stats['protos']} protos, {stats['starts']} starts)")
        return path

    def solve(self, u1, u2, blob, job):
        """Compute the 32-bit answer. Note arg order: runner --u1=u2_field."""
        prog = self.standardize(blob)
        r = subprocess.run(
            [RUNNER, f"--program={prog}", f"--u1={u2:#x}", f"--u2={u1:#x}",
             f"--job={job}", *SANDBOX],
            capture_output=True, text=True, timeout=10)
        m = re.search(r"answer=([0-9a-f]{8})", r.stdout)
        if not m:
            raise RuntimeError(f"runner failed: {r.stdout!r} {r.stderr!r}")
        return int(m.group(1), 16)

    def response(self, u1, u2, blob, job):
        """Full 9-byte 0x9B response message."""
        ans = self.solve(u1, u2, blob, job)
        return b"\x9b" + struct.pack("<I", u2) + struct.pack("<I", ans), ans


def solve_message(chal_msg, job, log=None):
    """probe10 entry point: full 0x9B message bytes -> 32-bit answer int.

    chal_msg = [9b][u1 u32][u2 u32][len u32][blob]
    """
    u1, u2, blob = extract_blob(chal_msg)
    s = ChallengeSolver(log=log or (lambda *a: None))
    return s.solve(u1, u2, blob, job)


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("chal", help="0x9B message file (wire_chal*.bin)")
    ap.add_argument("--job", required=True)
    ap.add_argument("--check", help="expected answer hex (native capture)")
    args = ap.parse_args()
    data = open(args.chal, "rb").read()
    if data[:1] == b"\x9b":
        u1, u2 = struct.unpack_from("<II", data, 1)
        ln = struct.unpack_from("<I", data, 9)[0]
        blob = data[13:13 + ln]
    else:
        blob = data
        raise SystemExit("need a full 0x9B message for u1/u2")
    s = ChallengeSolver()
    resp, ans = s.response(u1, u2, blob, args.job)
    print(f"u1={u1:#010x} u2={u2:#010x} -> answer={ans:#010x} resp={resp.hex()}")
    if args.check:
        want = int(args.check, 16)
        print("CHECK:", "MATCH" if ans == want else f"MISMATCH want {want:#010x}")
