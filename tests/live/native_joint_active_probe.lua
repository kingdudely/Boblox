-- native_joint_active_probe.lua — native JointInstance.Active semantics.
-- Prints one stable ACTIVE line per case so the Boblox probe can be compared
-- line by line.  Uses only API available in the live 1818 client.
local ws = game:GetService("Workspace")

local function out(s)
    rconsoleprint("ACTIVE " .. s .. "\n")
    print("ACTIVE " .. s)
end

local folder = Instance.new("Folder")
folder.Name = "JointActiveNative"
folder.Parent = ws

local function part(name, pos, size, anchored, massless, parent)
    local p = Instance.new("Part")
    p.Name = name
    p.Size = size or Vector3.new(2, 2, 2)
    p.CFrame = CFrame.new(pos or Vector3.new(0, 500, 0))
    p.Anchored = anchored == true
    p.CanCollide = false
    p.Massless = massless == true
    p.Parent = parent or folder
    return p
end

local function weld(name, a, b)
    local j = Instance.new("Weld")
    j.Name = name
    j.Part0 = a
    j.Part1 = b
    j.Parent = a.Parent
    return j
end

local function rootName(p)
    local ok, root = pcall(function() return p:GetRootPart() end)
    if not ok then return "ERR" end
    return root and root.Name or "nil"
end

local function flags(...)
    local bits = {}
    for i = 1, select("#", ...) do
        local j = select(i, ...)
        bits[i] = j.Name .. "=" .. tostring(j.Active)
    end
    return table.concat(bits, " ")
end

-- Equal anchored parts with an unanchored middle.  The newer A-middle weld
-- is the native active link; B-middle is the inactive one.
local a = part("ActiveSplitA", Vector3.new(1400, 500, 500),
               Vector3.new(2, 2, 2), true)
local m = part("ActiveSplitM", Vector3.new(1402, 500, 500),
               Vector3.new(2, 2, 2), false)
local b = part("ActiveSplitB", Vector3.new(1404, 500, 500),
               Vector3.new(2, 2, 2), true)
local wb = weld("ActiveSplitWB", m, b)
local wa = weld("ActiveSplitWA", a, m)

-- Humanoid assembly: exact HumanoidRootPart name gives the native root.
local model = Instance.new("Model")
model.Name = "ActiveHumanoidModel"
model.Parent = ws
local hrp = part("HumanoidRootPart", Vector3.new(1410, 500, 500),
                 Vector3.new(2, 2, 1), false, false, model)
local torso = part("Torso", Vector3.new(1412, 500, 500),
                   Vector3.new(2, 2, 1), false, false, model)
local humanoidWeld = weld("ActiveHumanoidWeld", hrp, torso)
local humanoid = Instance.new("Humanoid")
humanoid.Parent = model

-- Mutual welds between equal unanchored parts.
local one = part("ActiveMutualOne", Vector3.new(1420, 500, 500))
local two = part("ActiveMutualTwo", Vector3.new(1422, 500, 500))
local w12 = weld("ActiveMutual12", one, two)
local w21 = weld("ActiveMutual21", two, one)

-- Massless large part versus solid small part.
local massless = part("ActiveMassless", Vector3.new(1430, 500, 500),
                      Vector3.new(20, 20, 20), false, true)
local solid = part("ActiveSolid", Vector3.new(1432, 500, 500),
                   Vector3.new(2, 2, 2))
local masslessWeld = weld("ActiveMasslessWeld", massless, solid)

-- Both sides anchored; each part is its own root.
local anchoredOne = part("ActiveAnchoredOne", Vector3.new(1440, 500, 500),
                         Vector3.new(2, 2, 2), true)
local anchoredTwo = part("ActiveAnchoredTwo", Vector3.new(1442, 500, 500),
                         Vector3.new(2, 2, 2), true)
local anchoredWeld = weld("ActiveAnchoredWeld", anchoredOne, anchoredTwo)

-- Disabled joint probe: does Active follow Enabled?
local disabledA = part("ActiveDisabledA", Vector3.new(1450, 500, 500))
local disabledB = part("ActiveDisabledB", Vector3.new(1452, 500, 500))
local disabledWeld = weld("ActiveDisabledWeld", disabledA, disabledB)
disabledWeld.Enabled = false

-- WeldConstraint has its own inherited Active property.
local wcA = part("ActiveWCA", Vector3.new(1470, 500, 500))
local wcB = part("ActiveWCB", Vector3.new(1472, 500, 500))
local wc = Instance.new("WeldConstraint")
wc.Name = "ActiveWC"
wc.Part0 = wcA
wc.Part1 = wcB
wc.Parent = folder

-- A joint outside the Workspace.
local outside = Instance.new("Folder")
outside.Name = "ActiveOutside"
outside.Parent = game
local outsideA = part("ActiveOutsideA", Vector3.new(1480, 500, 500),
                      Vector3.new(2, 2, 2), false, false, outside)
local outsideB = part("ActiveOutsideB", Vector3.new(1482, 500, 500),
                      Vector3.new(2, 2, 2), false, false, outside)
local outsideWeld = weld("ActiveOutsideWeld", outsideA, outsideB)

task.wait(1.5)

out("anchoredSplit root=" .. rootName(m) .. " " .. flags(wa, wb))
out("humanoid root=" .. rootName(hrp) .. " " .. flags(humanoidWeld))
out("mutual root=" .. rootName(one) .. " " .. flags(w12, w21))
out("massless root=" .. rootName(massless) .. " " .. flags(masslessWeld))
out("fullyAnchored oneRoot=" .. rootName(anchoredOne) ..
    " twoRoot=" .. rootName(anchoredTwo) .. " " .. flags(anchoredWeld))
out("disabled root=" .. rootName(disabledA) .. " " .. flags(disabledWeld))
out("weldConstraint root=" .. rootName(wcA) .. " " .. flags(wc))
out("outside root=" .. rootName(outsideA) .. " " .. flags(outsideWeld))

local writeOk, writeErr = pcall(function() wa.Active = false end)
out("write ok=" .. tostring(writeOk) .. " error=" ..
    tostring(writeErr) .. " after=" .. flags(wa, wb))

local enableOk, enableErr = pcall(function() wa.Enabled = false end)
out("disable ok=" .. tostring(enableOk) .. " error=" ..
    tostring(enableErr) .. " active=" .. flags(wa, wb))

out("done")
