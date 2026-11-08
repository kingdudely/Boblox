#!/usr/bin/env bash
# script_compat.sh — run one place and report script failures plus stub usage.
#
#   tools/script_compat.sh <place-id> [cookie-file] [seconds] [lua-file]
#
# Defaults: fourth cookie, 60 s, built-in idle loop. Outputs:
#   $OUT/run.log        client log
#   $OUT/fail/          chunks for failed scripts (RBX_DUMP_SCRIPT_FAIL)
#   $OUT/summary.txt    first failures + sorted stub census
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLACE="${1:?usage: script_compat.sh <place-id> [cookie-file] [seconds] [lua-file]}"
COOKIE="${2:-$HOME/.config/fourth-cookie}"
SECS="${3:-60}"
LUA="${4:-}"
OUT="${OUT:-$HOME/script_compat}"
rm -rf "$OUT"; mkdir -p "$OUT/fail"
if [ -z "$LUA" ]; then
    LUA="$OUT/idle.lua"
    cat >"$LUA" <<'LUA'
print("[compat] start")
for i = 1, 500 do task.wait(1) end
LUA
fi
pkill -9 -f 'build/client/roblo[x]' 2>/dev/null || true
BOBLOX_COOKIE_FILE="$COOKIE" RBX_STUB_STATS=1 RBX_LOG_SCRIPTS=1 \
RBX_TRACEBACK=1 RBX_DUMP_SCRIPT_FAIL="$OUT/fail" \
  timeout $((SECS + 30)) "$ROOT/tools/run.sh" "$PLACE" --seconds "$SECS" \
    --exec "$LUA" >"$OUT/run.log" 2>&1 || true
{
    echo "# script failures"
    grep -E 'script id=.*failed|scripts=' "$OUT/run.log" | tail -80 || true
    echo "# stub census"
    grep -E '^\[stub\]' "$OUT/run.log" | tail -120 || true
} >"$OUT/summary.txt"
cat "$OUT/summary.txt"
