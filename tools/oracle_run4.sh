#!/usr/bin/env bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; export RBX_ROOT="$ROOT"
# oracle_run4.sh — warm native re-join + kill-on-send.
set -u
COOKIE2=${COOKIE2:-$ROOT/run/cookie2.txt}
CHAL=$ROOT/run/oracle_challenge.bin
ANS=$ROOT/run/oracle_answer.txt
P10LOG=$ROOT/run/p10_run4.log
GLOG=$ROOT/run/oracle_gdb.log
ts() { date +%H:%M:%S; }

rm -f "$CHAL" "$ANS" "$P10LOG"
: > "$GLOG"

echo "[$(ts)] start mocktail idle"
for q in $(pgrep -x mocktail); do kill -9 $q 2>/dev/null; done
sleep 2
cd ${MOCKTAIL:-/home/john/mocktail}/build
setsid nohup env LD_LIBRARY_PATH=$ROOT/lib/sdl3ttf/lib DISPLAY=:0 ./mocktail >/dev/null 2>&1 </dev/null &
disown
RP=""
for i in $(seq 1 90); do
  for q in $(pgrep -x mocktail); do
    grep -q "libroblox.so" /proc/$q/maps 2>/dev/null && RP=$q
  done
  [ -n "$RP" ] && break
  sleep 1
done
[ -z "$RP" ] && { echo "FAIL: mocktail"; exit 1; }
echo "[$(ts)] mocktail=$RP"

echo "[$(ts)] attach oracle gdb"
setsid nohup gdb -p "$RP" -batch -x $ROOT/tools/oracle_patch.gdb >"/dev/null" 2>&1 </dev/null &
disown
for i in $(seq 1 30); do grep -qa "oracle bp set" "$GLOG" && break; sleep 1; done
grep -a "oracle bp set" "$GLOG" | head -1 || { echo "FAIL: gdb"; exit 1; }

echo "[$(ts)] python client (acct2) joins and holds challenge"
cd $ROOT/py
RBX_COOKIE_FILE="$COOKIE2" nohup python3 -u probe10.py --seconds 300 >"$P10LOG" 2>&1 &
P10PID=$!
for i in $(seq 1 90); do [ -f "$CHAL" ] && break; sleep 1; done
[ -f "$CHAL" ] || { echo "FAIL: no challenge"; kill $P10PID; exit 1; }
JOB=$(grep -oP 'joined job=\K[0-9a-f-]+' "$P10LOG" | head -1)
echo "[$(ts)] challenge captured; JOB=$JOB"

echo "[$(ts)] forward join to warm native for the SAME job"
cd ${MOCKTAIL:-/home/john/mocktail}/build
timeout 30 env LD_LIBRARY_PATH=$ROOT/lib/sdl3ttf/lib DISPLAY=:0 \
  ./mocktail --launch-uri "roblox://placeId=1818&gameInstanceId=$JOB" >/dev/null 2>&1 &
disown

for i in $(seq 1 90); do [ -f "$ANS" ] && break; sleep 1; done
if [ -f "$ANS" ]; then
  echo "[$(ts)] ANSWER: $(cat "$ANS")"
else
  echo "[$(ts)] no answer; gdb log:"; grep -a "blobcheck\|patching\|captured\|native challenge" "$GLOG" | tail -5
fi

wait $P10PID 2>/dev/null
echo "[$(ts)] python result:"
grep -aE "CHALLENGE|ORACLE|challenge=|resets=|terminated" "$P10LOG" | tail -6
