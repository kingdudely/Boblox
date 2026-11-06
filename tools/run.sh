#!/usr/bin/env bash
# run.sh — launch the headless client with Boblox's own cookie.
#   ./tools/run.sh 1818
#   ./tools/run.sh 1818 --exec run/live_test.lua --seconds 100
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COOKIE="${BOBLOX_COOKIE_FILE:-$HOME/.config/boblox-cookie}"
PLACE="${1:?usage: run.sh <place-id> [extra flags]}"
shift
[ -f "$COOKIE" ] || { echo "[error] no cookie at $COOKIE — run tools/cookie.sh" >&2; exit 1; }
exec "$ROOT/build/client/roblox" --connection-type=RakNet \
    --place-id "$PLACE" --headless --cookie-file "$COOKIE" "$@"
