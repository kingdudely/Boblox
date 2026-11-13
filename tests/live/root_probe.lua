-- root_probe.lua — live proof for BasePart assembly-root selection.
--
-- Run through --exec on a live session.  Prints the observed GetRootPart and
-- AssemblyRootPart results for the characteristic native cases; it does not
-- assert, so the log shows exactly what the running runtime returned.

local ws = game:GetService("Workspace")

local deadline = os.clock() + 20
while #ws:GetDescendants() < 100 and os.clock() < deadline do
    task.wait(0.25)
end

local folder = Instance.new("Folder")
folder.Name = "RootProbeBoblox"
folder.Parent = ws

local function makePart(name, size, anchored, massless, parent)
    local p = Instance.new("Part")
    p.Name = name
    p.Size = size
    p.Anchored = anchored or false
    p.Massless = massless or false
    p.CanCollide = false
    p.Parent = parent or folder
    return p
end

local function weld(a, b)
    local j = Instance.new("Weld")
    j.Part0 = a
    j.Part1 = b
    j.Parent = a.Parent
    return j
end

local function rootName(p)
    local root = p:GetRootPart()
    return root and root.Name or "nil"
end

local function report(tag, p)
    local assembly = p.AssemblyRootPart
    print(string.format("root-probe %s root=%s assembly=%s",
                        tag, rootName(p),
                        assembly and assembly.Name or "nil"))
end

local isolated = makePart("RootIsolated", Vector3.new(2, 2, 2))
local anchor = makePart("RootChainAnchor", Vector3.new(2, 2, 2), true)
local mid = makePart("RootChainMid", Vector3.new(2, 2, 2))
local tail = makePart("RootChainTail", Vector3.new(2, 2, 2))
weld(anchor, mid)
weld(mid, tail)

local large = makePart("RootLargest", Vector3.new(12, 12, 12))
local small = makePart("RootSmall", Vector3.new(2, 2, 2))
weld(large, small)

local priority = makePart("RootPriority", Vector3.new(1, 1, 1))
priority.RootPriority = 100
local priorityPeer = makePart("RootPriorityPeer", Vector3.new(20, 20, 20))
weld(priority, priorityPeer)

local massless = makePart("RootMassless", Vector3.new(20, 20, 20),
                          false, true)
local solid = makePart("RootSolid", Vector3.new(2, 2, 2))
weld(massless, solid)

local humanoidModel = Instance.new("Model")
humanoidModel.Name = "RootHumanoidModel"
humanoidModel.Parent = ws
local hrp = makePart("HumanoidRootPart", Vector3.new(2, 2, 1),
                     false, false, humanoidModel)
local torso = makePart("Torso", Vector3.new(2, 2, 1),
                       false, false, humanoidModel)
weld(hrp, torso)
local humanoid = Instance.new("Humanoid")
humanoid.Parent = humanoidModel

local mutualOne = makePart("RootMutualOne", Vector3.new(2, 2, 2))
local mutualTwo = makePart("RootMutualTwo", Vector3.new(2, 2, 2))
weld(mutualOne, mutualTwo)
weld(mutualTwo, mutualOne)

local primaryModel = Instance.new("Model")
primaryModel.Name = "RootPrimaryModel"
primaryModel.Parent = ws
local primary = makePart("RootPrimary", Vector3.new(2, 2, 2),
                         false, false, primaryModel)
local primaryLarge = makePart("RootPrimaryLarge", Vector3.new(12, 12, 12),
                              false, false, primaryModel)
weld(primary, primaryLarge)
primaryModel.PrimaryPart = primary

local anchoredOne = makePart("RootAnchoredOne", Vector3.new(1, 1, 1), true)
local anchoredTwo = makePart("RootAnchoredTwo", Vector3.new(12, 12, 12), true)
weld(anchoredOne, anchoredTwo)

-- Equal anchors: the newer direct weld is the native active link.
local tieAnchorA = makePart("RootTieAnchorA", Vector3.new(2, 2, 2), true)
local tieMiddle = makePart("RootTieMiddle", Vector3.new(2, 2, 2))
local tieAnchorB = makePart("RootTieAnchorB", Vector3.new(2, 2, 2), true)
weld(tieAnchorB, tieMiddle)
weld(tieAnchorA, tieMiddle)

-- Rank beats the active link: the large anchor keeps the middle even when
-- the small anchor's weld is newer.
local sizeAnchorSmall = makePart("RootSizeAnchorSmall",
                                 Vector3.new(1, 1, 1), true)
local sizeMiddle = makePart("RootSizeMiddle", Vector3.new(2, 2, 2))
local sizeAnchorLarge = makePart("RootSizeAnchorLarge",
                                 Vector3.new(12, 12, 12), true)
weld(sizeMiddle, sizeAnchorLarge)
weld(sizeAnchorSmall, sizeMiddle)

-- Equal anchors several joints apart: newest anchor-incident link wins.
local longAnchorA = makePart("RootLongAnchorA", Vector3.new(2, 2, 2), true)
local longOne = makePart("RootLongOne", Vector3.new(2, 2, 2))
local longTwo = makePart("RootLongTwo", Vector3.new(2, 2, 2))
local longThree = makePart("RootLongThree", Vector3.new(2, 2, 2))
local longAnchorB = makePart("RootLongAnchorB", Vector3.new(2, 2, 2), true)
weld(longThree, longAnchorB)
weld(longTwo, longThree)
weld(longOne, longTwo)
weld(longAnchorA, longOne)

local orphan = Instance.new("Part")
orphan.Name = "RootOrphan"

task.wait(0.25)

report("isolated", isolated)
report("chain-anchor", anchor)
report("chain-mid", mid)
report("chain-tail", tail)
report("largest-small", small)
report("priority-peer", priorityPeer)
report("massless-large", massless)
report("humanoid-hrp", hrp)
report("humanoid-torso", torso)
report("mutual-one", mutualOne)
report("mutual-two", mutualTwo)
report("primary-small", primary)
report("primary-large", primaryLarge)
report("anchored-one", anchoredOne)
report("anchored-two", anchoredTwo)
report("anchor-tie", tieMiddle)
report("anchor-size", sizeMiddle)
report("anchor-long-chain", longTwo)
report("orphan", orphan)
local isolatedConnected = isolated:GetConnectedParts()
print("root-probe summary isolated=" .. rootName(isolated) ..
      " chain=" .. rootName(mid) ..
      " larger=" .. rootName(small) ..
      " priority=" .. rootName(priorityPeer) ..
      " massless=" .. rootName(massless) ..
      " humanoid=" .. rootName(hrp) ..
      " humanoidProperty=" .. tostring(humanoid.RootPart == hrp) ..
      " mutual=" .. rootName(mutualOne) ..
      " primary=" .. rootName(primary) ..
      " anchoredSplit=" ..
      tostring(anchoredOne:GetRootPart() == anchoredOne and
               anchoredTwo:GetRootPart() == anchoredTwo) ..
      " anchorTie=" .. rootName(tieMiddle) ..
      " anchorSize=" .. rootName(sizeMiddle) ..
      " longChain=" .. rootName(longTwo) ..
      " orphan=" .. rootName(orphan) ..
      " connectedSelf=" ..
      tostring(#isolatedConnected == 1 and isolatedConnected[1] == isolated))
