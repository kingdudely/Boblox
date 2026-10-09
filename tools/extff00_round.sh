#!/usr/bin/env bash
# extff00_round.sh — focused gdb round: dump the TLS custom ext 0xFF00
# getter, its payload + caller chain, and the parse side (server echo).
set -u
ROOT=/home/john/RobloxInBrowser
SDL=$ROOT/lib/sdl3ttf/lib
RD=$ROOT/run/extff00

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
cd /home/john/mocktail/build
setsid nohup env LD_LIBRARY_PATH=$SDL DISPLAY=:0 ./mocktail >/dev/null 2>&1 </dev/null &
disown
PID=""
for i in $(seq 1 120); do PID=$(engine_pid); [ -n "$PID" ] && break; sleep 1; done
[ -z "$PID" ] && { echo "no engine"; exit 1; }
echo "engine=$PID"
setsid nohup env QD_DIR="$RD" gdb -p "$PID" -batch -x $ROOT/tools/extff00.gdb >"$RD/gdb.log" 2>&1 </dev/null &
disown
sleep 8
cd /home/john/mocktail/build && setsid nohup env LD_LIBRARY_PATH=$SDL DISPLAY=:0 \
  ./mocktail --launch-uri "roblox://placeId=1818" >/dev/null 2>&1 </dev/null &
disown
# wait for GETTER lines (game conns start ~9s after launch-uri)
for i in $(seq 1 150); do
  [ -f "$RD/index.txt" ] && grep -q "PAYLOAD#" "$RD/index.txt" 2>/dev/null && break
  sleep 1
done
sleep 12
echo "=== index.txt ==="
cat "$RD/index.txt" 2>/dev/null
echo "=== gdb.log tail ==="
tail -5 "$RD/gdb.log" 2>/dev/null
kill_all
echo "=== EXTFF00 ROUND DONE ==="
