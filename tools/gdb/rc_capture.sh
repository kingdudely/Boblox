#!/usr/bin/env bash
# rc_capture.sh — run the native Roblox client under gdb with rc_capture.gdb and
# collect every session/join artefact into $RC_DIR.
#
#   RC_DIR=/tmp/rc1 RC_SECS=180 tools/gdb/rc_capture.sh
#
# Env:
#   RC_DIR      output directory           (default /tmp/rc_capture)
#   RC_SECS     capture window in seconds  (default 180)
#   RC_COOKIE   .ROBLOSECURITY value       (default: contents of $HOME/king.txt)
#   RC_PLACE    place id                   (default 1818)
#   RC_CLIENT   client binary              (default /home/john/Rum/build/roblox)
#   RC_LIB      libroblox.so               (default /home/john/rr/libroblox.so)
#   RC_ASSETS   assets root                (default /home/john/rr/assets)
#   RC_CWD      working directory          (default the client binary's dir)
#
# Note: the Roblox client maps libroblox.so with its own loader, so breakpoints
# cannot be armed until the library is mapped; rc_capture.gdb polls for it
# (the join happens ~3 s in, well before a post-hoc `gdb -p` attach could arm).
set -u

RC_DIR=${RC_DIR:-/tmp/rc_capture}
RC_SECS=${RC_SECS:-180}
RC_COOKIE=${RC_COOKIE:-$(tr -d '\r\n' < "$HOME/king.txt" 2>/dev/null)}
RC_PLACE=${RC_PLACE:-1818}
RC_CLIENT=${RC_CLIENT:-/home/john/Rum/build/roblox}
RC_LIB=${RC_LIB:-/home/john/rr/libroblox.so}
RC_ASSETS=${RC_ASSETS:-/home/john/rr/assets}
RC_CWD=${RC_CWD:-$(dirname "$RC_CLIENT")}
# resolve the script path before we cd into the client directory
GDB_SCRIPT=${GDB_SCRIPT:-$(cd "$(dirname "$0")" && pwd)/rc_capture.gdb}
[ -r "$GDB_SCRIPT" ] || { echo "gdb script not found: $GDB_SCRIPT" >&2; exit 1; }

rm -rf "$RC_DIR"; mkdir -p "$RC_DIR"

cd "$RC_CWD" || exit 1
export DISPLAY=${DISPLAY:-:0}
export RKC_DIR="$RC_DIR" RKC_COOKIE="$RC_COOKIE" RKC_PLACE="$RC_PLACE"
export RKC_LIB="$RC_LIB" RKC_ASSETS="$RC_ASSETS"

setsid nohup gdb -batch -x "$GDB_SCRIPT" "$RC_CLIENT" >"$RC_DIR/gdb.log" 2>&1 </dev/null &
GPID=$!
echo "gdb pid=$GPID  (window ${RC_SECS}s)"

sleep "$RC_SECS"

# stop gdb, then the client it owns (never kill by name: this process tree
# contains the words we would match on)
CHILD=$(pgrep -P "$GPID" | head -1)
kill -INT "$GPID" 2>/dev/null; sleep 3
kill -9 "$GPID" 2>/dev/null
if [ -n "${CHILD:-}" ]; then
  kill -TERM "$CHILD" 2>/dev/null; sleep 2; kill -9 "$CHILD" 2>/dev/null
fi
echo "done: wire=$(ls "$RC_DIR"/wire_tx_*.bin 2>/dev/null | wc -l) dgram=$(ls "$RC_DIR"/dgram_*.bin 2>/dev/null | wc -l) log=$RC_DIR/rkc.log"
