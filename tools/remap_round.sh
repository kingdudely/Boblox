#!/usr/bin/env bash
# full_round.sh — one fresh native join; captures in one gdb session:
#   wire 0x9B challenge, decoded =challenge program, complete NextInteger stream,
#   and the sent answer; plus JobId from the session log.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; export RBX_ROOT="$ROOT"
SDL=$ROOT/lib/sdl3ttf/lib
RD=$ROOT/run/remap
kill_all() {
  for p in $(pgrep -x gdb); do kill -9 $p 2>/dev/null; done
  for p in $(pgrep -x mocktail) $(pgrep -x Main); do kill -9 $p 2>/dev/null; done
  sleep 2
}
engine_pid() {
  for d in /proc/[0-9]*; do
    p=${d#/proc/}
    [ -r "$d/maps" ] || continue
    if grep -q "libroblox.so" "$d/maps" 2>/dev/null; then echo "$p"; return 0; fi
  done
  return 1
}

kill_all
rm -rf "$RD"; mkdir -p "$RD"
cd ${MOCKTAIL:-/home/john/mocktail}/build
setsid nohup env LD_LIBRARY_PATH=$SDL DISPLAY=:0 ./mocktail >/dev/null 2>&1 </dev/null &
disown
PID=""
for i in $(seq 1 120); do PID=$(engine_pid); [ -n "$PID" ] && break; sleep 1; done
[ -z "$PID" ] && { echo "no engine"; exit 1; }
echo "engine=$PID"
setsid nohup env CCAP_DIR="$RD" gdb -p "$PID" -batch -x $ROOT/tools/remap_dump.gdb >/dev/null 2>&1 </dev/null &
disown
sleep 8
cd ${MOCKTAIL:-/home/john/mocktail}/build && setsid nohup env LD_LIBRARY_PATH=$SDL DISPLAY=:0 \
  ./mocktail --launch-uri "roblox://placeId=1818" >/dev/null 2>&1 </dev/null &
disown
for i in $(seq 1 150); do
  [ -f "$RD/full.log" ] && grep -q "WIREANS" "$RD/full.log" 2>/dev/null && break
  sleep 1
done
SL=$(ls -t /home/john/.local/state/mocktail/logs/sessions/*.log | head -1)
grep -aoP "Joining game '\K[0-9a-f-]+" "$SL" | tail -1 > "$RD/jobid.txt" 2>/dev/null || true
echo "=== full.log ==="
cat "$RD/full.log" 2>/dev/null | head -80
echo "=== jobid: $(cat $RD/jobid.txt 2>/dev/null)"
ls -la "$RD" | head -12
