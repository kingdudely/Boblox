-- owner_request_probe.lua — request network ownership of a real map part and
-- report whether the RCC honours it. NDS keeps map bodies server-owned in the
-- normal session; this distinguishes "the grant rule never fires" from "the
-- client never asks".
local ws = game:GetService("Workspace")
local players = game:GetService("Players")

local deadline = os.clock() + 25
while os.clock() < deadline and #ws:GetDescendants() < 100 do task.wait(0.25) end

local player = players.LocalPlayer
local char = player and player.Character
deadline = os.clock() + 25
while os.clock() < deadline and not (char and char:FindFirstChild("HumanoidRootPart")) do
    task.wait(0.25)
    player = players.LocalPlayer
    char = player and player.Character
end
if not char then
    print("owner-request: no character")
    return
end
local root = char:FindFirstChild("HumanoidRootPart") or char:FindFirstChildWhichIsA("BasePart")
local grant = 0
if root then
    local ok, v = pcall(function() return root.NetworkOwnerV3 end)
    if ok and type(v) == "number" then grant = v end
end
print(string.format("owner-request: local grant=%s", tostring(grant)))

local function owner_of(part)
    local ok, v = pcall(function() return part.NetworkOwnerV3 end)
    return ok and v or nil
end

local target
for _, d in ipairs(ws:GetDescendants()) do
    if d:IsA("BasePart") and not d.Anchored and d.CanCollide and
       d ~= root and d:IsDescendantOf(ws) then
        local parent = d.Parent
        local inside_char = false
        while parent do
            if parent == char then
                inside_char = true
                break
            end
            parent = parent.Parent
        end
        if not inside_char then
            local v = owner_of(d)
            if v ~= nil and v ~= grant then target = d break end
        end
    end
end
if not target then
    print("owner-request: no map target")
    return
end
print(string.format("owner-request: target='%s' owner=%s",
                    target:GetFullName(), tostring(owner_of(target))))

local ok, err = pcall(function() target:SetNetworkOwner(player) end)
print(string.format("owner-request: SetNetworkOwner=%s err=%s",
                    tostring(ok), tostring(err)))
task.wait(3)
print(string.format("owner-request: after target owner=%s",
                    tostring(owner_of(target))))
