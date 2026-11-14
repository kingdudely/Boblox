-- teleport_probe.lua — native/Boblox ground truth for TeleportService.
--
-- Run through Xecutor in the native client and with --exec on Boblox while
-- joined to place 1818 (Crossroads).  The source half starts a teleport to
-- 189707 (Natural Disaster Survival) with a nested data table; the destination
-- half prints what GetLocalPlayerTeleportData() sees after the transition.
-- Both clients must produce the same ARRIVED_DATA fields.
local TeleportService = game:GetService("TeleportService")

local function out(s)
    print("TP " .. s)
    if rconsoleprint then
        rconsoleprint("TP " .. s .. "\n")
    end
end

local function encode(value, depth)
    depth = depth or 0
    if depth > 8 then
        return "<max-depth>"
    end
    local t = type(value)
    if t == "nil" or t == "boolean" or t == "number" then
        return tostring(value)
    elseif t == "string" then
        return string.format("%q", value)
    elseif t == "table" then
        local keys = {}
        for k in pairs(value) do
            keys[#keys + 1] = k
        end
        table.sort(keys, function(a, b)
            return tostring(a) < tostring(b)
        end)
        local parts = {}
        for _, k in ipairs(keys) do
            parts[#parts + 1] = tostring(k) .. "=" .. encode(value[k], depth + 1)
        end
        return "{" .. table.concat(parts, ",") .. "}"
    end
    return "<" .. t .. ">"
end

TeleportService.LocalPlayerArrivedFromTeleport:Connect(function(gui, data)
    out("ARRIVED_EVENT gui=" .. tostring(gui) .. " data=" .. encode(data))
end)

local place = game.PlaceId
out(string.format("START place=%s", tostring(place)))

local localData = TeleportService:GetLocalPlayerTeleportData()
out("LOCAL_DATA " .. encode(localData))

if place == 1818 then
    -- Let the session finish hydrating before asking for the transition.
    task.wait(8)
    local returned = TeleportService:Teleport(189707, nil, {
        probe = "teleport-probe",
        n = 42,
        arr = {1, 2, 3},
        nested = {ok = true},
    })
    out("TELEPORT_RETURN " .. tostring(returned))

    -- The native reports the in-flight request synchronously and fires
    -- TeleportInitFailed instead of starting a second transition.
    local duplicate = TeleportService:Teleport(189707)
    out("DUPLICATE_RETURN " .. tostring(duplicate))
elseif place == 189707 then
    out(string.format("ARRIVED place=%s data=%s", tostring(place),
                      encode(TeleportService:GetLocalPlayerTeleportData())))
    out(string.format("HISTORY places=%s universes=%s",
                      encode(TeleportService:TeleportedPlacesBackHistory()),
                      encode(TeleportService:TeleportedUniversesBackHistory())))
end

out("PROBE-DONE")
