-- native_root_edges_probe.lua — native missing-assembly and split-anchor edges.
local ws = game:GetService("Workspace")
local players = game:GetService("Players")

local function out(s)
    rconsoleprint("ROOTEDGE " .. s .. "\n")
    print("ROOTEDGE " .. s)
end

local player = players.LocalPlayer
local deadline = os.clock() + 25
while os.clock() < deadline and not (player and player.Character) do
    task.wait(0.5)
    player = players.LocalPlayer
end
out("start")

local function rootName(p)
    local ok, root = pcall(function() return p:GetRootPart() end)
    if not ok then return "ERR" end
    return root and root.Name or "nil"
end

local function assemblyName(p)
    local ok, root = pcall(function() return p.AssemblyRootPart end)
    if not ok then return "ERR" end
    return root and root.Name or "nil"
end

local function report(tag, p)
    local connected = {}
    local okC, c = pcall(function() return p:GetConnectedParts() end)
    if okC then
        for _, q in ipairs(c) do connected[#connected + 1] = q.Name end
        table.sort(connected)
    end
    out(string.format("%s part=%s root=%s assembly=%s connected=[%s] anchored=%s",
                      tag, p.Name, rootName(p), assemblyName(p),
                      table.concat(connected, ","), tostring(p.Anchored)))
end

-- Unparented instance.
local orphan = Instance.new("Part")
orphan.Name = "RootEdge_Orphan"
orphan.Anchored = true
report("ORPHAN", orphan)

-- Parented outside Workspace.
local outside = Instance.new("Folder")
outside.Name = "RootEdge_Outside"
outside.Parent = game
local outsidePart = Instance.new("Part")
outsidePart.Name = "RootEdge_OutsidePart"
outsidePart.Parent = outside
report("OUTSIDE", outsidePart)
outside.Parent = nil

-- Two anchored parts with an unanchored middle welded to both.
local folder = Instance.new("Folder")
folder.Name = "RootEdgeNative"
folder.Parent = ws
local anchorA = Instance.new("Part")
anchorA.Name = "RootEdge_AnchorA"
anchorA.Size = Vector3.new(1, 1, 1)
anchorA.CFrame = CFrame.new(800, 500, 500)
anchorA.Anchored = true
anchorA.CanCollide = false
anchorA.Parent = folder
local anchorB = Instance.new("Part")
anchorB.Name = "RootEdge_AnchorB"
anchorB.Size = Vector3.new(12, 12, 12)
anchorB.CFrame = CFrame.new(804, 500, 500)
anchorB.Anchored = true
anchorB.CanCollide = false
anchorB.Parent = folder
local middle = Instance.new("Part")
middle.Name = "RootEdge_Middle"
middle.Size = Vector3.new(2, 2, 2)
middle.CFrame = CFrame.new(802, 500, 500)
middle.Anchored = false
middle.CanCollide = false
middle.Parent = folder
local weldA = Instance.new("Weld")
weldA.Name = "RootEdge_WeldA"
weldA.Part0 = anchorA
weldA.Part1 = middle
weldA.Parent = folder
local weldB = Instance.new("Weld")
weldB.Name = "RootEdge_WeldB"
weldB.Part0 = middle
weldB.Part1 = anchorB
weldB.Parent = folder
task.wait(1.5)
report("SPLIT-anchorA", anchorA)
report("SPLIT-middle", middle)
report("SPLIT-anchorB", anchorB)
out("weldA.Active=" .. tostring(weldA.Active) .. " weldB.Active=" .. tostring(weldB.Active))
out("done")
