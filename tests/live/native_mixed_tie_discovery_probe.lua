-- native_mixed_tie_discovery_probe.lua — find replicated parts suitable for
-- a mixed network/local complete tie and print native debug ids.
local ws = game:GetService("Workspace")
local players = game:GetService("Players")

local function out(s)
    rconsoleprint("MIXED " .. s .. "\n")
    print("MIXED " .. s)
end

local function debugId(p)
    local ok, id = pcall(function() return p:GetDebugId() end)
    return ok and tostring(id) or "ERR"
end

local function connectedCount(p)
    local ok, list = pcall(function() return p:GetConnectedParts() end)
    return ok and #list or -1
end

local function describe(tag, p)
    out(string.format("%s name=%s class=%s debug=%s size=%s anchored=%s massless=%s priority=%d connected=%d parent=%s",
                      tag, p.Name, p.ClassName, debugId(p), tostring(p.Size),
                      tostring(p.Anchored), tostring(p.Massless), p.RootPriority,
                      connectedCount(p), p.Parent and p.Parent:GetFullName() or "nil"))
end

local player = players.LocalPlayer
local deadline = os.clock() + 25
while os.clock() < deadline and not (player and player.Character) do
    task.wait(0.5)
    player = players.LocalPlayer
end

local seen = 0
local candidates = {}
for _, p in ipairs(ws:GetDescendants()) do
    if p:IsA("BasePart") then
        seen = seen + 1
        if seen <= 8 then
            describe("sample", p)
        end
        if p.Anchored == false and p.Massless == false and
           p.RootPriority == 0 and not p:IsA("Seat") and
           not p:IsA("VehicleSeat") and connectedCount(p) == 1 and
           p.Name ~= "HumanoidRootPart" and p.Name ~= "Torso" then
            candidates[#candidates + 1] = p
        end
    end
end
out("scanned=" .. tostring(seen) .. " candidates=" .. tostring(#candidates))
for i = 1, math.min(#candidates, 12) do
    describe("candidate", candidates[i])
end

local char = player and player.Character
if char then
    for _, p in ipairs(char:GetDescendants()) do
        if p:IsA("BasePart") then
            describe("character", p)
        end
    end
end

local folder = Instance.new("Folder")
folder.Name = "MixedLocalDiscovery"
folder.Parent = ws
local p1 = Instance.new("Part")
p1.Name = "MixedLocalA"
p1.Size = Vector3.new(2, 2, 2)
p1.Anchored = false
p1.CanCollide = false
p1.Parent = folder
local p2 = Instance.new("Part")
p2.Name = "MixedLocalB"
p2.Size = Vector3.new(2, 2, 2)
p2.Anchored = false
p2.CanCollide = false
p2.Parent = folder
describe("localA", p1)
describe("localB", p2)

-- Print internal-looking debug ids in creation order.
out("local debug A=" .. debugId(p1) .. " B=" .. debugId(p2))
out("done")
