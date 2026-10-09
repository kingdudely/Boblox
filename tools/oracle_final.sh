#!/usr/bin/env bash
# oracle_final.sh — freeze-first + followUserId + blob-match-retry pipeline.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; export RBX_ROOT="$ROOT"
RUN=$ROOT/run
SDL=$ROOT/lib/sdl3ttf/lib
CHAL=$RUN/oracle_challenge.bin
ANS=$RUN/oracle_answer.txt
GLOG=$RUN/oracle_gdb.log
ts() { date +%H:%M:%S; }

rm -f "$CHAL" "$ANS" "$GLOG" "$RUN"/p10_f*.log

echo "[$(ts)] kill old processes"
for g in $(pgrep -f "gdb -p"); do kill -9 $g 2>/dev/null; done
for q in $(pgrep -x mocktail) $(pgrep -x Main); do kill -9 $q 2>/dev/null; done
sleep 3

engine_pid() {
  for d in /proc/[0-9]*; do
    p=${d#/proc/}
    [ -r "$d/maps" ] || continue
    [ "$(stat -c %u "$d" 2>/dev/null)" = "$(id -u)" ] || continue
    if grep -q "libroblox.so" "$d/maps" 2>/dev/null; then
      echo "$p"; return 0
    fi
  done
  return 1
}

echo "[$(ts)] start mocktail idle"
cd ${MOCKTAIL:-/home/john/mocktail}/build
setsid nohup env LD_LIBRARY_PATH=$SDL DISPLAY=:0 ./mocktail >/dev/null 2>&1 </dev/null &
disown
RP=""
for i in $(seq 1 150); do
  RP=$(engine_pid)
  [ -n "$RP" ] && break
  sleep 1
done
[ -z "$RP" ] && { echo "FAIL: mocktail"; exit 1; }
echo "[$(ts)] mocktail engine=$RP"

echo "[$(ts)] attach oracle gdb"
setsid nohup gdb -p "$RP" -batch -x $ROOT/tools/oracle_patch.gdb >/dev/null 2>&1 </dev/null &
for i in $(seq 1 60); do grep -qa "oracle bp set" "$GLOG" 2>/dev/null && break; sleep 1; done
grep -a "oracle bp set" "$GLOG" | head -1 || { echo "FAIL: gdb"; exit 1; }

echo "[$(ts)] trigger native join -> freeze"
cd ${MOCKTAIL:-/home/john/mocktail}/build && timeout 40 env LD_LIBRARY_PATH=$SDL DISPLAY=:0 \
  ./mocktail --launch-uri "roblox://placeId=1818" >/dev/null 2>&1 &
disown
for i in $(seq 1 120); do grep -qa "native challenge msg at" "$GLOG" 2>/dev/null && break; sleep 1; done
grep -qa "native challenge msg at" "$GLOG" || { echo "FAIL: no freeze"; exit 1; }
echo "[$(ts)] NATIVE FROZEN"

# The gdb discards challenge files whose blob doesn't match the native's.
# Retry the python client until its blob matches (same server via followUserId).
echo "[$(ts)] retry python client until blob match + answer"
for attempt in 1 2 3 4 5 6 7 8; do
  echo "[$(ts)] attempt $attempt"
  RBX_COOKIE_FILE=$RUN/cookie2.txt timeout 100 python3 -u $ROOT/py/probe10.py \
      --seconds 45 --follow 4656429295 > "$RUN/p10_f$attempt.log" 2>&1 &
  P=$!
  for i in $(seq 1 40); do
    [ -f "$ANS" ] && break
    kill -0 $P 2>/dev/null || break
    sleep 1
  done
  if [ -f "$ANS" ]; then
    echo "[$(ts)] ANSWER: $(cat "$ANS")"
    # wait for the python client to send it
    wait $P 2>/dev/null
    grep -aE "ORACLE|challenge=|resets=|terminated|RX chan1" "$RUN/p10_f$attempt.log" | tail -8
    break
  fi
  grep -a "blobcheck" "$GLOG" | tail -1
  wait $P 2>/dev/null
done
echo "[$(ts)] gdb trace:"
grep -a "blobcheck\|patching\|captured answer\|killing" "$GLOG" | tail -6
echo "[$(ts)] done"
