#!/usr/bin/env bash
# hybrid_run.sh — one farm instance: the real engine owns the character, the
# harness owns everything else (cookie, display, autoexec, logs).
#   tools/hybrid_run.sh <account> <place> [autoexec.lua]
# Engine cost measured: 854 MB (small place, real display), 941 MB (640x480
# Xvfb), 1059 MB (populated server), 1420 MB (worst observed). The graphics
# backend is NOT the cost: --graphics opengl under Xvfb is the stable choice
# (direct-vulkan errors out on software Vulkan).
set -u
ACCT="${1:?account}"; PLACE="${2:-189707}"; LUA="${3:-}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISP=":$((80 + RANDOM % 100))"
OUT="$ROOT/run/hybrid-$ACCT"
mkdir -p "$OUT" "$HOME/Documents/UwUcutor/autoexec"
[ -n "$LUA" ] && cp "$LUA" "$HOME/Documents/UwUcutor/autoexec/farm.lua"
Xvfb "$DISP" -screen 0 640x480x24 >/dev/null 2>&1 &
XPID=$!; sleep 2
echo "instance=$ACCT place=$PLACE display=$DISP log=$OUT/engine.log"
ROBLOX_ROBLOSECURITY="$(cat "$HOME/.config/$ACCT-cookie")" DISPLAY="$DISP" \
LD_PRELOAD="$HOME/work/UwUcutor/build/libUwUcutor.so" \
  "$HOME/work/Rum/build/roblox" --lib "$HOME/work/rr/libroblox.so" \
  --assets "$HOME/work/rr/assets" --graphics opengl \
  --place-id "$PLACE" > "$OUT/engine.log" 2>&1
kill "$XPID" 2>/dev/null
