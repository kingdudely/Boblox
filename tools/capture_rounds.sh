#!/usr/bin/env bash
# capture_rounds.sh — N fresh-process native joins; per-round capture of:
#   wire 0x9B challenge (u1/u2 + encrypted blob), decoded =challenge program,
#   sent answer, and the session JobId.
# Usage: capture_rounds.sh <rounds> <placeId>
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; export RBX_ROOT="$ROOT"
RUN=$ROOT/run
SDL=$ROOT/lib/sdl3ttf/lib
ROUNDS=${1:-3}
PLACE=${2:-1818}
OUTDIR=$RUN/rounds
mkdir -p "$OUTDIR"

ts() { date +%H:%M:%S; }

engine_pid() {
  for d in /proc/[0-9]*; do
    p=${d#/proc/}
    [ -r "$d/maps" ] || continue
    if grep -q "libroblox.so" "$d/maps" 2>/dev/null; then
      echo "$p"; return 0
    fi
  done
  return 1
}

kill_all() {
  for p in $(pgrep -x gdb); do kill -9 $p 2>/dev/null; done
  for p in $(pgrep -x mocktail) $(pgrep -x Main); do kill -9 $p 2>/dev/null; done
  sleep 2
}

for R in $(seq 1 "$ROUNDS"); do
  echo "[$(ts)] ===== ROUND $R (place $PLACE) ====="
  kill_all
  RD=$OUTDIR/r$R
  rm -rf "$RD"; mkdir -p "$RD"

  cd ${MOCKTAIL:-/home/john/mocktail}/build
  setsid nohup env LD_LIBRARY_PATH=$SDL DISPLAY=:0 ./mocktail >/dev/null 2>&1 </dev/null &
  disown
  PID=""
  for i in $(seq 1 120); do
    PID=$(engine_pid)
    [ -n "$PID" ] && break
    sleep 1
  done
  if [ -z "$PID" ]; then echo "[$(ts)] FAIL: no engine"; continue; fi
  echo "[$(ts)] engine=$PID"

  setsid nohup env CCAP_DIR="$RD" gdb -p "$PID" -batch -x $ROOT/tools/challenge_capture2.gdb >/dev/null 2>&1 </dev/null &
  disown
  sleep 10

  SL_OLD=$(ls -t /home/john/.local/state/mocktail/logs/sessions/*.log | head -1)
  cd ${MOCKTAIL:-/home/john/mocktail}/build && setsid nohup env LD_LIBRARY_PATH=$SDL DISPLAY=:0 \
    ./mocktail --launch-uri "roblox://placeId=$PLACE" >/dev/null 2>&1 </dev/null &
  disown

  for i in $(seq 1 150); do
    [ -f "$RD/ccap.log" ] && grep -q "WIREANS" "$RD/ccap.log" 2>/dev/null && { echo "[$(ts)] answer captured at ${i}s"; break; }
    sleep 1
  done

  echo "[$(ts)] --- round $R results:"
  cat "$RD/ccap.log" 2>/dev/null
  SL=$(ls -t /home/john/.local/state/mocktail/logs/sessions/*.log | head -1)
  grep -aoP "Joining game '\K[0-9a-f-]+" "$SL" | tail -1 > "$RD/jobid.txt" 2>/dev/null || true
  echo "[$(ts)] jobid: $(cat "$RD/jobid.txt" 2>/dev/null)"
  sleep 2
done

kill_all
echo "[$(ts)] all rounds done"
echo "=== decoded challenge programs per round ==="
md5sum $OUTDIR/r*/chal_*.bin 2>/dev/null
