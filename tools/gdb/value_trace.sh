#!/usr/bin/env bash
# value_trace.sh — run the native client under value_trace.gdb and collect
# the per-tag value-extent log.
#
#   VT_DIR=/tmp/vt1 VT_SECS=60 tools/gdb/value_trace.sh
#
# Env: VT_DIR (output dir), VT_SECS (capture window), VT_COOKIE, VT_PLACE,
#      VT_CLIENT, VT_LIB, VT_ASSETS, GDB_SCRIPT.
set -u

VT_DIR=${VT_DIR:-/tmp/value_trace}
VT_SECS=${VT_SECS:-60}
VT_COOKIE=${VT_COOKIE:-$(tr -d '\r\n' < "$HOME/king.txt" 2>/dev/null)}
VT_PLACE=${VT_PLACE:-1818}
VT_CLIENT=${VT_CLIENT:-/home/john/Rum/build/roblox}
VT_LIB=${VT_LIB:-/home/john/rr/libroblox.so}
VT_ASSETS=${VT_ASSETS:-/home/john/rr/assets}
VT_CWD=${VT_CWD:-$(dirname "$VT_CLIENT")}
GDB_SCRIPT=${GDB_SCRIPT:-$(cd "$(dirname "$0")" && pwd)/value_trace.gdb}
[ -r "$VT_CLIENT" ] || { echo "client not found: $VT_CLIENT" >&2; exit 1; }
[ -r "$GDB_SCRIPT" ] || { echo "gdb script not found: $GDB_SCRIPT" >&2; exit 1; }

rm -rf "$VT_DIR"; mkdir -p "$VT_DIR"
cd "$VT_CWD" || exit 1
export DISPLAY=${DISPLAY:-:0}
export RKC_DIR="$VT_DIR" RKC_COOKIE="$VT_COOKIE" RKC_PLACE="$VT_PLACE"
export RKC_LIB="$VT_LIB" RKC_ASSETS="$VT_ASSETS"

setsid nohup gdb -batch -x "$GDB_SCRIPT" "$VT_CLIENT" >"$VT_DIR/gdb.log" 2>&1 </dev/null &
GPID=$!
echo "gdb pid=$GPID (window ${VT_SECS}s) dir=$VT_DIR"

sleep "$VT_SECS"

CHILD=$(pgrep -P "$GPID" | head -1)
kill -INT "$GPID" 2>/dev/null
sleep 2
[ -n "$CHILD" ] && kill -9 "$CHILD" 2>/dev/null
sleep 1
kill -9 "$GPID" 2>/dev/null
echo "vt lines: $(grep -c '^VT ' "$VT_DIR/vt.log" 2>/dev/null || echo 0)"
tail -5 "$VT_DIR/vt.log" 2>/dev/null
