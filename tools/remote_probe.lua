-- remote_probe.lua — generate a labeled remote call and a labeled move.
--
-- Sent to the *native* client through the executor socket, so the capture has
-- known semantics: whatever bytes carry the marker string are the RemoteEvent
-- marshalling, and whatever changes while Move() runs is the movement input.
--
--   BOBLOX_EXECUTOR=1 ./tools/native_capture.sh 10449761463 /tmp/nat_re.bin
--   python3 ~/work/Xecutor/tests/send.py < tools/remote_probe.lua

local MARK = "BOBLOXPROBE0123456789"

local function wait_for_game()
    for _ = 1, 60 do
        local ok, plr = pcall(function()
            return game:GetService("Players").LocalPlayer
        end)
        if ok and plr and plr.Character then
            return plr
        end
        task.wait(1)
    end
    return nil
end

local plr = wait_for_game()
print("[probe] game ready:", plr ~= nil)

-- 1. RemoteEvent fire with a unique string payload.
local fired = 0
local function walk(obj, depth)
    if depth > 5 or fired >= 3 then return end
    local ok, kids = pcall(function() return obj:GetChildren() end)
    if not ok then return end
    for _, c in ipairs(kids) do
        if fired >= 3 then return end
        local okIs, isRemote = pcall(function() return c:IsA("RemoteEvent") end)
        if okIs and isRemote then
            local fok, ferr = pcall(function() c:FireServer(MARK) end)
            print("[probe] fired", c:GetFullName(), fok, tostring(ferr))
            fired = fired + 1
        else
            walk(c, depth + 1)
        end
    end
end
walk(game:GetService("ReplicatedStorage"), 0)
walk(game:GetService("ReplicatedFirst"), 0)
walk(game:GetService("Workspace"), 0)
walk(game:GetService("Players"), 0)
print("[probe] remotes fired:", fired)

-- 2. Move forward with a known vector for two seconds, then stop.
local hum = plr and plr.Character and plr.Character:FindFirstChildOfClass("Humanoid")
if hum then
    print("[probe] moving forward")
    for _ = 1, 20 do
        hum:Move(Vector3.new(0, 0, -1), false)
        task.wait(0.1)
    end
    hum:Move(Vector3.zero, false)
    print("[probe] move stopped")
else
    print("[probe] no humanoid")
end

print("[probe] done")
