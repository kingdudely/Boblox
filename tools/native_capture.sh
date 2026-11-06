#!/usr/bin/env bash
# native_capture.sh — run the native client (Rum) with the probe preload.
#
#   ./tools/native_capture.sh [place-id] [capture-path]
#
# Differs from ~/work/run.sh in exactly two ways: the preload is the probe
# (not the executor), and the run bounds itself with
# ROBLOX_AUTO_EXIT_AFTER_PRESENT_MS. Everything else mirrors run.sh so the
# session behaves like the one the executor is tested against.
set -euo pipefail

BOBLOX_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="$(cd "$BOBLOX_ROOT/.." && pwd)"   # ~/work
COOKIE_FILE="${RUM_COOKIE_FILE:-$HOME/.config/rum-cookie}"
PLACE_ID="${1:-10449761463}"
OUT="${2:-/tmp/nat_drive.bin}"
RUM_BIN="${ROBLOX_BIN:-$WORK_DIR/Rum/build/roblox}"
RUM_LIB="${RUM_LIB:-$WORK_DIR/rr/libroblox.so}"
RUM_ASSETS="${RUM_ASSETS:-$WORK_DIR/rr/assets}"
PROBE="$BOBLOX_ROOT/tools/native_probe.so"

[ -x "$RUM_BIN" ] || { echo "[error] missing $RUM_BIN" >&2; exit 1; }
[ -f "$RUM_LIB" ] || { echo "[error] missing $RUM_LIB" >&2; exit 1; }
[ -f "$PROBE" ] || { echo "[error] missing $PROBE (run cc in tools/)" >&2; exit 1; }
[ -f "$COOKIE_FILE" ] || { echo "[error] missing $COOKIE_FILE" >&2; exit 1; }

COOKIE="$(tr -d '\r\n' < "$COOKIE_FILE")"
export ROBLOX_ROBLOSECURITY="$COOKIE"

if [ -f /usr/share/vulkan/icd.d/nvidia_icd.json ]; then
    export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/nvidia_icd.json}"
fi

export ROBLOX_FRAME_RATE_LIMIT="${ROBLOX_FRAME_RATE_LIMIT:-unlimited}"
export ROBLOX_VSYNC="${ROBLOX_VSYNC:-off}"
export ROBLOX_AUTO_EXIT_AFTER_PRESENT_MS="${ROBLOX_AUTO_EXIT_AFTER_PRESENT_MS:-45000}"

export BOBLOX_NET_DUMP="$OUT"
export BOBLOX_DRIVE_AFTER_MS="${BOBLOX_DRIVE_AFTER_MS:-8000}"
export BOBLOX_DRIVE_HOLD_MS="${BOBLOX_DRIVE_HOLD_MS:-4000}"
export BOBLOX_DRIVE_MODE="${BOBLOX_DRIVE_MODE:-key}"
if [ "${BOBLOX_EXECUTOR:-0}" = "1" ]; then
    EXEC_SO="${XECUTOR_SO:-$WORK_DIR/UwUcutor/build/libUwUcutor.so}"
    [ -f "$EXEC_SO" ] || { echo "[error] missing $EXEC_SO" >&2; exit 1; }
    export XECUTOR_DEV="${XECUTOR_DEV:-1}"
    export LD_PRELOAD="$PROBE:$EXEC_SO"
else
    export LD_PRELOAD="$PROBE"
fi

: > "$OUT"
echo "[capture] place=$PLACE_ID out=$OUT auto_exit_ms=$ROBLOX_AUTO_EXIT_AFTER_PRESENT_MS"
exec "$RUM_BIN" --lib "$RUM_LIB" --assets "$RUM_ASSETS" --place-id "$PLACE_ID"
