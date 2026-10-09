#!/usr/bin/env python3
"""fit_env.py — run challenge_runner over all sandbox option combos and find
which combination reproduces the native answers, for each known dataset.

Datasets come from run/rounds/r*/ (WIRECHAL + CHALPROG + WIREANS + jobid).
"""
import itertools
import os
import re
import subprocess
import sys

ROOT = "/home/john/RobloxInBrowser"
RUNNER = f"{ROOT}/luau_runner/build/challenge_runner"

def load_dataset(rd):
    d = {}
    for f in os.listdir(rd):
        if f.startswith("wire_chal_"):
            raw = open(os.path.join(rd, f), "rb").read()
            d["u1"] = int.from_bytes(raw[1:5], "little")
            d["u2"] = int.from_bytes(raw[5:9], "little")
        elif f.startswith("chal_") and f.endswith(".bin"):
            d["program"] = os.path.join(rd, f)
        elif f.startswith("wire_ans_"):
            raw = open(os.path.join(rd, f), "rb").read()
            d["answer"] = int.from_bytes(raw[5:9], "little")
        elif f == "jobid.txt":
            d["job"] = open(os.path.join(rd, f)).read().strip()
    return d

def run(ds, opts):
    cmd = [RUNNER, f"--program={ds['program']}", f"--u1={ds['u1']}", f"--u2={ds['u2']}",
           f"--job={ds['job']}"]
    for k, v in opts.items():
        cmd.append(f"--{k}={v}")
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    if r.returncode != 0:
        return None, r.stderr.strip()
    m = re.search(r"answer=([0-9a-f]{8})", r.stdout)
    return (int(m.group(1), 16) if m else None), r.stdout.strip()

def main():
    rounds = sorted([d for d in os.listdir(f"{ROOT}/run/rounds") if d.startswith("r")])
    datasets = []
    for r in rounds:
        ds = load_dataset(f"{ROOT}/run/rounds/{r}")
        if all(k in ds for k in ("u1", "u2", "program", "answer", "job")):
            datasets.append((r, ds))
    if not datasets:
        print("no complete datasets found")
        return
    print(f"datasets: {[r for r, _ in datasets]}")
    for r, ds in datasets:
        print(f"  {r}: u1=0x{ds['u1']:08x} u2=0x{ds['u2']:08x} expected=0x{ds['answer']:08x} job={ds['job']}")
    print()

    combos = list(itertools.product(
        ["error", "ok"],           # usersettings
        ["missing", "ok", "exit"], # os-exit
        ["false", "true", "error"],# studio
        ["ok", "error"],           # newproxy
    ))
    print(f"testing {len(combos)} sandbox combos x {len(datasets)} datasets ...")
    best = []
    for combo in combos:
        us, ox, st, np_ = combo
        opts = {"usersettings": us, "os-exit": ox, "studio": st, "newproxy": np_}
        oks = 0
        outs = []
        for r, ds in datasets:
            ans, info = run(ds, opts)
            outs.append(ans)
            if ans == ds["answer"]:
                oks += 1
        if oks:
            best.append((oks, combo, outs))
            print(f"  {oks}/{len(datasets)} match  {opts}  answers={[hex(a) if a is not None else None for a in outs]}")

    if not best:
        print("\nno combo matched; showing sample outputs of a default run:")
        r, ds = datasets[0]
        ans, info = run(ds, {"usersettings": "error", "os-exit": "missing", "studio": "false", "newproxy": "ok"})
        print(" out:", ans, info)
    else:
        best.sort(reverse=True)
        print("\nBEST:", best[0])

if __name__ == "__main__":
    main()
