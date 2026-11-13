-- native_root_ties_probe.lua — second native evidence run for root ordering.
-- Focus: equal-size ties, weld direction, mass/density, anchored precedence,
-- root-priority precedence, and the real player character's assembly.

local ws = game:GetService("Workspace")
local players = game:GetService("Players")

local function out(s)
    rconsoleprint("ROOTTIE " .. s .. "\n")
    print("ROOTTIE " .. s)
end

local player = players.LocalPlayer
local deadline = os.clock() + 25
while os.clock() < deadline and not (player and player.Character) do
    task.wait(0.5)
    player = players.LocalPlayer
end
out("start")

local folder = Instance.new("Folder")
folder.Name = "RootTieNative"
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
    local mass = "?"
    local okM, m = pcall(function() return p.Mass end)
    if okM then mass = tostring(m) end
    out(string.format("%s part=%s root=%s assembly=%s anchored=%s massless=%s priority=%d mass=%s size=%s",
                      tag, p.Name, rootName(p), assembly, tostring(p.Anchored),
                      tostring(p.Massless), p.RootPriority, mass,
                      tostring(p.Size)))
end

-- Equal-size tie, one weld A -> B.
local t1a = part("RootTie1_A", Vector3.new(600, 500, 500))
local t1b = part("RootTie1_B", Vector3.new(602, 500, 500))
weld("RootTie1_Weld", t1a, t1b)
-- Equal-size tie, one weld B -> A.
local t2a = part("RootTie2_A", Vector3.new(610, 500, 500))
local t2b = part("RootTie2_B", Vector3.new(612, 500, 500))
weld("RootTie2_Weld", t2b, t2a)
-- Equal-size tie, two opposite welds.
local t3a = part("RootTie3_A", Vector3.new(620, 500, 500))
local t3b = part("RootTie3_B", Vector3.new(622, 500, 500))
weld("RootTie3_WeldAB", t3a, t3b)
weld("RootTie3_WeldBA", t3b, t3a)
-- Equal-size tie where B was created before A.
local t4b = part("RootTie4_B", Vector3.new(632, 500, 500))
local t4a = part("RootTie4_A", Vector3.new(630, 500, 500))
weld("RootTie4_Weld", t4a, t4b)
-- Density/mass difference at equal size.
local t5a = part("RootTie5_Dense", Vector3.new(640, 500, 500),
                 Vector3.new(4, 4, 4))
local t5b = part("RootTie5_Light", Vector3.new(642, 500, 500),
                 Vector3.new(4, 4, 4))
t5a.CustomPhysicalProperties = PhysicalProperties.new(20, 0.3, 0.5, 1, 1)
t5b.CustomPhysicalProperties = PhysicalProperties.new(1, 0.3, 0.5, 1, 1)
weld("RootTie5_Weld", t5a, t5b)
-- High RootPriority on a massless part.
local t6massless = part("RootTie6_Massless", Vector3.new(650, 500, 500),
                        Vector3.new(20, 20, 20), false, true)
local t6solid = part("RootTie6_Solid", Vector3.new(652, 500, 500),
                     Vector3.new(2, 2, 2))
t6massless.RootPriority = 100
weld("RootTie6_Weld", t6massless, t6solid)
-- Anchored small vs unanchored high-priority large.
local t7anchor = part("RootTie7_Anchor", Vector3.new(660, 500, 500),
                      Vector3.new(1, 1, 1), true)
local t7priority = part("RootTie7_Priority", Vector3.new(662, 500, 500),
                        Vector3.new(20, 20, 20))
t7priority.RootPriority = 100
weld("RootTie7_Weld", t7anchor, t7priority)
-- Two anchored parts of different sizes.
local t8small = part("RootTie8_Small", Vector3.new(670, 500, 500),
                     Vector3.new(1, 1, 1), true)
local t8large = part("RootTie8_Large", Vector3.new(672, 500, 500),
                     Vector3.new(12, 12, 12), true)
weld("RootTie8_Weld", t8small, t8large)
-- Two anchored parts with priority on the smaller one.
local t9small = part("RootTie9_Small", Vector3.new(680, 500, 500),
                     Vector3.new(1, 1, 1), true)
local t9large = part("RootTie9_Large", Vector3.new(682, 500, 500),
                     Vector3.new(12, 12, 12), true)
t9small.RootPriority = 100
weld("RootTie9_Weld", t9small, t9large)
-- Two anchored parts, priority on the larger one.
local t10small = part("RootTie10_Small", Vector3.new(690, 500, 500),
                      Vector3.new(1, 1, 1), true)
local t10large = part("RootTie10_Large", Vector3.new(692, 500, 500),
                      Vector3.new(12, 12, 12), true)
t10large.RootPriority = 100
weld("RootTie10_Weld", t10small, t10large)

task.wait(1.5)

for _, row in ipairs({
    {"T1-one-weld-AtoB", t1a}, {"T1-peer", t1b},
    {"T2-one-weld-BtoA", t2a}, {"T2-peer", t2b},
    {"T3-two-welds", t3a}, {"T3-peer", t3b},
    {"T4-create-B-first", t4a}, {"T4-peer", t4b},
    {"T5-dense", t5a}, {"T5-light", t5b},
    {"T6-massless-priority", t6massless}, {"T6-solid", t6solid},
    {"T7-anchor", t7anchor}, {"T7-priority", t7priority},
    {"T8-anchored-small", t8small}, {"T8-anchored-large", t8large},
    {"T9-anchored-small-priority", t9small}, {"T9-anchored-large", t9large},
    {"T10-anchored-small", t10small}, {"T10-anchored-large-priority", t10large},
}) do
    report(row[1], row[2])
end

local char = player and player.Character
local hum = char and char:FindFirstChildOfClass("Humanoid")
local hrp = char and char:FindFirstChild("HumanoidRootPart")
if char and hum and hrp then
    out(string.format("CHAR humanoidRootPartProperty=%s hrpGetRoot=%s hrpPriority=%d hrpSize=%s",
                      tostring(hum.RootPart and hum.RootPart.Name or "nil"),
                      rootName(hrp), hrp.RootPriority, tostring(hrp.Size)))
    local torso = char:FindFirstChild("Torso") or char:FindFirstChild("UpperTorso")
    if torso then
        out(string.format("CHAR torso=%s torsoGetRoot=%s torsoPriority=%d torsoSize=%s",
                          torso.Name, rootName(torso), torso.RootPriority,
                          tostring(torso.Size)))
    end
else
    out("CHAR unavailable")
end
out("done")
