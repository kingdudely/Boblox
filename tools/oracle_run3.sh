#!/usr/bin/env bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; export RBX_ROOT="$ROOT"
# oracle_run3.sh — FREEZE-FIRST pipeline (correct order).
#
#  1. mocktail (account 1) starts IDLE; attach oracle_patch.gdb.
#  2. Trigger native join -> native receives 0x9B challenge ->
#     gdb FREEZES it at dispatcher, waits (up to 10 min) for our challenge file.
#  3. Python client (account 2) joins -> receives its 0x9B challenge within seconds
#     and writes oracle_challenge.bin.
#  4. gdb patches the native's u1/u2 -> native computes answer -> gdb writes
#     oracle_answer.txt.
#  5. Python client sends the answer immediately.
#
set -u
COOKIE2=${COOKIE2:-$ROOT/run/cookie2.txt}
CHAL=$ROOT/run/oracle_challenge.bin
ANS=$ROOT/run/oracle_answer.txt
P10LOG=$ROOT/run/p10_run.log
GLOG=$ROOT/run/oracle_gdb_final.log
ts() { date +%H:%M:%S; }

rm -f "$CHAL" "$ANS" "$P10LOG" "$GLOG"

echo "[$(ts)] start mocktail (idle)"
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
setsid nohup gdb -p "$RP" -batch -x $ROOT/tools/oracle_patch.gdb >"$GLOG" 2>&1 </dev/null &
disown
for i in $(seq 1 30); do
  grep -qa "oracle bp set" "$GLOG" && break
  sleep 1
done
grep -a "oracle bp set" "$GLOG" | head -1 || { echo "FAIL: gdb"; exit 1; }

echo "[$(ts)] trigger native join (will freeze at challenge)"
cd ${MOCKTAIL:-/home/john/mocktail}/build
timeout 30 env LD_LIBRARY_PATH=$ROOT/lib/sdl3ttf/lib DISPLAY=:0 \
  ./mocktail --launch-uri "roblox://placeId=1818" >/dev/null 2>&1 &
disown
for i in $(seq 1 90); do
  grep -qa "native challenge msg at" "$GLOG" && break
  sleep 1
done
if ! grep -qa "native challenge msg at" "$GLOG"; then echo "FAIL: no freeze"; tail -10 "$GLOG"; exit 1; fi
echo "[$(ts)] native FROZEN"

echo "[$(ts)] start python client (account 2)"
cd $ROOT/py
RBX_COOKIE_FILE="$COOKIE2" nohup python3 -u probe10.py --seconds 240 >"$P10LOG" 2>&1 &
P10PID=$!

# wait until python has the challenge file written
for i in $(seq 1 90); do
  [ -f "$CHAL" ] && break
  sleep 1
done
[ -f "$CHAL" ] || { echo "FAIL: python never got challenge"; grep -a challenge= "$P10LOG"; kill $P10PID; exit 1; }
echo "[$(ts)] python challenge captured"

for i in $(seq 1 90); do
  [ -f "$ANS" ] && break
  sleep 1
done
if [ -f "$ANS" ]; then
  echo "[$(ts)] ORACLE ANSWER: $(cat "$ANS")"
else
  echo "[$(ts)] no oracle answer; gdb log tail:"; grep -a "patching\|captured\|no oracle" "$GLOG" | tail -5; tail -6 "$GLOG"
fi

wait $P10PID 2>/dev/null
echo "[$(ts)] python result:"
grep -aE "CHALLENGE|ORACLE|challenge=|resets=|terminated" "$P10LOG" | tail -8
echo "[$(ts)] done"
