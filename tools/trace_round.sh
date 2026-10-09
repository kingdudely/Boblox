#!/usr/bin/env bash
# rng_round.sh — one native join with RNG capture; prints the native NextInteger stream.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; export RBX_ROOT="$ROOT"
SDL=$ROOT/lib/sdl3ttf/lib

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
rm -f $ROOT/run/trace/trace.log
cd ${MOCKTAIL:-/home/john/mocktail}/build
setsid nohup env LD_LIBRARY_PATH=$SDL DISPLAY=:0 ./mocktail >/dev/null 2>&1 </dev/null &
disown
PID=""
for i in $(seq 1 120); do PID=$(engine_pid); [ -n "$PID" ] && break; sleep 1; done
[ -z "$PID" ] && { echo "no engine"; exit 1; }
echo "engine=$PID"
setsid nohup gdb -p "$PID" -batch -x $ROOT/tools/trace_capture.gdb >/dev/null 2>&1 </dev/null &
disown
sleep 8
cd ${MOCKTAIL:-/home/john/mocktail}/build && setsid nohup env LD_LIBRARY_PATH=$SDL DISPLAY=:0 \
  ./mocktail --launch-uri "roblox://placeId=1818" >/dev/null 2>&1 </dev/null &
disown
for i in $(seq 1 120); do
  grep -qa "NEXT#60" $ROOT/run/trace/trace.log 2>/dev/null && break
  sleep 1
done
echo "=== rng.log ==="
cat $ROOT/run/trace/trace.log 2>/dev/null | head -60
