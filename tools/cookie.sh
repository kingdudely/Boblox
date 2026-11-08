#!/usr/bin/env bash
# cookie.sh — set Boblox's own .ROBLOSECURITY, separate from Rum's.
#   ./tools/cookie.sh            # paste the value, Enter
#   ./tools/cookie.sh --show     # print which file is in use (not the value)
set -euo pipefail
F="${BOBLOX_COOKIE_FILE:-$HOME/.config/boblox-cookie}"
if [ "${1:-}" = "--show" ]; then
    printf 'cookie file: %s\n' "$F"
    [ -f "$F" ] && printf 'mode: %s  bytes: %s\n' "$(stat -c%a "$F")" "$(stat -c%s "$F")" || echo "not set"
    exit 0
fi
umask 077
printf 'paste .ROBLOSECURITY for Boblox (input hidden): '
IFS= read -rs V
echo
[ -n "$V" ] || { echo "empty; nothing written" >&2; exit 1; }
printf '%s' "$V" > "$F"
chmod 600 "$F"
echo "wrote $F ($(stat -c%s "$F") bytes, mode $(stat -c%a "$F"))"
