-- native_root_detail_probe.lua — native root-ordering details.
-- Checks actual character child order, equal-size Humanoid assemblies, the
-- anchored-part "root is itself" rule, GetConnectedParts on anchored pairs,
-- volume-vs-shape ties, priority-vs-size, and an all-massless assembly.

local ws = game:GetService("Workspace")
local players = game:GetService("Players")

local function out(s)
    rconsoleprint("ROOTDETAIL " .. s .. "\n")
    print("ROOTDETAIL " .. s)
end

local player = players.LocalPlayer
local deadline = os.clock() + 25
while os.clock() < deadline and not (player and player.Character) do
    task.wait(0.5)
    player = players.LocalPlayer
end
out("start")

local folder = Instance.new("Folder")
folder.Name = "RootDetailNative"
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
    out(string.format("%s part=%s root=%s connected=[%s] anchored=%s massless=%s priority=%d size=%s",
                      tag, p.Name, rootName(p), table.concat(connected, ","),
                      tostring(p.Anchored), tostring(p.Massless),
                      p.RootPriority, tostring(p.Size)))
end

-- Actual character child order and root.
local char = player and player.Character
if char then
    local names = {}
    for _, c in ipairs(char:GetChildren()) do names[#names + 1] = c.Name end
    out("CHAR children=[" .. table.concat(names, ",") .. "]")
    local hum = char:FindFirstChildOfClass("Humanoid")
    local hrp = char:FindFirstChild("HumanoidRootPart")
    local torso = char:FindFirstChild("Torso") or char:FindFirstChild("UpperTorso")
    if hrp then
        report("CHAR-hrp", hrp)
    end
    if torso then
        report("CHAR-torso", torso)
    end
    if hum then
        out("CHAR humanoidRootPart=" .. tostring(hum.RootPart and hum.RootPart.Name or "nil"))
    end
else
    out("CHAR unavailable")
end

-- Equal-size custom Humanoid assembly; HRP and torso inserted in both orders.
local modelG = Instance.new("Model")
modelG.Name = "RootDetail_Character"
modelG.Parent = folder
local torsoG = part("RootDetail_Torso", Vector3.new(700, 500, 500),
                    Vector3.new(2, 2, 1))
local hrpG = part("RootDetail_HumanoidRootPart", Vector3.new(700, 500, 500),
                  Vector3.new(2, 2, 1))
torsoG.Parent = modelG
hrpG.Parent = modelG
weld("RootDetail_Weld", torsoG, hrpG)
local humG = Instance.new("Humanoid")
humG.Parent = modelG
-- Same-size custom model with HRP inserted first.
local modelH = Instance.new("Model")
modelH.Name = "RootDetail_CharacterH"
modelH.Parent = folder
local hrpH = part("RootDetailH_HumanoidRootPart", Vector3.new(710, 500, 500),
                  Vector3.new(2, 2, 1))
local torsoH = part("RootDetailH_Torso", Vector3.new(710, 500, 500),
                    Vector3.new(2, 2, 1))
hrpH.Parent = modelH
torsoH.Parent = modelH
weld("RootDetailH_Weld", hrpH, torsoH)
local humH = Instance.new("Humanoid")
humH.Parent = modelH

-- Two anchored parts with a weld: each GetRootPart should be itself.
local a1 = part("RootDetail_AnchorOne", Vector3.new(720, 500, 500),
                Vector3.new(1, 1, 1), true)
local a2 = part("RootDetail_AnchorTwo", Vector3.new(722, 500, 500),
                Vector3.new(12, 12, 12), true)
weld("RootDetail_AnchorWeld", a1, a2)

-- Isolated anchored part.
local soloAnchor = part("RootDetail_SoloAnchor", Vector3.new(730, 500, 500),
                        Vector3.new(3, 3, 3), true)

-- Same volume, different shape.
local shapeA = part("RootDetail_ShapeA", Vector3.new(740, 500, 500),
                    Vector3.new(1, 2, 8))
local shapeB = part("RootDetail_ShapeB", Vector3.new(742, 500, 500),
                    Vector3.new(2, 2, 4))
weld("RootDetail_ShapeWeld", shapeA, shapeB)

-- Priority on a small unanchored part vs a much larger unanchored part.
local priSmall = part("RootDetail_PriSmall", Vector3.new(750, 500, 500),
                      Vector3.new(1, 1, 1))
local priLarge = part("RootDetail_PriLarge", Vector3.new(752, 500, 500),
                      Vector3.new(20, 20, 20))
priSmall.RootPriority = 100
weld("RootDetail_PriWeld", priSmall, priLarge)

-- All parts massless.
local mlA = part("RootDetail_MasslessA", Vector3.new(760, 500, 500),
                 Vector3.new(2, 2, 2), false, true)
local mlB = part("RootDetail_MasslessB", Vector3.new(762, 500, 500),
                 Vector3.new(2, 2, 2), false, true)
weld("RootDetail_MasslessWeld", mlA, mlB)

task.wait(1.5)

if hrpG and torsoG then
    report("G-hrp", hrpG)
    report("G-torso", torsoG)
    local okR, r = pcall(function() return humG.RootPart end)
    out("G-humanoidRootPart=" .. tostring(okR and (r and r.Name or "nil") or "ERR"))
end
if hrpH and torsoH then
    report("H-hrp", hrpH)
    report("H-torso", torsoH)
    local okR, r = pcall(function() return humH.RootPart end)
    out("H-humanoidRootPart=" .. tostring(okR and (r and r.Name or "nil") or "ERR"))
end
report("ANCHORED-one", a1)
report("ANCHORED-two", a2)
report("SOLO-anchored", soloAnchor)
report("SHAPE-a", shapeA)
report("SHAPE-b", shapeB)
report("PRIORITY-small", priSmall)
report("PRIORITY-large", priLarge)
report("MASSLESS-a", mlA)
report("MASSLESS-b", mlB)
out("done")
