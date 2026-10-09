#!/usr/bin/env bash
# matrix.sh — challenge-arrival matrix:
#   A7 mode  : empty | fresh (35B acct2 native) | stale (99B)
#   90 tmpl  : acct2 (identity-correct) | acct1 (old, mismatched)
# Logs per-run: job, challenge yes/no, resets, chan1_rx.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; export RBX_ROOT="$ROOT"
PY=$ROOT/py
COOKIE=$ROOT/run/cookie2.txt
OUT=$ROOT/run/matrix
mkdir -p "$OUT"
: > "$OUT/summary.txt"

# make the 35B acct2 A7 available to probe (load_cap resolves inside py/captures)
cp $ROOT/run/dummycap2/m0006_a4_c1_s35.bin $PY/captures/acct2_a7_35.bin 2>/dev/null || true
cp $ROOT/run/fresh_a7.bin $PY/captures/fresh_a7.bin 2>/dev/null || true

run_one() {
  local a7="$1" tmpl="$2" tag="$3"
  local log="$OUT/${tag}.log"
  RBX_COOKIE_FILE=$COOKIE RBX_90_TEMPLATE="$tmpl" RBX_A7="$a7" \
    timeout 40 python3 -u "$PY/probe11.py" --seconds 8 --a7 real > "$log" 2>&1 || true
  local job=$(grep -ao 'job=[0-9a-f-]*' "$log" | head -1 | cut -d= -f2)
  local ch=$(grep -ac "CHALLENGE" "$log")
  local rs=$(grep -a "resets=" "$log" | tail -1 | sed -E 's/.*resets=(\[[^]]*\]).*/\1/')
  local rx=$(grep -ao 'chan1_rx=[0-9]*' "$log" | tail -1 | cut -d= -f2)
  printf "%-28s a7=%-12s tmpl=%-8s job=%s challenge=%s resets=%s rx=%s\n" \
    "$tag" "$a7" "$tmpl" "$job" "$ch" "$rs" "$rx" | tee -a "$OUT/summary.txt"
}

for i in 1 2 3; do
  run_one "acct2_a7_35.bin" "acct2_90.bin" "r${i}_freshA7_acct2-90"
  run_one "acct2_a7_35.bin" "msg_0007_a4_c1.bin" "r${i}_freshA7_acct1-90"
  run_one "msg_0006_a4_c1.bin" "acct2_90.bin" "r${i}_staleA7_acct2-90"
done

echo
echo "=== summary ==="
cat "$OUT/summary.txt"
