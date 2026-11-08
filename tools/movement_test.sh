#!/usr/bin/env bash
# movement_test.sh — one-shot proof/disproof that our transform uploads move the
# body as seen by another client.
#
#   tools/movement_test.sh [place-id] [seconds]
#
# What it does:
#   1. picks a populated server for the place (public servers API),
#   2. starts the observer (third cookie) pinned to it,
#   3. starts the mover (alt cookie) pinned to the same job, walking +X,
#   4. pipes the observer's reported CFrame for our body into /tmp/body.txt,
#      which the client adopts (RBX_BODY_FILE) so the claim starts where the
#      RCC actually put the body (they differ by 30-60 studs otherwise),
#   5. prints the distinct CFrame values seen for our body: more than one means
#      movement replicated.
#
# Cookies: ~/.config/alt-cookie (mover), ~/.config/third-cookie (observer).
# The 189707 body spawn is a lottery; run it a few times if `observed=0`.
set -u
PLACE="${1:-189707}"
SECS="${2:-150}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JOB=$(curl -s -H "Cookie: .ROBLOSECURITY=$(cat "$HOME/.config/third-cookie")" \
  "https://games.roblox.com/v1/games/$PLACE/servers/Public?sortOrder=Desc&limit=10" |
  python3 -c '
import json,sys
try: d=json.load(sys.stdin)["data"]
except Exception: print(""); raise SystemExit
c=[s for s in d if s["playing"] <= s["maxPlayers"]-3]
print(c[0]["id"] if c else "")
')
[ -z "$JOB" ] && { echo "no populated server found" >&2; exit 1; }
echo "job=$JOB place=$PLACE secs=$SECS"
pkill -9 -f '[b]uild/client/roblox' 2>/dev/null; sleep 2
rm -f /tmp/body.txt
cat > /tmp/observe2.lua <<'LUA'
local pl = game:GetService("Players")
for i = 1, 200 do
  for _, p in ipairs(pl:GetPlayers()) do
    local ch = p.Character
    local hrp = ch and ch:FindFirstChild("HumanoidRootPart")
    if hrp then
      print(string.format("[ob2] i=%d %s pos=%s cf=%s", i, p.Name,
                          tostring(hrp.Position), tostring(hrp.CFrame)))
    end
  end
  task.wait(2)
end
LUA
cat > /tmp/walksafe.lua <<'LUA'
for i = 1, 400 do boblox.move(1, 0, 0); task.wait(0.25) end
LUA
( BOBLOX_COOKIE_FILE=$HOME/.config/third-cookie timeout $((SECS+20)) \
    "$ROOT/tools/run.sh" "$PLACE" --job-id "$JOB" --seconds "$SECS" \
    --exec /tmp/observe2.lua > /tmp/observe.log 2>&1 & )
sleep 4
( BOBLOX_COOKIE_FILE=$HOME/.config/alt-cookie \
    RBX_SELFCHAR=1 RBX_NO_SELFSPAWN=1 \
    RBX_BODY_FILE=/tmp/body.txt timeout $((SECS+20)) \
    "$ROOT/tools/run.sh" "$PLACE" --job-id "$JOB" --seconds "$SECS" \
    --exec /tmp/walksafe.lua > /tmp/mover.log 2>&1 & )
# Only our own body may be adopted: another player's position would make the
# claim address someone else's body.
NAME=$(curl -s -H "Cookie: .ROBLOSECURITY=$(cat "$HOME/.config/alt-cookie")" \
  https://users.roblox.com/v1/users/authenticated | python3 -c \
  'import json,sys; print(json.load(sys.stdin)["name"])')
for _ in $(seq 1 "$SECS"); do
  X=$(grep "ob2.*${NAME}" /tmp/observe.log 2>/dev/null | tail -1 |
      sed 's/.*cf=//' | cut -d',' -f1-3 | tr -d ' ' | tr ',' ' ')
  [ -n "$X" ] && echo "$X" > /tmp/body.txt
  sleep 1
done
echo "observed=$(grep -c 'ob2.*' /tmp/observe.log) adopted=$(grep -c bodyfile /tmp/mover.log)"
echo "mover=$NAME"
echo "distinct body CFrames for $NAME:"
grep "ob2.*${NAME}" /tmp/observe.log | sed 's/.*cf=//' | cut -d',' -f1-3 |
  sort -u | head -10
