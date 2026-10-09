#!/usr/bin/env bash
# session_round.sh — run the REAL client (mocktail) through a full join of
# place 1818 under a gdb session capture (TX + RX + 0x9B challenge markers).
# Ground truth for what the server sends/expects around the challenge.
set -u
ROOT=/home/john/RobloxInBrowser
SDL=$ROOT/lib/sdl3ttf/lib
RD=$ROOT/run/sessioncap

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
setsid nohup env SCAP_DIR="$RD" gdb -p "$PID" -batch -x $ROOT/tools/session_capture.gdb >"$RD/gdb.log" 2>&1 </dev/null &
disown
sleep 8
cd /home/john/mocktail/build && setsid nohup env LD_LIBRARY_PATH=$SDL DISPLAY=:0 \
  ./mocktail --launch-uri "roblox://placeId=1818" >/dev/null 2>&1 </dev/null &
disown
for i in $(seq 1 150); do
  [ -f "$RD/index.txt" ] && grep -q "MSG9B" "$RD/index.txt" 2>/dev/null && sleep 6 && break
  sleep 1
done
SL=$(ls -t /home/john/.local/state/mocktail/logs/sessions/*.log 2>/dev/null | head -1)
grep -aoP "Joining game '\K[0-9a-f-]+" "$SL" 2>/dev/null | tail -1 > "$RD/jobid.txt" || true
echo "=== index.txt ==="
cat "$RD/index.txt" 2>/dev/null
echo "=== jobid: $(cat $RD/jobid.txt 2>/dev/null)"
kill_all
echo "=== SESSION ROUND DONE ==="
