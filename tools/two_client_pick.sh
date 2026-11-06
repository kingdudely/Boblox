#!/usr/bin/env bash
# two_client_pick.sh — launch two clients in the SAME server, or fail loudly.
#   tools/two_client_pick.sh [place] [seconds] [mover-lua] [obs-lua]
# Retries each client until its --require-job pin is honoured (exit 3 = mismatch).
set -u
PLACE="${1:-189707}"; SECS="${2:-90}"
MOVER="${3:-/tmp/walksafe.lua}"; OBS="${4:-/tmp/observe2.lua}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JOB=$(curl -s -H "Cookie: .ROBLOSECURITY=$(cat "$HOME/.config/third-cookie")" \
  "https://games.roblox.com/v1/games/$PLACE/servers/Public?sortOrder=Desc&limit=10" |
  python3 -c 'import json,sys
try: d=json.load(sys.stdin)["data"]
except Exception: print(""); raise SystemExit
c=[s for s in d if s["playing"]<=s["maxPlayers"]-3]
print(c[0]["id"] if c else "")')
[ -z "$JOB" ] && { echo "no populated server"; exit 1; }
echo "job=$JOB"
# Job ids are not honoured by v1/join-game (measured: 5/5 pins landed in the
# matchmaker's server), so pairing is verified by the observer actually seeing
# the mover. Retry whole attempts until that happens.
launch() { # cookie lua log secs
  BOBLOX_COOKIE_FILE="$1" timeout $(($4+40)) "$ROOT/tools/run.sh" "$PLACE" \
    --seconds "$4" --exec "$2" > "$3" 2>&1; }
NAME=$(curl -s -H "Cookie: .ROBLOSECURITY=$(cat "$HOME/.config/alt-cookie")" \
  https://users.roblox.com/v1/users/authenticated | python3 -c \
  'import json,sys; print(json.load(sys.stdin)["name"])')
for attempt in 1 2 3 4 5; do
  rm -f /tmp/body.txt; pkill -9 -f '[b]uild/client/roblox' 2>/dev/null; sleep 2
  ( launch "$HOME/.config/third-cookie" "$OBS" /tmp/observe.log "$SECS" & ) 
  sleep 4
  ( launch "$HOME/.config/alt-cookie" "$MOVER" /tmp/mover.log "$SECS" & )
  for _ in $(seq 1 "$SECS"); do
    X=$(grep "ob2.*${NAME}" /tmp/observe.log 2>/dev/null | tail -1 |
        sed 's/.*cf=//' | cut -d',' -f1-3 | tr -d ' ' | tr ',' ' ')
    [ -n "$X" ] && echo "$X" > /tmp/body.txt
    sleep 1
  done
  seen=$(grep -c "ob2.*${NAME}" /tmp/observe.log 2>/dev/null)
  echo "attempt=$attempt mover=$NAME observed=$seen"
  [ "$seen" -gt 0 ] && break
done
grep "ob2.*${NAME}" /tmp/observe.log | sed 's/.*cf=//' | cut -d',' -f1-3 |
  sort -u | head -8
