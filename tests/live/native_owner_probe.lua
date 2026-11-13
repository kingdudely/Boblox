-- native_owner_probe.lua — run inside the native client through Xecutor.
-- The client-side GetNetworkOwner API is server-only in this build, so the
-- behavioural test is the same one used against Boblox: lift map parts and see
-- whether the native's local physics simulation moves them back (owned) or the
-- server overwrites the pose (server-owned).
local ws = game:GetService("Workspace")
local players = game:GetService("Players")

local function out(s)
    rconsoleprint("OWNERPROBE " .. s .. "\n")
    print("OWNERPROBE " .. s)
end

out("start")
local player = players.LocalPlayer
local deadline = os.clock() + 25
while os.clock() < deadline and not (player and player.Character) do
    task.wait(0.5)
    player = players.LocalPlayer
end
if not player or not player.Character then
    out("no character")
    return
end
local char = player.Character
local root = char:FindFirstChild("HumanoidRootPart") or char:FindFirstChildWhichIsA("BasePart")
out(string.format("root=%s authority=%s", root and root:GetFullName() or "nil",
                  tostring(ws.AuthorityMode)))

local candidates = {}
local function collect(node, depth)
    if #candidates >= 8 or depth > 12 then return end
    for _, d in ipairs(node:GetChildren()) do
        if #candidates >= 8 then break end
        if d:IsA("BasePart") and not d.Anchored and d.CanCollide then
            local p = d
            local in_character = false
            local in_avatar = false
            while p do
                if p == char then in_character = true end
                if p:IsA("Model") and p:FindFirstChildOfClass("Humanoid") then
                    in_avatar = true
                end
                p = p.Parent
            end
            if not in_character and not in_avatar then
                candidates[#candidates + 1] = d
            end
        end
        collect(d, depth + 1)
    end
end
collect(ws, 0)
out("candidates=" .. tostring(#candidates))
for i, part in ipairs(candidates) do
    local before = part.Position
    part.CFrame = part.CFrame + Vector3.new(0, 30, 0)
    task.wait(0.3)
    local lifted = part.Position
    task.wait(1.5)
    local after = part.Position
    local ownerText = "?"
    local ok, owner = pcall(function() return part:GetNetworkOwner() end)
    if ok then ownerText = owner and owner:GetFullName() or "nil" end
    out(string.format("%d '%s' beforeY=%.2f liftedY=%.2f afterY=%.2f moved=%s owner=%s",
                      i, part:GetFullName(), before.Y, lifted.Y, after.Y,
                      tostring(after.Y < before.Y - 1), ownerText))
end
out("done")
