-- native_player_web_probe.lua — native/Boblox ground truth for Player web APIs.
--
-- Run through Xecutor in the native client and with --exec on Boblox.  The
-- group ids below cover a member (rank 1), an owner (rank 255), a non-member,
-- and a group whose role name is not its rank name.  Friend listings are
-- live data; the probe prints the schema and current values, not a fixture.
--
-- The native *Async methods yield on a web cache miss.  Once the client has
-- finished its sign-in/cache warmup they complete synchronously, so the probe
-- waits below and then calls them directly.  Only the deliberately detached-
-- player error probes use pcall, where the error is raised before any web
-- call and no yield is possible.
local Players = game:GetService("Players")
local player = Players.LocalPlayer

local function out(s)
    print("PWEB " .. s)
    if rconsoleprint then
        rconsoleprint("PWEB " .. s .. "\n")
    end
end

if not player then
    out("NO_LOCAL_PLAYER")
    return
end
out(string.format("PLAYER userId=%d name=%s display=%s", player.UserId,
                  player.Name, player.DisplayName))

-- The native web client finishes its sign-in/cache warmup a few seconds
-- after the executor gets the game VM.  Calling a cold web method here makes
-- its native yield cross the executor's C-call boundary, so wait for the
-- client to be ready before measuring.
task.wait(10)

local groups = {
    {825735094, "member-rank1"},
    {646080861, "owner-rank255"},
    {9685810, "member-rank2"},
    {16536679, "rank1-member"},
    {5101636, "rank1-customer"},
    {2629950, "rank1-named-role"},
    {3486412, "rank9"},
    {1, "non-member"},
}

for _, entry in ipairs(groups) do
    local gid, label = entry[1], entry[2]
    local member = player:IsInGroup(gid)
    local rank = player:GetRankInGroup(gid)
    local role = player:GetRoleInGroup(gid)
    out(string.format("GROUP gid=%d label=%s member=%s rank=%s role=%s",
                      gid, label, tostring(member), tostring(rank),
                      tostring(role)))
end

-- Async variants are separate API rows and must agree with the blocking ones.
for _, entry in ipairs(groups) do
    local gid, label = entry[1], entry[2]
    local member = player:IsInGroupAsync(gid)
    local rank = player:GetRankInGroupAsync(gid)
    local role = player:GetRoleInGroupAsync(gid)
    out(string.format("ASYNC gid=%d label=%s member=%s rank=%s role=%s",
                      gid, label, tostring(member), tostring(rank),
                      tostring(role)))
end

local friends = player:GetFriendsOnline(200)
out(string.format("FRIENDS type=%s count=%s", type(friends),
                  friends and #friends or "nil"))
if type(friends) == "table" then
    if friends[1] then
        local keys = {}
        for k in pairs(friends[1]) do keys[#keys + 1] = k end
        table.sort(keys)
        for _, k in ipairs(keys) do
            out(string.format("FRIEND_FIELD key=%s type=%s value=%s", k,
                              type(friends[1][k]), tostring(friends[1][k])))
        end
    end
    local shown = math.min(#friends, 5)
    for i = 1, shown do
        local f = friends[i]
        out(string.format(
            "FRIEND i=%d VisitorId=%s UserName=%s DisplayName=%s IsOnline=%s PlaceId=%s GameId=%s UniverseId=%s LastOnline=%s LastLocation=%s LastLocationType=%s",
            i, tostring(f.VisitorId), tostring(f.UserName),
            tostring(f.DisplayName), tostring(f.IsOnline),
            tostring(f.PlaceId), tostring(f.GameId),
            tostring(f.UniverseId), tostring(f.LastOnline),
            tostring(f.LastLocation), tostring(f.LastLocationType)))
        for _, k in ipairs({"LocationType", "PlaceId", "GameId",
                            "UniverseId"}) do
            out(string.format("FRIEND_EXTRA i=%d key=%s value=%s", i, k,
                              tostring(f[k])))
        end
    end
end

local limited = player:GetFriendsOnline(1)
out(string.format("FRIENDS_LIMIT1 type=%s count=%s", type(limited),
                  limited and #limited or "nil"))

local asyncFriends = player:GetFriendsOnlineAsync(200)
out(string.format("FRIENDS_ASYNC type=%s count=%s", type(asyncFriends),
                  asyncFriends and #asyncFriends or "nil"))

-- The methods are LocalPlayer-only and require a DataModel.  A detached
-- Player must fail instead of silently returning a made-up empty result.
local other = Instance.new("Player")
local ok, err = pcall(function() return other:GetFriendsOnline() end)
out(string.format("NONLOCAL ok=%s err=%s", tostring(ok), tostring(err)))
local groupOk, groupErr = pcall(function() return other:IsInGroup(1) end)
out(string.format("NONLOCAL_GROUP ok=%s err=%s", tostring(groupOk),
                  tostring(groupErr)))

out("PROBE-DONE")
