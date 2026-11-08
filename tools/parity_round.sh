#!/usr/bin/env bash
# parity_round.sh — run the native client and ours in the SAME job, then diff.
#
#   PARITY_SECS=676 tools/parity_round.sh        # the full 11-minute window
#   PARITY_SECS=90  tools/parity_round.sh        # smoke round
#
# The native client under gdb picks its own job via matchmaking, so the round
# is: start the native capture, read the job id out of its join line, then pin
# our client to that same job with --job-id.  Both connections are captured as
# plaintext RakNet datagrams (native `dgram_*.bin` from the seal hook, ours
# `tNNNNNN.bin` from RBX_RAKNET_TXDUMP) and compared with tools/tx_diff.py.
#
# Accounts: the server drops the older session when the same account launches
# another client, so two *simultaneous* sessions cannot share one account —
# measured, not assumed: the native logs `Disconnect reason received: 276` /
# "joined a game from another device" 8.5 s after ours connected.  Two modes:
#
#   PARITY_SEQUENTIAL=0 (default)  two accounts, both live at once, same job.
#   PARITY_SEQUENTIAL=1            ONE account and one job: the native runs its
#                                  window first, leaves, then ours joins the
#                                  same job with the same cookie.  Per-account
#                                  values (player id, character refs) are then
#                                  the same on both sides, which is what a
#                                  byte-level diff needs.
#
# Env:
#   PARITY_SECS      session window in seconds (default 676)
#   PARITY_PLACE     place id (default 1818)
#   PARITY_DIR       output root (default run/parity/round)
#   PARITY_SEQUENTIAL 1 = same account, same job, one after the other
#   NATIVE_COOKIE    file holding .ROBLOSECURITY for the native (default run/cookie.txt)
#   OUR_COOKIE       file holding .ROBLOSECURITY for ours    (default ~/king.txt,
#                    or NATIVE_COOKIE when PARITY_SEQUENTIAL=1)
#   NATIVE_CLIENT    native launcher (default /home/john/Rum/build/roblox)
#   OUR_CLIENT       our client     (default build/client/roblox)
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SECS=${PARITY_SECS:-676}
PLACE=${PARITY_PLACE:-1818}
OUT=${PARITY_DIR:-$ROOT/run/parity/round}
SEQUENTIAL=${PARITY_SEQUENTIAL:-0}
NATIVE_COOKIE=${NATIVE_COOKIE:-$ROOT/run/cookie.txt}
if [ "$SEQUENTIAL" = "1" ]; then
  OUR_COOKIE=${OUR_COOKIE:-$NATIVE_COOKIE}
else
  OUR_COOKIE=${OUR_COOKIE:-$HOME/king.txt}
fi
OUR_CLIENT=${OUR_CLIENT:-$ROOT/build/client/roblox}
NATIVE_CLIENT=${NATIVE_CLIENT:-/home/john/Rum/build/roblox}

[ -r "$NATIVE_COOKIE" ] || { echo "missing native cookie: $NATIVE_COOKIE" >&2; exit 1; }
[ -r "$OUR_COOKIE" ]    || { echo "missing our cookie: $OUR_COOKIE" >&2; exit 1; }
[ -x "$OUR_CLIENT" ]    || { echo "missing our client: $OUR_CLIENT" >&2; exit 1; }

rm -rf "$OUT"; mkdir -p "$OUT/native" "$OUT/ours"
echo "round: place=$PLACE secs=$SECS out=$OUT"

# --- native --------------------------------------------------------------
DISPLAY=${DISPLAY:-:0} RC_DIR="$OUT/native" RC_SECS=$((SECS + 120)) \
  RC_HOOKS=dgram RC_PLACE="$PLACE" RC_CLIENT="$NATIVE_CLIENT" \
  RC_COOKIE="$(tr -d '\r\n' < "$NATIVE_COOKIE")" \
  setsid nohup bash "$ROOT/tools/gdb/rc_capture.sh" \
  >"$OUT/native/driver.log" 2>&1 </dev/null &
NDRIVER=$!
echo "native driver pid=$NDRIVER (capture window $((SECS + 120))s)"

JOB=""
for _ in $(seq 1 180); do
  JOB=$(sed -n "s/.*Joining game '\([0-9a-f-]\{36\}\)'.*/\1/p" \
        "$OUT/native/gdb.log" 2>/dev/null | head -1)
  [ -n "$JOB" ] && break
  sleep 1
done
if [ -z "$JOB" ]; then
  echo "native never joined within 180s; see $OUT/native/gdb.log" >&2
  kill -INT "$NDRIVER" 2>/dev/null
  exit 2
fi
echo "native joined job $JOB"

# --- sequential: let the native finish and leave the job first ------------
if [ "$SEQUENTIAL" = "1" ]; then
  echo "sequential mode: waiting for the native driver to finish"
  for _ in $(seq 1 $((SECS + 180))); do
    kill -0 "$NDRIVER" 2>/dev/null || break
    sleep 1
  done
  kill -INT "$NDRIVER" 2>/dev/null
  sleep 5
  kill -9 "$NDRIVER" 2>/dev/null
  echo "native left job $JOB; ours joins it with the same account"
fi

# --- ours, pinned to the native's job ------------------------------------
(
  cd "$ROOT"
  export RBX_RAKNET_TXDUMP="$OUT/ours"
  exec timeout --signal=INT --kill-after=20 "$SECS" "$OUR_CLIENT" \
    --connection-type=RakNet --place-id "$PLACE" --job-id "$JOB" \
    --roblosecurity "$(tr -d '\r\n' < "$OUR_COOKIE")" --headless
) >"$OUT/ours.log" 2>&1 &
OPID=$!
echo "ours pid=$OPID (window ${SECS}s)"

# --- wait for both -------------------------------------------------------
wait "$OPID" 2>/dev/null
echo "ours exited rc=$?"
# let the native run out its (longer) window, then stop it deterministically
for _ in $(seq 1 $((SECS + 150))); do
  kill -0 "$NDRIVER" 2>/dev/null || break
  sleep 1
done
kill -INT "$NDRIVER" 2>/dev/null
sleep 5
kill -9 "$NDRIVER" 2>/dev/null

# find, not `ls glob`: a 676 s round writes ~25k datagram files per side and
# the expanded argv overflows ARG_MAX, which silently reported 0.
NAT=$(find "$OUT/native" -maxdepth 1 -name 'dgram_*.bin' 2>/dev/null | wc -l)
OURS=$(find "$OUT/ours" -maxdepth 1 -name 't*.bin' 2>/dev/null | wc -l)
echo "captured: native=$NAT datagrams  ours=$OURS datagrams"
echo "logs: $OUT/native/rkc.log  $OUT/native/gdb.log  $OUT/ours.log"

python3 "$ROOT/tools/tx_diff.py" --ours "$OUT/ours" --native "$OUT/native" \
  --epoch game >"$OUT/diff.txt" 2>&1
RC=$?
tail -n 40 "$OUT/diff.txt"
echo "full diff: $OUT/diff.txt (exit $RC)"
exit $RC
