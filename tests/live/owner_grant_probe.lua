-- owner_grant_probe.lua — move the local character onto the replicated map and
-- ask whether the RCC's spatial ownership rule grants nearby unanchored parts.
--
-- The earlier probes only looked at the spawn platform, where every map body is
-- still server-owned. This one reports the owner histogram, teleports the
-- client-owned HumanoidRootPart next to a real map assembly, and reports the
-- histogram again after the server has had time to evaluate the grant.
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

local function histogram()
    local owners, unanchored = {}, 0
    for _, d in ipairs(ws:GetDescendants()) do
        if d:IsA("BasePart") and not d.Anchored then
            unanchored = unanchored + 1
            local ok, v = pcall(function() return d.NetworkOwnerV3 end)
            local key = ok and tostring(v) or "<hidden>"
            owners[key] = (owners[key] or 0) + 1
        end
    end
    local text = ""
    for k, v in pairs(owners) do
        text = text .. " " .. tostring(k) .. ":" .. v
    end
    return unanchored, text
end

local function local_owner(char)
    local root = char and (char:FindFirstChild("HumanoidRootPart") or
                           char:FindFirstChildWhichIsA("BasePart"))
    if not root then return 0, nil end
    local ok, v = pcall(function() return root.NetworkOwnerV3 end)
    return (ok and type(v) == "number") and v or 0, root
end

if not wait_for_world() then
    print("owner-grant: no world")
    return
end

-- The RCC streams the map before it links the client's Character, so the
-- root grant can appear several seconds after the world is populated.
local player = players.LocalPlayer
local char, grant, root = nil, 0, nil
local deadline = os.clock() + 25
while os.clock() < deadline do
    char = player and player.Character
    grant, root = local_owner(char)
    if grant > 1 then break end
    task.wait(0.25)
end
local n0, h0 = histogram()
print(string.format("owner-grant spawn grant=%s unanchored=%d owners=%s",
                    tostring(grant), n0, h0))

if not root or grant <= 1 then
    print("owner-grant: no local character/grant")
    return
end

-- Pick a real unanchored map part with a different owner and put the character
-- on top of it. The RCC's grant is spatial, so this is the only client-side
-- action that can make the ownership rule fire.
local target = nil
for _, d in ipairs(ws:GetDescendants()) do
    if d:IsA("BasePart") and not d.Anchored and d.CanCollide and d ~= root then
        local ok, v = pcall(function() return d.NetworkOwnerV3 end)
        if ok and type(v) == "number" and v ~= grant then
            target = d
            break
        end
    end
end

if not target then
    print("owner-grant: no server-owned unanchored target")
    return
end

local p = target.Position
print(string.format("owner-grant target='%s' pos=(%.2f, %.2f, %.2f)",
                    target:GetFullName(), p.X, p.Y, p.Z))
root.CFrame = CFrame.new(p + Vector3.new(0, 8, 0))
task.wait(5)
local n1, h1 = histogram()
local grant2 = local_owner(char)
print(string.format("owner-grant after grant=%s unanchored=%d owners=%s",
                    tostring(grant2), n1, h1))
