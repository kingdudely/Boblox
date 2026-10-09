#!/usr/bin/env bash
# oracle_run2.sh — fast challenge-oracle pipeline (freeze-first ordering).
#
#  1. Start mocktail idle, attach oracle_patch.gdb.
#  2. Trigger mocktail join -> native receives its 0x9B challenge ->
#     gdb FREEZES it at the dispatcher and waits for our challenge file.
#  3. Launch the Python client (account 2) -> joins, receives 0x9B challenge,
#     writes oracle_challenge.bin.
#  4. gdb patches the native's u1/u2 to ours, resumes -> native computes the
#     answer -> gdb writes oracle_answer.txt.
#  5. Python client sends the answer within ~1s of receiving its challenge.
#
set -u

COOKIE2=${COOKIE2:-/home/john/RobloxInBrowser/run/cookie2.txt}
CHAL=/home/john/RobloxInBrowser/run/oracle_challenge.bin
ANS=/home/john/RobloxInBrowser/run/oracle_answer.txt
P10LOG=/home/john/RobloxInBrowser/run/p10_run.log
GDBLOG=/home/john/RobloxInBrowser/run/oracle_gdb.log
LAUNCHLOG=/home/john/RobloxInBrowser/run/oracle_launch.log

rm -f "$CHAL" "$ANS" "$P10LOG" "$GDBLOG" "$LAUNCHLOG"

ts() { date +%H:%M:%S; }

echo "[$(ts)] == start mocktail idle =="
for q in $(pgrep -x mocktail); do kill -9 $q 2>/dev/null; done
sleep 2
cd /home/john/mocktail/build
setsid nohup env LD_LIBRARY_PATH=/home/john/RobloxInBrowser/lib/sdl3ttf/lib DISPLAY=:0 ./mocktail >/dev/null 2>&1 </dev/null &
disown
RP=""
for i in $(seq 1 60); do
  for q in $(pgrep -x mocktail); do
    grep -q "libroblox.so" /proc/$q/maps 2>/dev/null && RP=$q
  done
  [ -n "$RP" ] && break
  sleep 1
done
[ -z "$RP" ] && { echo "FAIL: mocktail not up"; exit 1; }
echo "[$(ts)] mocktail pid=$RP"

echo "[$(ts)] == attach oracle gdb =="
setsid nohup gdb -p "$RP" -batch -x /home/john/RobloxInBrowser/tools/oracle_patch.gdb >"$GDBLOG" 2>&1 </dev/null &
disown
sleep 6
grep -a "oracle bp set" "$GDBLOG" | head -1 || { echo "FAIL: gdb not armed"; exit 1; }

echo "[$(ts)] == trigger mocktail join =="
cd /home/john/mocktail/build
timeout 30 env LD_LIBRARY_PATH=/home/john/RobloxInBrowser/lib/sdl3ttf/lib DISPLAY=:0 \
  ./mocktail --launch-uri "roblox://placeId=1818" >"$LAUNCHLOG" 2>&1 &
disown

echo "[$(ts)] == wait for native challenge freeze =="
FROZEN=0
for i in $(seq 1 90); do
  if grep -qa "native challenge msg at" "$GDBLOG"; then FROZEN=1; break; fi
  sleep 1
done
if [ "$FROZEN" != 1 ]; then echo "FAIL: native challenge not reached"; tail -5 "$GDBLOG"; exit 1; fi
echo "[$(ts)] native frozen; launching python client NOW"

echo "[$(ts)] == probe10 (account 2) =="
cd /home/john/RobloxInBrowser/py
RBX_COOKIE_FILE="$COOKIE2" nohup python3 -u probe10.py --seconds 90 >"$P10LOG" 2>&1 &
P10PID=$!

echo "[$(ts)] == wait for oracle answer =="
for i in $(seq 1 60); do
  [ -f "$ANS" ] && break
  sleep 1
done
if [ -f "$ANS" ]; then echo "[$(ts)] ANSWER: $(cat "$ANS")"; else echo "[$(ts)] no answer"; tail -10 "$GDBLOG"; fi

echo "[$(ts)] == wait for python client =="
wait $P10PID 2>/dev/null
echo "[$(ts)] == python log =="
grep -aE "CHALLENGE|ORACLE|resets=|assigned|peer|replicat" "$P10LOG" | head -20
echo "[$(ts)] == done =="
