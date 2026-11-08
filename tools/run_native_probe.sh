#!/usr/bin/env bash
# run_native_probe.sh — launch the real client (Rum + UwUcutor) on a given
# cookie and place, with the autoexec timeline probe writing to
# /tmp/native_probe.log. This is the "known-good client" half of the physics
# investigation; it does not touch the boblox client.
#
#   COOKIE=~/.config/rum-cookie tools/run_native_probe.sh [place-id]
set -u
COOKIE=${COOKIE:-$HOME/.config/rum-cookie}
PLACE=${1:-189707}
export ROBLOX_ROBLOSECURITY="$(cat "$COOKIE")"
mkdir -p "$HOME/Documents/UwUcutor/autoexec"
: > /tmp/native_probe.log
cd "$HOME/work/Rum" || exit 1
echo "launching native: place=$PLACE cookie=$COOKIE"
echo "probe log: /tmp/native_probe.log"
exec env LD_PRELOAD="$HOME/work/UwUcutor/build/libUwUcutor.so" \
  "$HOME/work/Rum/build/roblox" \
  --lib "$HOME/work/rr/libroblox.so" \
  --assets "$HOME/work/rr/assets" \
  --place-id "$PLACE"
