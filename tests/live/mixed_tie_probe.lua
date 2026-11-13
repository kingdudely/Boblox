-- mixed_tie_probe.lua — Boblox counterpart to native_mixed_tie_probe.lua.
-- Compares replicated-only, mixed, and local-only complete ties.
local ws = game:GetService("Workspace")

local function out(s)
    print("MIXED " .. s)
end

local function rootName(p)
    local ok, root = pcall(function() return p:GetRootPart() end)
    if not ok then return "ERR" end
    return root and root.Name or "nil"
end

local function debugId(p)
    local ok, id = pcall(function() return p:GetDebugId() end)
    return ok and tostring(id) or "ERR"
end

local function isRoot(p, candidate)
    local ok, root = pcall(function() return p:GetRootPart() end)
    return ok and root == candidate
end

local function eligible(p)
    -- Use any replicated BasePart for the tie.  The native probe selects the
    -- same Part class, but a place can stream MeshParts instead; the tie rule
    -- is about size and rank, not the concrete part class.
    return p:IsA("BasePart") and
           p.Anchored == false and p.Massless == false and
           p.RootPriority == 0 and not p:IsA("Seat") and
           not p:IsA("VehicleSeat") and
           p.Name ~= "HumanoidRootPart" and p.Name ~= "Torso" and
           #p:GetConnectedParts() == 1
end

local function pickPair()
    local candidates = {}
    for _, p in ipairs(ws:GetDescendants()) do
        if eligible(p) then
            candidates[#candidates + 1] = p
        end
    end
    for i = 1, #candidates do
        for j = i + 1, #candidates do
            if tostring(candidates[i].Size) == tostring(candidates[j].Size) then
                return candidates[i], candidates[j]
            end
        end
    end
    return nil, nil
end

-- Streaming can deliver the replicated map after the local script starts.
-- Wait until two rank-neutral, unanchored, equal-sized replicated parts exist.
local deadline = os.clock() + 40
local r1, r2
repeat
    r1, r2 = pickPair()
    if not r1 then task.wait(0.5) end
until r1 or os.clock() >= deadline
if not r1 then
    error("MIXED no replicated equal-size pair")
end

local size = r1.Size
local replicatedFolder = Instance.new("Folder")
replicatedFolder.Name = "MixedTieReplicatedOnly"
replicatedFolder.Parent = ws
local replicatedWeld = Instance.new("Weld")
replicatedWeld.Name = "MixedTieReplicatedWeld"
replicatedWeld.Part0 = r1
replicatedWeld.Part1 = r2
replicatedWeld.Parent = replicatedFolder

task.wait(0.25)
out(string.format("replicatedOnly root=%s isR1=%s isR2=%s r1=%s/%s r2=%s/%s",
                  rootName(r1), tostring(isRoot(r1, r1)),
                  tostring(isRoot(r1, r2)), r1.Name, debugId(r1),
                  r2.Name, debugId(r2)))

replicatedWeld.Enabled = false
-- Let the replicated instances settle before creating the local tie members.
task.wait(2.0)

local folder = Instance.new("Folder")
folder.Name = "MixedTieLocal"
folder.Parent = ws

local function localPart(name)
    local p = Instance.new("Part")
    p.Name = name
    p.Size = size
    p.Anchored = false
    p.CanCollide = false
    p.Massless = false
    p.RootPriority = 0
    p.Parent = folder
    return p
end

local l1 = localPart("MixedTieLocalA")
local l2 = localPart("MixedTieLocalB")
local l3 = localPart("MixedTieLocalC")
local l4 = localPart("MixedTieLocalD")

local function weld(name, a, b, parent)
    local j = Instance.new("Weld")
    j.Name = name
    j.Part0 = a
    j.Part1 = b
    j.Parent = parent or folder
    return j
end
weld("MixedTieWeldR1L1", r1, l1)
weld("MixedTieWeldL1L2", l1, l2)
weld("MixedTieWeldL2R2", l2, r2)

local localFolder = Instance.new("Folder")
localFolder.Name = "MixedTieLocalOnly"
localFolder.Parent = ws
weld("MixedTieLocalWeld", l3, l4, localFolder)

task.wait(0.25)
out(string.format("mixed root=%s isR1=%s isR2=%s isL1=%s isL2=%s r1=%s/%s r2=%s/%s l1=%s/%s l2=%s/%s",
                  rootName(l1), tostring(isRoot(l1, r1)),
                  tostring(isRoot(l1, r2)), tostring(isRoot(l1, l1)),
                  tostring(isRoot(l1, l2)), r1.Name, debugId(r1),
                  r2.Name, debugId(r2), l1.Name, debugId(l1),
                  l2.Name, debugId(l2)))
out(string.format("localOnly root=%s isL3=%s isL4=%s l3=%s/%s l4=%s/%s",
                  rootName(l3), tostring(isRoot(l3, l3)),
                  tostring(isRoot(l3, l4)), l3.Name, debugId(l3),
                  l4.Name, debugId(l4)))
out("done")
