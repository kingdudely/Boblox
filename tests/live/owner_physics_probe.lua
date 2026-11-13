-- owner_physics_probe.lua — prove local simulation of a granted assembly.
--
-- Finds the local avatar's NetworkOwnerV3 grant, picks a nearby unanchored
-- part with the same grant that is not part of an avatar, lifts it 30 studs,
-- and reports whether the local Jolt world moves it back down.  A server-owned
-- world (AuthorityMode.Server) skips: there is nothing local to prove.
local ws = game:GetService("Workspace")
local players = game:GetService("Players")

local function wait_for_world()
    local deadline = os.clock() + 25
    while os.clock() < deadline do
        if #ws:GetDescendants() > 100 then return true end
        task.wait(0.25)
    end
    return false
end

local function character()
    local player = players.LocalPlayer
    if not player then return nil end
    return player.Character
end

local function in_character(part, char)
    local p = part
    while p and p ~= ws do
        if p == char then return true end
        p = p.Parent
    end
    return false
end

wait_for_world()

-- The RCC streams the map before it links the Character, so the root grant can
-- arrive several seconds after the world is populated.
local char, root, owner = nil, nil, 0
local deadline = os.clock() + 25
while os.clock() < deadline do
    char = character()
    root = char and (char:FindFirstChild("HumanoidRootPart") or
                     char:FindFirstChildWhichIsA("BasePart"))
    owner = 0
    if root then
        local ok, v = pcall(function() return root.NetworkOwnerV3 end)
        if ok and type(v) == "number" then owner = v end
    end
    if owner > 1 then break end
    task.wait(0.25)
end
if not char then
    print("owner-physics: no local character")
    return
end
if owner <= 1 then
    print(string.format("owner-physics: no grant (owner=%s authority=%s)",
                        tostring(owner), tostring(ws.AuthorityMode)))
    return
end

local candidate = nil
for _, d in ipairs(ws:GetDescendants()) do
    if d:IsA("BasePart") and not d.Anchored and d.CanCollide and
       not in_character(d, char) then
        local ok, v = pcall(function() return d.NetworkOwnerV3 end)
        if ok and v == owner then
            candidate = d
            break
        end
    end
end
if not candidate then
    -- Live places only stream nearby props, and a spawn platform can be far
    -- from the map.  Create a local body carrying the same grant so the live
    -- Jolt path is still exercised; it has no replication ref, so no owner
    -- CFrame upload is emitted for it.
    candidate = Instance.new("Part")
    candidate.Name = "BobloxOwnerProbe"
    candidate.Size = Vector3.new(2, 2, 2)
    candidate.CFrame = root.CFrame * CFrame.new(0, 20, 0)
    candidate.Anchored = false
    candidate.CanCollide = true
    pcall(function() candidate.NetworkOwnerV3 = owner end)
    candidate.Parent = ws
    task.wait(0.25)
    print(string.format("owner-physics: synthetic local body grant=%d", owner))
end

local before = candidate.Position
candidate.CFrame = candidate.CFrame + Vector3.new(0, 30, 0)
task.wait(0.15)
local lifted = candidate.Position
task.wait(1.5)
local after = candidate.Position
local velocity = candidate.AssemblyLinearVelocity
print(string.format(
    "owner-physics: grant=%d part='%s' y=%.2f lifted=%.2f after=%.2f " ..
    "vy=%.2f fell=%.2f moved=%s",
    owner, candidate:GetFullName(), before.Y, lifted.Y, after.Y, velocity.Y,
    before.Y - after.Y, tostring(after.Y < before.Y - 1.0)))
