#!/usr/bin/env bash
# oracle_run.sh — full challenge-oracle pipeline (needs 2 accounts).
#
#   1. Python client (account 2) joins 1818, receives 0x9B challenge,
#      writes /home/john/RobloxInBrowser/run/oracle_challenge.bin, then waits for the answer.
#   2. mocktail (account 1) starts fresh; oracle_patch.gdb attached.
#   3. mocktail joins the SAME server (gameInstanceId from step 1).
#   4. gdb patches the native's challenge u1/u2 to ours, captures its answer.
#   5. Python client sends the answer to the server.
#
set -u

COOKIE2=${COOKIE2:-/home/john/RobloxInBrowser/run/cookie2.txt}
CHAL=/home/john/RobloxInBrowser/run/oracle_challenge.bin
ANS=/home/john/RobloxInBrowser/run/oracle_answer.txt
P10LOG=/home/john/RobloxInBrowser/run/p10_run.log
GDBLOG=/home/john/RobloxInBrowser/run/oracle_gdb.log

rm -f "$CHAL" "$ANS" "$P10LOG" "$GDBLOG"

echo "== step 1: python client (account 2) =="
cd /home/john/RobloxInBrowser/py
RBX_COOKIE_FILE="$COOKIE2" nohup python3 -u probe10.py --seconds 240 >"$P10LOG" 2>&1 &
P10PID=$!
echo "probe10 pid=$P10PID"

# wait for join (the log line has the job id)
JOB=""
for i in $(seq 1 60); do
  JOB=$(grep -oP 'joined job=\K[0-9a-f-]+' "$P10LOG" 2>/dev/null | head -1)
  [ -n "$JOB" ] && break
  sleep 1
done
if [ -z "$JOB" ]; then echo "FAIL: no join"; cat "$P10LOG"; exit 1; fi
echo "job=$JOB"

# wait for the challenge to arrive
for i in $(seq 1 120); do
  [ -f "$CHAL" ] && break
  sleep 1
done
if [ ! -f "$CHAL" ]; then echo "FAIL: no challenge"; tail -5 "$P10LOG"; kill $P10PID; exit 1; fi
echo "challenge captured: $(xxd -p "$CHAL" | head -c 40)..."

echo "== step 2: start mocktail (account 1) fresh =="
for q in $(pgrep -x mocktail); do kill -9 $q 2>/dev/null; done
sleep 3
cd /home/john/mocktail/build
setsid nohup env LD_LIBRARY_PATH=/home/john/RobloxInBrowser/lib/sdl3ttf/lib DISPLAY=:0 ./mocktail >/dev/null 2>&1 </dev/null &
disown
echo "waiting for mocktail..."
RP=""
for i in $(seq 1 60); do
  for q in $(pgrep -x mocktail); do
    grep -q "libroblox.so" /proc/$q/maps 2>/dev/null && RP=$q
  done
  [ -n "$RP" ] && break
  sleep 1
done
if [ -z "$RP" ]; then echo "FAIL: mocktail not up"; kill $P10PID; exit 1; fi
echo "mocktail pid=$RP"

echo "== step 3: attach oracle gdb, trigger join to $JOB =="
setsid nohup gdb -p "$RP" -batch -x /home/john/RobloxInBrowser/tools/oracle_patch.gdb >"$GDBLOG" 2>&1 </dev/null &
disown
sleep 6
grep -a "oracle bp set" "$GDBLOG" | head -1

cd /home/john/mocktail/build
timeout 30 env LD_LIBRARY_PATH=/home/john/RobloxInBrowser/lib/sdl3ttf/lib DISPLAY=:0 \
  ./mocktail --launch-uri "roblox://placeId=1818&gameInstanceId=$JOB" >/home/john/RobloxInBrowser/run/oracle_launch.log 2>&1
echo "join sent"

echo "== step 4: wait for oracle answer =="
for i in $(seq 1 90); do
  [ -f "$ANS" ] && break
  sleep 1
done
if [ -f "$ANS" ]; then
  echo "ANSWER: $(cat "$ANS")"
else
  echo "no answer captured; gdb log:"
  tail -20 "$GDBLOG"
fi

echo "== step 5: wait for python client to finish =="
wait $P10PID 2>/dev/null
echo "== python log tail =="
tail -15 "$P10LOG"
