-- native_root_probe.lua — run inside the native client through Xecutor.
--
-- Characteristic rigid assemblies built in a local Workspace folder.  The
-- probe reports BasePart:GetRootPart(), BasePart.AssemblyRootPart and
-- Humanoid.RootPart so Boblox can match the native physics root choice.

local ws = game:GetService("Workspace")
local players = game:GetService("Players")

local function out(s)
    rconsoleprint("ROOTPROBE " .. s .. "\n")
    print("ROOTPROBE " .. s)
end

local player = players.LocalPlayer
local deadline = os.clock() + 25
while os.clock() < deadline and not (player and player.Character) do
    task.wait(0.5)
    player = players.LocalPlayer
end
out("start player=" .. tostring(player and player.Name))

local folder = Instance.new("Folder")
folder.Name = "RootProbeNative"
folder.Parent = ws

local function part(name, pos, size, anchored, massless)
    local p = Instance.new("Part")
    p.Name = name
    p.Size = size or Vector3.new(2, 2, 2)
    p.CFrame = CFrame.new(pos or Vector3.new(0, 500, 0))
    p.Anchored = anchored == true
    p.CanCollide = false
    p.Massless = massless == true
    p.Parent = folder
    return p
end

local function weld(name, a, b)
    local j = Instance.new("Weld")
    j.Name = name
    j.Part0 = a
    j.Part1 = b
    j.Parent = folder
    return j
end

local function rootName(p)
    local ok, root = pcall(function() return p:GetRootPart() end)
    if not ok then return "ERR:" .. tostring(root) end
    return root and root.Name or "nil"
end

local function report(tag, p)
    local assembly = "?"
    local okA, a = pcall(function() return p.AssemblyRootPart end)
    if okA then assembly = a and a.Name or "nil" else assembly = "ERR" end
    out(string.format("%s part=%s root=%s assembly=%s anchored=%s massless=%s priority=%d size=%s",
                      tag, p.Name, rootName(p), assembly, tostring(p.Anchored),
                      tostring(p.Massless), p.RootPriority,
                      tostring(p.Size)))
end

-- A: isolated part.
local isolated = part("RootA_Isolated", Vector3.new(500, 500, 500))
-- B: chain with an anchored member.
local chainA = part("RootB_Anchor", Vector3.new(510, 500, 500),
                    Vector3.new(2, 2, 2), true)
local chainB = part("RootB_Mid", Vector3.new(512, 500, 500))
local chainC = part("RootB_End", Vector3.new(514, 500, 500))
weld("RootB_WeldAB", chainA, chainB)
weld("RootB_WeldBC", chainB, chainC)
-- C: anchored member is small, unanchored member is large.
local anchorC = part("RootC_AnchorSmall", Vector3.new(520, 500, 500),
                     Vector3.new(1, 1, 1), true)
local bigC = part("RootC_Big", Vector3.new(522, 500, 500),
                  Vector3.new(12, 12, 12))
weld("RootC_Weld", anchorC, bigC)
-- D: unanchored pair of different sizes.
local bigD = part("RootD_Big", Vector3.new(530, 500, 500),
                  Vector3.new(12, 12, 12))
local smallD = part("RootD_Small", Vector3.new(532, 500, 500),
                    Vector3.new(2, 2, 2))
weld("RootD_Weld", bigD, smallD)
-- E: equal-size pair with RootPriority on one member.
local priorityE = part("RootE_Priority", Vector3.new(540, 500, 500))
local plainE = part("RootE_Plain", Vector3.new(542, 500, 500))
priorityE.RootPriority = 100
weld("RootE_Weld", priorityE, plainE)
-- F: large massless member welded to a small physical member.
local masslessF = part("RootF_Massless", Vector3.new(550, 500, 500),
                       Vector3.new(20, 20, 20), false, true)
local solidF = part("RootF_Solid", Vector3.new(552, 500, 500),
                    Vector3.new(2, 2, 2))
weld("RootF_Weld", masslessF, solidF)
-- G: Humanoid assembly.  HumanoidRootPart plus a welded torso/head.
local modelG = Instance.new("Model")
modelG.Name = "RootG_Character"
modelG.Parent = folder
local hrpG = part("RootG_HumanoidRootPart", Vector3.new(560, 500, 500),
                  Vector3.new(2, 2, 1))
local torsoG = part("RootG_Torso", Vector3.new(560, 500, 500),
                    Vector3.new(4, 4, 2))
local headG = part("RootG_Head", Vector3.new(560, 503, 500),
                   Vector3.new(2, 2, 2))
hrpG.Parent = modelG
torsoG.Parent = modelG
headG.Parent = modelG
weld("RootG_WeldHRP", hrpG, torsoG)
weld("RootG_WeldHead", torsoG, headG)
local humanoidG = Instance.new("Humanoid")
humanoidG.Parent = modelG
-- H: mutually welded pair (two joints between the same parts).
local mutualH1 = part("RootH_One", Vector3.new(570, 500, 500))
local mutualH2 = part("RootH_Two", Vector3.new(572, 500, 500))
weld("RootH_WeldForward", mutualH1, mutualH2)
weld("RootH_WeldBackward", mutualH2, mutualH1)
-- I: Model.PrimaryPart that is not the largest/anchored member.
local modelI = Instance.new("Model")
modelI.Name = "RootI_Model"
modelI.Parent = folder
local primaryI = part("RootI_Primary", Vector3.new(580, 500, 500),
                      Vector3.new(2, 2, 2))
local largestI = part("RootI_Largest", Vector3.new(582, 500, 500),
                      Vector3.new(12, 12, 12))
primaryI.Parent = modelI
largestI.Parent = modelI
weld("RootI_Weld", primaryI, largestI)
modelI.PrimaryPart = primaryI

task.wait(1.5)

report("A-isolated", isolated)
report("B-chain-anchor", chainA)
report("B-chain-mid", chainB)
report("B-chain-end", chainC)
report("C-anchor-small", anchorC)
report("C-big", bigC)
report("D-big", bigD)
report("D-small", smallD)
report("E-priority", priorityE)
report("E-plain", plainE)
report("F-massless", masslessF)
report("F-solid", solidF)
report("G-humanoid-root", hrpG)
report("G-humanoid-torso", torsoG)
report("G-humanoid-head", headG)
local okHR, hr = pcall(function() return humanoidG.RootPart end)
out("G-humanoid-property rootPart=" .. tostring(okHR and (hr and hr.Name or "nil") or "ERR"))
report("H-mutual-one", mutualH1)
report("H-mutual-two", mutualH2)
report("I-primary", primaryI)
report("I-largest", largestI)
out("I-model-primary=" .. tostring(modelI.PrimaryPart and modelI.PrimaryPart.Name or "nil"))
out("done")
