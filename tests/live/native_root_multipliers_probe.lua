-- native_root_multipliers_probe.lua — native multipliers and Humanoid binding.
local ws = game:GetService("Workspace")

local function out(s)
    rconsoleprint("ROOTMUL " .. s .. "\n")
    print("ROOTMUL " .. s)
end

local function rootName(p)
    local ok, root = pcall(function() return p:GetRootPart() end)
    if not ok then return "ERR" end
    return root and root.Name or "nil"
end

local function part(name, pos, size, class)
    local p = Instance.new(class or "Part")
    p.Name = name
    p.Size = size or Vector3.new(2, 2, 1)
    p.CFrame = CFrame.new(pos or Vector3.new(0, 500, 0))
    p.Anchored = false
    p.CanCollide = false
    p.Parent = ws
    return p
end

local function weld(a, b)
    local j = Instance.new("Weld")
    j.Part0 = a
    j.Part1 = b
    j.Parent = ws
    return j
end

local function report(tag, p)
    out(string.format("%s part=%s class=%s root=%s priority=%d size=%s",
                      tag, p.Name, p.ClassName, rootName(p), p.RootPriority,
                      tostring(p.Size)))
end

out("start")

-- Equal normal parts named HumanoidRootPart and Torso, no Humanoid.
local nameHRP = part("HumanoidRootPart", Vector3.new(900, 500, 500))
local nameTorso = part("Torso", Vector3.new(902, 500, 500))
weld(nameHRP, nameTorso)
-- Equal normal parts named Torso and a neutral name.
local nameTorso2 = part("Torso", Vector3.new(910, 500, 500))
local nameNeutral = part("RootMul_Neutral", Vector3.new(912, 500, 500))
weld(nameTorso2, nameNeutral)
-- Equal Seat and normal part.
local seatA = part("RootMul_Seat", Vector3.new(920, 500, 500),
                   Vector3.new(2, 2, 1), "Seat")
local seatPeer = part("RootMul_SeatPeer", Vector3.new(922, 500, 500))
weld(seatA, seatPeer)
-- Equal VehicleSeat and normal part.
local vseat = part("RootMul_VehicleSeat", Vector3.new(930, 500, 500),
                   Vector3.new(2, 2, 1), "VehicleSeat")
local vseatPeer = part("RootMul_VehicleSeatPeer", Vector3.new(932, 500, 500))
weld(vseat, vseatPeer)

-- Equal-size character model directly under Workspace, both insertion orders.
local modelA = Instance.new("Model")
modelA.Name = "RootMul_CharA"
modelA.Parent = ws
local torsoA = part("Torso", Vector3.new(940, 500, 500))
local hrpA = part("HumanoidRootPart", Vector3.new(940, 500, 500))
torsoA.Parent = modelA
hrpA.Parent = modelA
weld(torsoA, hrpA)
local humA = Instance.new("Humanoid")
humA.Parent = modelA

local modelB = Instance.new("Model")
modelB.Name = "RootMul_CharB"
modelB.Parent = ws
local hrpB = part("HumanoidRootPart", Vector3.new(950, 500, 500))
local torsoB = part("Torso", Vector3.new(950, 500, 500))
hrpB.Parent = modelB
torsoB.Parent = modelB
weld(hrpB, torsoB)
local humB = Instance.new("Humanoid")
humB.Parent = modelB

-- Try setting Humanoid.RootPart explicitly on a third model.
local modelC = Instance.new("Model")
modelC.Name = "RootMul_CharC"
modelC.Parent = ws
local torsoC = part("Torso", Vector3.new(960, 500, 500))
local hrpC = part("HumanoidRootPart", Vector3.new(960, 500, 500))
torsoC.Parent = modelC
hrpC.Parent = modelC
weld(torsoC, hrpC)
local humC = Instance.new("Humanoid")
humC.Parent = modelC
local okSet, errSet = pcall(function() humC.RootPart = hrpC end)

task.wait(1.5)

report("NAME-hrp", nameHRP)
report("NAME-torso", nameTorso)
report("NAME2-torso", nameTorso2)
report("NAME2-neutral", nameNeutral)
report("SEAT", seatA)
report("SEAT-peer", seatPeer)
report("VSEAT", vseat)
report("VSEAT-peer", vseatPeer)
report("CHARA-hrp", hrpA)
report("CHARA-torso", torsoA)
report("CHARB-hrp", hrpB)
report("CHARB-torso", torsoB)
out("CHARA.RootPart=" .. tostring(pcall(function() return humA.RootPart and humA.RootPart.Name or "nil" end)))
out("CHARB.RootPart=" .. tostring(pcall(function() return humB.RootPart and humB.RootPart.Name or "nil" end)))
out("CHARC.setRootPart ok=" .. tostring(okSet) .. " err=" .. tostring(errSet))
out("CHARC.hrp=" .. rootName(hrpC) .. " torso=" .. rootName(torsoC))
out("done")
