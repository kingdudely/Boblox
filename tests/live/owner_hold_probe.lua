-- owner_hold_probe.lua — hold the local root over a map part while owner-pose
-- uploads run, then report whether the RCC transferred the part. A single
-- teleport is overwritten by local gravity before the server evaluates it.
local ws = game:GetService("Workspace")
local players = game:GetService("Players")

local deadline = os.clock() + 30
while os.clock() < deadline and #ws:GetDescendants() < 100 do task.wait(0.25) end

local player = players.LocalPlayer
local char, root, grant = nil, nil, 0
deadline = os.clock() + 25
while os.clock() < deadline do
    player = players.LocalPlayer
    char = player and player.Character
    root = char and (char:FindFirstChild("HumanoidRootPart") or
                     char:FindFirstChildWhichIsA("BasePart"))
    if root then
        local ok, v = pcall(function() return root.NetworkOwnerV3 end)
        if ok and type(v) == "number" then grant = v end
    end
    if grant > 1 then break end
    task.wait(0.25)
end
if not char or not root or grant <= 1 then
    print(string.format("owner-hold: no character/grant grant=%s", tostring(grant)))
    return
end

local function owner_of(p)
    local ok, v = pcall(function() return p.NetworkOwnerV3 end)
    return ok and v or nil
end

local target
for _, d in ipairs(ws:GetDescendants()) do
    if d:IsA("BasePart") and not d.Anchored and d.CanCollide and d ~= root then
        local v = owner_of(d)
        if v ~= nil and v ~= grant then target = d break end
    end
end
if not target then
    print("owner-hold: no target")
    return
end
local p = target.Position + Vector3.new(0, 8, 0)
print(string.format("owner-hold: hold at (%.1f,%.1f,%.1f) target='%s' owner=%s",
                    p.X, p.Y, p.Z, target:GetFullName(),
                    tostring(owner_of(target))))
local finish = os.clock() + 6
while os.clock() < finish do
    if not root.Parent then break end
    root.CFrame = CFrame.new(p)
    task.wait(0.1)
end
print(string.format("owner-hold: after grant=%s targetOwner=%s",
                    tostring(owner_of(root)), tostring(owner_of(target))))
