#!/usr/bin/env bash
# solve_delta.sh — for each captured dataset: standardize the wire program, run
# with both argument orders, and print deltas vs the native's captured answer.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; export RBX_ROOT="$ROOT"
RUNNER=$ROOT/rbx_runtime/build/challenge_runner
STD=$ROOT/tools/standardize_wire.py
OUT=$ROOT/run/delta
mkdir -p "$OUT"
: > "$OUT/table.txt"

for D in run/comb run/full run/rounds/r1 run/rounds/r2 run/rounds/r3 run/rounds/r4 run/remap; do
  [ -f "$ROOT/$D/wire.bin" ] || [ -f "$ROOT/$D/wire_chal_0.bin" ] || continue
  # standardize (uses wire.bin); for rounds dirs, copy wire_chal_0 to wire.bin if needed
  W="$ROOT/$D/wire.bin"
  if [ ! -f "$W" ]; then
    cp "$ROOT/$D/wire_chal_0.bin" "$W" 2>/dev/null || true
  fi
  luac="$OUT/$(echo "$D" | tr '/' '_').luac"
  python3 "$STD" "$ROOT/$D" "$luac" > "$OUT/std_$(echo "$D" | tr '/' '_').log" 2>&1 || true
  # extract u1/u2/answer/job
  python3 - "$ROOT/$D" "$luac" "$OUT/table.txt" <<'EOF'
import os, re, struct, subprocess, sys
d, luac, table = sys.argv[1], sys.argv[2], sys.argv[3]
def varint(buf, off):
    v=0; sh=0
    while True:
        b=buf[off]; off+=1
        v |= (b&0x7f)<<sh
        if not (b&0x80): return v,off
        sh+=7
def parse_wire(buf):
    o=0; ver=buf[o]; o+=1; tv=buf[o]; o+=1
    ns,o=varint(buf,o)
    for _ in range(ns):
        ln,o=varint(buf,o); o+=ln
    if tv==3:
        x=buf[o]; o+=1
        while x!=0:
            _,o=varint(buf,o); x=buf[o]; o+=1
    pc,o=varint(buf,o)
    return ver
# challenge file
chal = os.path.join(d, "wire_chal_0.bin")
if not os.path.exists(chal): chal = os.path.join(d, "wire_chal.bin")
raw = open(chal,"rb").read()
if raw[:1] == b"\x9b":
    u1 = int.from_bytes(raw[1:5],"little"); u2 = int.from_bytes(raw[5:9],"little")
else:
    # run/comb saved the whole message? comb.log had WIRECHAL parsed
    u1 = u2 = None
# answer
ansf = os.path.join(d, "wire_ans_0.bin")
if not os.path.exists(ansf): ansf = os.path.join(d, "wire_ans.bin")
ans = None
if os.path.exists(ansf):
    a = open(ansf,"rb").read()
    if a[:1] == b"\x9b":
        ans = int.from_bytes(a[5:9],"little")
# job
job = ""
jf = os.path.join(d, "jobid.txt")
if os.path.exists(jf):
    job = open(jf).read().strip()
if u1 is None or ans is None:
    print(f"{d}: incomplete", file=open(table,"a")); sys.exit()
def run(a1, a2, job):
    r = subprocess.run([ "$ROOT/rbx_runtime/build/challenge_runner",
        f"--program={luac}", f"--u1={a1:#x}", f"--u2={a2:#x}", f"--job={job}",
    ], capture_output=True, text=True, timeout=60)
    if r.returncode != 0: return None
    m = re.search(r"answer=([0-9a-f]{8})", r.stdout)
    return int(m.group(1),16) if m else None
o12 = run(u1, u2, job)
o21 = run(u2, u1, job)
d12 = (ans - o12) & 0xFFFFFFFF if o12 is not None else None
d21 = (ans - o21) & 0xFFFFFFFF if o21 is not None else None
jsum = sum(job.encode())
line = f"{d}: u1={u1:#010x} u2={u2:#010x} ans={ans:#010x} ours12={o12 if o12 is None else hex(o12)} d12={d12 if d12 is None else hex(d12)} ours21={o21 if o21 is None else hex(o21)} d21={d21 if d21 is None else hex(d21)} jsum={jsum}"
print(line, file=open(table,"a"))
print(line)
EOF
done
echo
echo "=== summary ==="
cat "$OUT/table.txt"
