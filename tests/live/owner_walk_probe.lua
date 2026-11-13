-- owner_walk_probe.lua — walk (not teleport) toward a map part and report
-- whether the RCC grants its network ownership. Teleporting the root is not
-- replicated by the RCC, so it cannot move the server-side simulation focus.
local ws = game:GetService("Workspace")
local players = game:GetService("Players")

local deadline = os.clock() + 30
while os.clock() < deadline and #ws:GetDescendants() < 100 do task.wait(0.25) end

local player = players.LocalPlayer
local char, root, owner = nil, nil, 0
deadline = os.clock() + 25
while os.clock() < deadline do
    player = players.LocalPlayer
    char = player and player.Character
    root = char and (char:FindFirstChild("HumanoidRootPart") or
                     char:FindFirstChildWhichIsA("BasePart"))
    if root then
        local ok, v = pcall(function() return root.NetworkOwnerV3 end)
        if ok and type(v) == "number" then owner = v end
    end
    if owner > 1 then break end
    task.wait(0.25)
end
if not char or not root or owner <= 1 then
    print(string.format("owner-walk: no character/grant owner=%s", tostring(owner)))
    return
end

local function owner_of(p)
    local ok, v = pcall(function() return p.NetworkOwnerV3 end)
    return ok and v or nil
end

local target
for _, d in ipairs(ws:GetDescendants()) do
    if d:IsA("BasePart") and not d.Anchored and d.CanCollide then
        local v = owner_of(d)
        if v ~= nil and v ~= owner and d ~= root then
            target = d
            break
        end
    end
end
if not target then
    print("owner-walk: no map target")
    return
end
local humanoid = char:FindFirstChildWhichIsA("Humanoid")
print(string.format("owner-walk: start=(%.1f,%.1f,%.1f) target='%s' owner=%s",
                    root.Position.X, root.Position.Y, root.Position.Z,
                    target:GetFullName(), tostring(owner_of(target))))
if humanoid then
    for _ = 1, 20 do
        if not char.Parent then break end
        humanoid:MoveTo(target.Position)
        task.wait(0.35)
    end
end
print(string.format("owner-walk: end=(%.1f,%.1f,%.1f) grant=%s targetOwner=%s",
                    root.Position.X, root.Position.Y, root.Position.Z,
                    tostring(owner_of(root)), tostring(owner_of(target))))
