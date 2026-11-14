-- native_friend_edge_probe.lua — isolate the native behavior of the two
-- feature-flagged friend-scope methods.  The previous probe reached
-- GetFriendsInServerAsync (which raised "Not Implemented") and then stopped
-- before GetFriendsInUniverseAsync; this run puts each call on its own thread
-- and keeps a watchdog so a yield cannot hide the other results.
local Players = game:GetService("Players")
local player = Players.LocalPlayer

local function out(s)
    print("FEDGE " .. s)
    if rconsoleprint then
        rconsoleprint("FEDGE " .. s .. "\n")
    end
end

if not player then
    out("NO_LOCAL_PLAYER")
    return
end
out(string.format("PLAYER userId=%d place=%s universe=%s game=%s",
                  player.UserId, tostring(game.PlaceId),
                  tostring(game.GameId), tostring(game.JobId)))

task.wait(15)

local whoPlayed = player:GetFriendsWhoPlayedAsync()
out(string.format("WHO_PLAYED type=%s count=%s", type(whoPlayed),
                  type(whoPlayed) == "table" and tostring(#whoPlayed) or "nil"))
if type(whoPlayed) == "table" then
    for i = 1, math.min(#whoPlayed, 10) do
        out(string.format("WHO_PLAYED_ID i=%d value=%s", i,
                          tostring(whoPlayed[i])))
    end
end

task.spawn(function()
    task.wait(8)
    out("WATCHDOG")
end)

task.spawn(function()
    local ok, result = pcall(function()
        return player:GetFriendsInUniverseAsync()
    end)
    out(string.format("UNIVERSE_ASYNC ok=%s type=%s value=%s count=%s",
                      tostring(ok), type(result), tostring(result),
                      type(result) == "table" and tostring(#result) or "nil"))
end)

task.spawn(function()
    local ok, result = pcall(function()
        return player:GetFriendsInServerAsync()
    end)
    out(string.format("SERVER_ASYNC ok=%s type=%s value=%s count=%s",
                      tostring(ok), type(result), tostring(result),
                      type(result) == "table" and tostring(#result) or "nil"))
end)

local detached = Instance.new("Player")
local ok, result = pcall(function()
    return detached:GetFriendsWhoPlayedAsync()
end)
out(string.format("WHO_DETACHED ok=%s type=%s value=%s", tostring(ok),
                  type(result), tostring(result)))

task.wait(60)
out("MAIN-DONE")
