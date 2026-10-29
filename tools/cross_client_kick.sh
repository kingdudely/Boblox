#!/usr/bin/env bash
# cross_client_kick.sh — prove whether our RakNet client displaces a running
# native client the same way a second native session would.
#
# What it does, in order:
#   1. launches the native client (Rum wrapper + libroblox.so) on --native-place,
#      detached with setsid so it survives this shell,
#   2. waits until it is actually in-game (network join + first replicated frame),
#   3. joins --our-place with our client under a timeout,
#   4. reads the native's log for a server disconnect and prints the verdict.
#
# A "kick" is the RCC sending the *old* session a disconnect reason after the
# web learned about the new one: 273 ("possibly due to game joined from another
# device") or 276 ("disconnected because you have joined a game from another
# device"). Roblox only re-evaluates presence when the new join is a *different
# place*; a second join to the same place is a rejoin of the same job and does
# not produce one, which is what --our-place same demonstrates.
#
# usage:
#   tools/cross_client_kick.sh                       # native 1818, ours 189707
#   tools/cross_client_kick.sh --our-place same      # both on 1818
#   tools/cross_client_kick.sh --native-place 1818 --our-place 920587237
#   tools/cross_client_kick.sh --keep                # leave the native running
#
# Environment overrides: RUM_NATIVE, RUM_PRELOAD, RUM_LIB, RUM_ASSETS,
# RBX_CLIENT (our binary), COOKIE_FILE.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
RUM_NATIVE=${RUM_NATIVE:-$HOME/Rum/build/roblox}
RUM_PRELOAD=${RUM_PRELOAD:-$HOME/Xecutor/build/libXecutor.so}
RUM_LIB=${RUM_LIB:-$HOME/rr/libroblox.so}
RUM_ASSETS=${RUM_ASSETS:-$HOME/rr/assets}
RBX_CLIENT=${RBX_CLIENT:-$ROOT/build/client/roblox}
COOKIE_FILE=${COOKIE_FILE:-$HOME/mizg.txt}

NATIVE_PLACE=1818
OUR_PLACE=189707
WAIT=90       # seconds to wait for the native to reach the DataModel
RUN=30        # seconds to keep our client connected
KEEP=0

while [ $# -gt 0 ]; do
  case "$1" in
    --native-place) NATIVE_PLACE=$2; shift 2 ;;
    --our-place)    OUR_PLACE=$2; shift 2 ;;
    --cookie-file)  COOKIE_FILE=$2; shift 2 ;;
    --wait)         WAIT=$2; shift 2 ;;
    --run)          RUN=$2; shift 2 ;;
    --keep)         KEEP=1; shift ;;
    -h|--help)      sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[ "$OUR_PLACE" = same ] && OUR_PLACE=$NATIVE_PLACE

for f in "$RUM_NATIVE" "$RUM_PRELOAD" "$RUM_LIB" "$RBX_CLIENT" "$COOKIE_FILE"; do
  if [ ! -e "$f" ]; then echo "missing: $f" >&2; exit 1; fi
done
COOKIE=$(tr -d '\r\n' < "$COOKIE_FILE")
[ -n "$COOKIE" ] || { echo "empty cookie file: $COOKIE_FILE" >&2; exit 1; }

TMP=$(mktemp -d /tmp/rbx-kick.XXXXXX)
NLOG=$TMP/native.log
OLOG=$TMP/ours.log
PIDFILE=$TMP/native.pid

cleanup() {
  if [ "$KEEP" -eq 0 ] && [ -s "$PIDFILE" ]; then
    kill -TERM "$(cat "$PIDFILE")" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "native: $RUM_NATIVE place=$NATIVE_PLACE  (log $NLOG)"
echo "ours  : $RBX_CLIENT place=$OUR_PLACE  (log $OLOG)"

# setsid + nohup + closed stdin: the app must outlive this shell. The inner
# bash records its own pid and execs the client, so the pidfile holds the app.
setsid bash -c 'echo $$ > "$0"; exec env LD_PRELOAD="$1" "$2" --lib "$3" --assets "$4" \
  --place-id "$5" --roblosecurity "$6"' \
  "$PIDFILE" "$RUM_PRELOAD" "$RUM_NATIVE" "$RUM_LIB" "$RUM_ASSETS" \
  "$NATIVE_PLACE" "$COOKIE" > "$NLOG" 2>&1 < /dev/null &
pidfile_waiter=$!

for _ in $(seq 1 "$WAIT"); do
  if grep -q "Join snapshot timer" "$NLOG" 2>/dev/null &&
     grep -q "present_sample frame=" "$NLOG" 2>/dev/null; then
    break
  fi
  if ! kill -0 "$pidfile_waiter" 2>/dev/null && [ -s "$PIDFILE" ] &&
     ! kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    echo "native exited before joining:" >&2
    tail -20 "$NLOG" >&2
    exit 1
  fi
  sleep 1
done

if ! grep -q "present_sample frame=" "$NLOG" 2>/dev/null; then
  echo "native never reached the DataModel in ${WAIT}s:" >&2
  tail -20 "$NLOG" >&2
  exit 1
fi

JOB=$(sed -n "s/.*Joining game '\([^']*\)'.*/\1/p" "$NLOG" | tail -1)
echo "native joined job ${JOB:-?}"
echo "--- joining with our client for ${RUN}s ---"

timeout "$RUN" "$RBX_CLIENT" --place-id "$OUR_PLACE" \
  --roblosecurity "$COOKIE" --headless > "$OLOG" 2>&1
OURS_RC=$?

OURS_JOB=$(sed -n 's/.*join: OK job=\(.*\)$/\1/p' "$OLOG" | tail -1)
if grep -q "0x97" "$OLOG"; then
  OURS_STATE="reached 0x97"
elif grep -q "live:" "$OLOG"; then
  OURS_STATE="replicating"
else
  OURS_STATE="did NOT join (rc=$OURS_RC)"
fi
echo "ours  : job ${OURS_JOB:-?} — $OURS_STATE"

echo "--- native after our join ---"
REASON=$(sed -n 's/.*Disconnection Notification\. Reason: \([0-9]*\).*/\1/p' "$NLOG" | tail -1)
BANNER=$(grep -aE "Client has been disconnected with reason:" "$NLOG" | tail -1 | sed 's/.*reason: //')
if grep -qaE "joined a game from another device|joined from another device" "$NLOG"; then
  echo "VERDICT: KICKED — reason ${REASON:-?}: $BANNER"
  exit 0
fi
if [ -n "$REASON" ]; then
  echo "VERDICT: disconnected, but not a cross-device kick — reason $REASON: $BANNER"
  exit 1
fi
if grep -q "present_sample frame=" <(tail -3 "$NLOG"); then
  echo "VERDICT: NOT KICKED — native is still in-game"
else
  echo "VERDICT: no kick in the native log (native may have exited)"
fi
exit 1
