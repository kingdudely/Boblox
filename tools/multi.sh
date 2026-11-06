#!/usr/bin/env bash
# multi.sh — one headless client per account, one process each.
#
#   ./tools/multi.sh <cookie-dir> [place-id] [max]
#
# Each file in <cookie-dir> is one account's .ROBLOSECURITY (mode 600). The
# cookie is passed in the environment, never on argv, and each client gets its
# own workspace so filesystem shims cannot collide. Process-per-account is the
# isolation model: separate VM, separate session, separate crash domain.
#
# Per-account egress (proxies) is NOT handled here on purpose: the client has
# no proxy knob, and faking one in a wrapper would look done while doing
# nothing. Add it when the join path works.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COOKIES="${1:?usage: multi.sh <cookie-dir> [place] [max]}"
PLACE="${2:-16518256559}"
MAX="${3:-4}"
BIN="$ROOT/build/client/roblox"
LOGDIR="${MULTI_LOGDIR:-/tmp/multi}"
mkdir -p "$LOGDIR"

[ -x "$BIN" ] || { echo "[error] missing $BIN" >&2; exit 1; }

n=0
for f in "$COOKIES"/*; do
    [ -f "$f" ] || continue
    [ "$n" -ge "$MAX" ] && break
    name="$(basename "$f")"
    export ROBLOX_ROBLOSECURITY="$(tr -d '\r\n' < "$f")"
    export ROBLOX_WORKSPACE="$HOME/.boblox/ws-$name"
    "$BIN" --connection-type=RakNet --place-id "$PLACE" --headless \
        --seconds 0 >"$LOGDIR/$name.log" 2>&1 &
    echo "[multi] $name pid=$! log=$LOGDIR/$name.log"
    n=$((n + 1))
    sleep 2
done
[ "$n" -gt 0 ] || { echo "[multi] no cookie files in $COOKIES" >&2; exit 1; }
echo "[multi] $n client(s) up; pkill -f 'build/client/roblox' to stop"
wait
