#!/usr/bin/env bash
# quicdump_round.sh — run the REAL client under gdb and dump the exact
# ngtcp2 settings/transport-params structs + TLS custom-ext payloads at
# ngtcp2_conn_client_new_versioned (sub_69BAE01) / add_cb (sub_638104F).
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; export RBX_ROOT="$ROOT"
SDL=$ROOT/lib/sdl3ttf/lib
RD=$ROOT/run/quicdump

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
setsid nohup env QD_DIR="$RD" gdb -p "$PID" -batch -x $ROOT/tools/quicdump.gdb >"$RD/gdb.log" 2>&1 </dev/null &
disown
sleep 8
cd ${MOCKTAIL:-/home/john/mocktail}/build && setsid nohup env LD_LIBRARY_PATH=$SDL DISPLAY=:0 \
  ./mocktail --launch-uri "roblox://placeId=1818" >/dev/null 2>&1 </dev/null &
disown
# wait for the conn dump, then give add_cb time to fire
for i in $(seq 1 150); do
  [ -f "$RD/index.txt" ] && grep -q "CONN#" "$RD/index.txt" 2>/dev/null && break
  sleep 1
done
sleep 15
echo "=== index.txt ==="
cat "$RD/index.txt" 2>/dev/null
echo "=== gdb.log (tail) ==="
tail -5 "$RD/gdb.log" 2>/dev/null
kill_all
echo "=== QUICDUMP ROUND DONE ==="
