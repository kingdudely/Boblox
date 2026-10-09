#!/usr/bin/env python3
"""fit_env2.py — find sandbox option combo matching native answers (with the
working LuauEmitCallFeedback=false compile)."""
import itertools
import os
import re
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNNER = os.path.join(ROOT, "rbx_runtime", "build", "challenge_runner")
# legacy calibration tool: needs luau-compile (build the Luau submodule with
# LUAU_BUILD_CLI=ON, or point LUAU_COMPILE at an existing binary)
LUAU_COMPILE = os.environ.get(
    "LUAU_COMPILE", os.path.join(ROOT, "third_party", "luau", "build", "luau-compile"))
SRC = os.path.join(ROOT, "rbx_runtime", "challenge", "challenge_src.lua")


def compile_prog(c1, c2, c3, c4):
    s = open(SRC).read()
    s = s.replace("__C1__", str(c1)).replace("__C2__", str(c2))
    s = s.replace("__C3__", str(c3)).replace("__C4__", str(c4))
    open("/tmp/opencode/cc.lua", "w").write(s)
    r = subprocess.run(
        [LUAU_COMPILE, "--binary", "--fflags=LuauEmitCallFeedback=false", "/tmp/opencode/cc.lua"],
        capture_output=True)
    assert r.returncode == 0, r.stderr
    open("/tmp/opencode/cc.luac", "wb").write(r.stdout)
    return "/tmp/opencode/cc.luac"


def run(luac, u1, u2, job, opts):
    cmd = [RUNNER, "--program=" + luac, "--u1=" + hex(u1), "--u2=" + hex(u2), "--job=" + job]
    for k, v in opts.items():
        cmd.append("--" + k + "=" + v)
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    if r.returncode != 0:
        return None, r.stderr.strip()
    m = re.search(r"answer=([0-9a-f]{8})", r.stdout)
    return (int(m.group(1), 16) if m else None), r.stdout.strip()


JOBS = {"23c2e471": ("23c2e471-7b39-44bd-82b8-cd25661f9355", (939834315, 104405534, 9957725, 1783671846))}
DATASETS = [
    # (u1, u2, job_key, expected)
    (0x3a25ef3f, 0x0d50654c, "23c2e471", 0x7167bb68),
    (0x78f16f43, 0x36361a73, "23c2e471", 0xf5a4cdba),
    (0x419dc990, 0xd4aed18a, "23c2e471", 0x067a328f),
    (0x705042c4, 0x46efc60f, "23c2e471", 0x70ee575b),
]

COMBOS = list(itertools.product(
    ["error", "ok"],
    ["missing", "ok", "exit"],
    ["false", "true", "error"],
    ["ok", "error"],
))


def main():
    luaс_per_job = {}
    for jk, (job, consts) in JOBS.items():
        luaс_per_job[jk] = compile_prog(*consts)
    best = []
    for combo in COMBOS:
        us, ox, st, np_ = combo
        opts = {"usersettings": us, "os-exit": ox, "studio": st, "newproxy": np_}
        oks = 0
        outs = []
        for u1, u2, jk, exp in DATASETS:
            job = JOBS[jk][0]
            luac = luaс_per_job[jk]
            a, out = run(luac, u1, u2, job, opts)
            outs.append(a)
            if a == exp:
                oks += 1
        if oks:
            best.append((oks, combo, outs))
            print(f"{oks}/{len(DATASETS)} {combo} -> {[hex(o) if o is not None else None for o in outs]}")
    if not best:
        print("no combo matched. sample default outputs:")
        for u1, u2, jk, exp in DATASETS:
            job = JOBS[jk][0]
            a, out = run(luaс_per_job[jk], u1, u2, job,
                         {"usersettings": "error", "os-exit": "missing", "studio": "false", "newproxy": "ok"})
            print(f"  got={hex(a) if a is not None else None} exp={hex(exp)}  ({out})")
    else:
        best.sort(reverse=True)
        print("\nBEST:", best[0])


if __name__ == "__main__":
    main()
