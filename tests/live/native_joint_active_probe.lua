-- native_joint_active_probe.lua — native JointInstance.Active semantics.
-- Prints one stable ACTIVE line per case so the Boblox probe can be compared
-- line by line.  Uses only API available in the live 1818 client.
local ws = game:GetService("Workspace")
ws.Gravity = 0

local function out(s)
    rconsoleprint("ACTIVE " .. s .. "\n")
    print("ACTIVE " .. s)
end

local folder = Instance.new("Folder")
folder.Name = "JointActiveNative"
folder.Parent = ws

local function part(name, pos, size, anchored, massless, parent)
    pos = pos or Vector3.new(0, 500, 0)
    local p = Instance.new("Part")
    p.Name = name
    p.Size = size or Vector3.new(2, 2, 2)
    p.CFrame = CFrame.new(pos.X, pos.Y, pos.Z)
    p.Anchored = anchored == true
    p.CanCollide = false
    p.Massless = massless == true
    p.Parent = parent or folder
    return p
end

local function joint(cls, name, a, b)
    local j = Instance.new(cls)
    j.Name = name
    j.Part0 = a
    j.Part1 = b
    j.Parent = a.Parent
    return j
end

local function weld(name, a, b)
    return joint("Weld", name, a, b)
end

local function rootName(p)
    local ok, root = pcall(function() return p:GetRootPart() end)
    if not ok then return "ERR" end
    return root and root.Name or "nil"
end

local function conn(p)
    local ok, list = pcall(function() return p:GetConnectedParts(true) end)
    if not ok then return "ERR" end
    return tostring(#list)
end

local function flags(...)
    local bits = {}
    for i = 1, select("#", ...) do
        local j = select(i, ...)
        bits[i] = j.Name .. "=" .. tostring(j.Active)
    end
    return table.concat(bits, " ")
end

local function cfString(c)
    if not c then return "nil" end
    local got = {c:GetComponents()}
    for i = 1, 12 do
        if math.abs(got[i]) < 1e-6 then got[i] = 0 end
        got[i] = string.format("%.3f", got[i])
    end
    return table.concat(got, ",")
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

-- Legacy surface joints: Glue and ManualGlue are rigid and participate in
-- the same Active forest; Rotate and VelocityMotor are non-rigid and never
-- report Active.  There is no native Hinge instance class.
local glueA = part("ActiveGlueA", Vector3.new(1500, 500, 500))
local glueB = part("ActiveGlueB", Vector3.new(1502, 500, 500))
local glue = joint("Glue", "ActiveGlue", glueA, glueB)

local manualGlueA = part("ActiveManualGlueA", Vector3.new(1510, 500, 500))
local manualGlueB = part("ActiveManualGlueB", Vector3.new(1512, 500, 500))
local manualGlue = joint("ManualGlue", "ActiveManualGlue", manualGlueA,
                         manualGlueB)

local rotateA = part("ActiveRotateA", Vector3.new(1520, 500, 500))
local rotateB = part("ActiveRotateB", Vector3.new(1522, 500, 500))
local rotate = joint("Rotate", "ActiveRotate", rotateA, rotateB)

local velocityA = part("ActiveVelocityA", Vector3.new(1530, 500, 500))
local velocityB = part("ActiveVelocityB", Vector3.new(1532, 500, 500))
local velocity = joint("VelocityMotor", "ActiveVelocity", velocityA,
                       velocityB)

local dupA = part("ActiveDupGlueA", Vector3.new(1540, 500, 500))
local dupB = part("ActiveDupGlueB", Vector3.new(1542, 500, 500))
local dupFirst = joint("Glue", "ActiveDupGlueFirst", dupA, dupB)
local dupSecond = joint("Glue", "ActiveDupGlueSecond", dupA, dupB)

local mixA = part("ActiveMixA", Vector3.new(1550, 500, 500))
local mixB = part("ActiveMixB", Vector3.new(1552, 500, 500))
local mixWeld = weld("ActiveMixWeld", mixA, mixB)
local mixGlue = joint("Glue", "ActiveMixGlue", mixA, mixB)

local detachA = part("ActiveDetachA", Vector3.new(1560, 500, 500))
local detachB = part("ActiveDetachB", Vector3.new(1562, 500, 500))
local detachC = part("ActiveDetachC", Vector3.new(1564, 500, 500))
local detachGlue = joint("Glue", "ActiveDetachGlue", detachA, detachB)

local glueAnchorA = part("ActiveGlueAnchorA", Vector3.new(1570, 500, 500),
                         nil, true)
local glueAnchorB = part("ActiveGlueAnchorB", Vector3.new(1572, 500, 500),
                         nil, true)
local glueAnchored = joint("Glue", "ActiveGlueAnchored", glueAnchorA,
                           glueAnchorB)

local cycleA = part("ActiveGlueCycleA", Vector3.new(1580, 500, 500))
local cycleB = part("ActiveGlueCycleB", Vector3.new(1582, 500, 500))
local cycleC = part("ActiveGlueCycleC", Vector3.new(1584, 500, 500))
local cycleW1 = weld("ActiveGlueCycleW1", cycleA, cycleB)
local cycleGlue = joint("Glue", "ActiveGlueCycleJ", cycleB, cycleC)
local cycleW3 = weld("ActiveGlueCycleW3", cycleC, cycleA)

-- SurfaceType.Hinge: native MakeJoints is asynchronous and evaluates the
-- contact pair's top/bottom surfaces.  The part whose surface wins owns the
-- Rotate (Top Hinge first, then the other part's Bottom Hinge), its C0/C1
-- are the constant local frames printed below, and Active stays false.
local hingeFolder = Instance.new("Folder")
hingeFolder.Name = "HingeSurface"
hingeFolder.Parent = ws

local function hingePart(name, pos)
    local p = part(name, pos, Vector3.new(2, 1, 2), false, false, hingeFolder)
    p.CanCollide = true
    return p
end

local hingeA = hingePart("HingeSurfaceA", Vector3.new(1600, 500, 500))
local hingeB = hingePart("HingeSurfaceB", Vector3.new(1600, 501, 500))
hingeA.TopSurface = Enum.SurfaceType.Hinge
hingeB.BottomSurface = Enum.SurfaceType.Smooth
hingeA:MakeJoints()

local hingeC = hingePart("HingeSurfaceC", Vector3.new(1604, 500, 500))
local hingeD = hingePart("HingeSurfaceD", Vector3.new(1604, 501, 500))
hingeC.TopSurface = Enum.SurfaceType.Smooth
hingeD.BottomSurface = Enum.SurfaceType.Hinge
hingeD:MakeJoints()

-- The face normal does not have to be world-up: native still creates the
-- Rotate for a Z90, X90 and Z180 caller (the rotation probe first proved it).
local hingeRZ = part("HingeRotZ", Vector3.new(1608, 500, 500),
                     Vector3.new(2, 1, 2), false, false, hingeFolder)
local hingeRZB = part("HingeRotZB", Vector3.new(1606.5, 500, 500),
                      Vector3.new(2, 2, 2), false, false, hingeFolder)
hingeRZ.CFrame = CFrame.new(1608, 500, 500) *
    CFrame.fromAxisAngle(Vector3.new(0, 0, 1), math.rad(90))
hingeRZB.CFrame = CFrame.new(1606.5, 500, 500)
hingeRZ.CanCollide = true
hingeRZB.CanCollide = true
hingeRZ.TopSurface = Enum.SurfaceType.Hinge
hingeRZB.BottomSurface = Enum.SurfaceType.Smooth
hingeRZ:MakeJoints()

local hingeRX = part("HingeRotX", Vector3.new(1612, 500, 500),
                     Vector3.new(2, 1, 2), false, false, hingeFolder)
local hingeRXB = part("HingeRotXB", Vector3.new(1612, 500, 501.5),
                      Vector3.new(2, 2, 2), false, false, hingeFolder)
hingeRX.CFrame = CFrame.new(1612, 500, 500) *
    CFrame.fromAxisAngle(Vector3.new(1, 0, 0), math.rad(90))
hingeRXB.CFrame = CFrame.new(1612, 500, 501.5)
hingeRX.CanCollide = true
hingeRXB.CanCollide = true
hingeRX.TopSurface = Enum.SurfaceType.Hinge
hingeRXB.BottomSurface = Enum.SurfaceType.Smooth
hingeRX:MakeJoints()

local hingeR180 = part("HingeRot180", Vector3.new(1616, 500, 500),
                       Vector3.new(2, 1, 2), false, false, hingeFolder)
local hingeR180B = part("HingeRot180B", Vector3.new(1616, 498.5, 500),
                        Vector3.new(2, 2, 2), false, false, hingeFolder)
hingeR180.CFrame = CFrame.new(1616, 500, 500) *
    CFrame.fromAxisAngle(Vector3.new(0, 0, 1), math.rad(180))
hingeR180B.CFrame = CFrame.new(1616, 498.5, 500)
hingeR180.CanCollide = true
hingeR180B.CanCollide = true
hingeR180.TopSurface = Enum.SurfaceType.Hinge
hingeR180B.BottomSurface = Enum.SurfaceType.Smooth
hingeR180:MakeJoints()

-- The contact pair, not just the receiver: a Smooth caller touching a
-- Bottom-Hinge part creates a Rotate owned by the Hinge part.
local hingeOtherE = hingePart("HingeOtherE", Vector3.new(1590, 500, 500))
local hingeOtherF = hingePart("HingeOtherF", Vector3.new(1590, 501, 500))
hingeOtherE.TopSurface = Enum.SurfaceType.Smooth
hingeOtherF.BottomSurface = Enum.SurfaceType.Hinge
hingeOtherE:MakeJoints()

-- A Top Motor/SteppingMotor wins over the other part's Bottom Hinge.
local hingeMotorG = hingePart("HingeMotorG", Vector3.new(1592, 500, 500))
local hingeMotorH = hingePart("HingeMotorH", Vector3.new(1592, 501, 500))
hingeMotorG.TopSurface = Enum.SurfaceType.Motor
hingeMotorH.BottomSurface = Enum.SurfaceType.Hinge
hingeMotorG:MakeJoints()

local hingeStepI = hingePart("HingeStepI", Vector3.new(1594, 500, 500))
local hingeStepJ = hingePart("HingeStepJ", Vector3.new(1594, 501, 500))
hingeStepI.TopSurface = Enum.SurfaceType.SteppingMotor
hingeStepJ.BottomSurface = Enum.SurfaceType.Hinge
hingeStepI:MakeJoints()

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
out("glue root=" .. rootName(glueA) .. " conn=" .. conn(glueA) ..
    " " .. flags(glue))
out("glueAnchored oneRoot=" .. rootName(glueAnchorA) .. " twoRoot=" ..
    rootName(glueAnchorB) .. " " .. flags(glueAnchored))
out("glueCycle root=" .. rootName(cycleA) .. " " ..
    flags(cycleW1, cycleGlue, cycleW3))
out("manualGlue root=" .. rootName(manualGlueA) .. " conn=" ..
    conn(manualGlueA) .. " " .. flags(manualGlue))
out("nonRigid rootA=" .. rootName(rotateA) .. " rootB=" ..
    rootName(rotateB) .. " connA=" .. conn(rotateA) .. " " ..
    flags(rotate, velocity))
out("duplicateGlue root=" .. rootName(dupA) .. " " ..
    flags(dupFirst, dupSecond))
out("mixedPair root=" .. rootName(mixA) .. " " ..
    flags(mixWeld, mixGlue))
out("detach initial root=" .. rootName(detachA) .. " " ..
    flags(detachGlue))
detachGlue.Part1 = nil
task.wait(0.5)
out("detach split root=" .. rootName(detachA) .. " " ..
    flags(detachGlue))
detachGlue.Part1 = detachC
task.wait(0.5)
out("detach reattached root=" .. rootName(detachA) .. " " ..
    flags(detachGlue))

local writeOk, writeErr = pcall(function() wa.Active = false end)
local hingeJoint = hingeA:GetJoints()[1]
out("hingeSurface class=" .. tostring(hingeJoint and hingeJoint.ClassName) ..
    " name=" .. tostring(hingeJoint and hingeJoint.Name) ..
    " parent=" .. tostring(hingeJoint and hingeJoint.Parent == hingeA) ..
    " parts=" .. tostring(hingeJoint and hingeJoint.Part0 == hingeA and
        hingeJoint.Part1 == hingeB) ..
    " active=" .. tostring(hingeJoint and hingeJoint.Active) ..
    " c0=" .. cfString(hingeJoint and hingeJoint.C0) ..
    " c1=" .. cfString(hingeJoint and hingeJoint.C1))

out("hingeRotZ90 class=" ..
    tostring(hingeRZ:GetJoints()[1] and hingeRZ:GetJoints()[1].ClassName) ..
    " c0=" .. cfString(hingeRZ:GetJoints()[1] and
        hingeRZ:GetJoints()[1].C0))
out("hingeRotX90 class=" ..
    tostring(hingeRX:GetJoints()[1] and hingeRX:GetJoints()[1].ClassName) ..
    " c0=" .. cfString(hingeRX:GetJoints()[1] and
        hingeRX:GetJoints()[1].C0))
out("hingeRotZ180 class=" ..
    tostring(hingeR180:GetJoints()[1] and hingeR180:GetJoints()[1].ClassName) ..
    " c0=" .. cfString(hingeR180:GetJoints()[1] and
        hingeR180:GetJoints()[1].C0))

local hingeBottomJoint = hingeD:GetJoints()[1]
out("hingeBottom class=" ..
    tostring(hingeBottomJoint and hingeBottomJoint.ClassName) ..
    " parent=" .. tostring(hingeBottomJoint and hingeBottomJoint.Parent == hingeD) ..
    " parts=" .. tostring(hingeBottomJoint and hingeBottomJoint.Part0 == hingeD and
        hingeBottomJoint.Part1 == hingeC) ..
    " active=" .. tostring(hingeBottomJoint and hingeBottomJoint.Active) ..
    " c0=" .. cfString(hingeBottomJoint and hingeBottomJoint.C0) ..
    " c1=" .. cfString(hingeBottomJoint and hingeBottomJoint.C1))

local hingeOtherJoint = hingeOtherF:GetJoints()[1]
out("hingeOther class=" ..
    tostring(hingeOtherJoint and hingeOtherJoint.ClassName) ..
    " parent=" .. tostring(hingeOtherJoint and hingeOtherJoint.Parent == hingeOtherF) ..
    " parts=" .. tostring(hingeOtherJoint and hingeOtherJoint.Part0 == hingeOtherF and
        hingeOtherJoint.Part1 == hingeOtherE) ..
    " c0=" .. cfString(hingeOtherJoint and hingeOtherJoint.C0) ..
    " c1=" .. cfString(hingeOtherJoint and hingeOtherJoint.C1))

local hingeMotorJoint = hingeMotorG:GetJoints()[1]
out("hingeMotor class=" ..
    tostring(hingeMotorJoint and hingeMotorJoint.ClassName) ..
    " parent=" .. tostring(hingeMotorJoint and hingeMotorJoint.Parent == hingeMotorG) ..
    " parts=" .. tostring(hingeMotorJoint and hingeMotorJoint.Part0 == hingeMotorG and
        hingeMotorJoint.Part1 == hingeMotorH) ..
    " c0=" .. cfString(hingeMotorJoint and hingeMotorJoint.C0) ..
    " c1=" .. cfString(hingeMotorJoint and hingeMotorJoint.C1))

local hingeStepJoint = hingeStepI:GetJoints()[1]
out("hingeStep class=" ..
    tostring(hingeStepJoint and hingeStepJoint.ClassName) ..
    " parent=" .. tostring(hingeStepJoint and hingeStepJoint.Parent == hingeStepI) ..
    " parts=" .. tostring(hingeStepJoint and hingeStepJoint.Part0 == hingeStepI and
        hingeStepJoint.Part1 == hingeStepJ) ..
    " c0=" .. cfString(hingeStepJoint and hingeStepJoint.C0) ..
    " c1=" .. cfString(hingeStepJoint and hingeStepJoint.C1))

hingeA:BreakJoints()
out("hingeBreak joints=" .. tostring(#hingeA:GetJoints()) .. " class=" ..
    tostring(hingeA:GetJoints()[1] and hingeA:GetJoints()[1].ClassName))

if hingeJoint then hingeJoint:Destroy() end
hingeA:MakeJoints()
task.wait(1)
local hingeRecreated = hingeA:GetJoints()[1]
out("hingeRecreate class=" ..
    tostring(hingeRecreated and hingeRecreated.ClassName) .. " different=" ..
    tostring(hingeRecreated ~= nil and hingeRecreated ~= hingeJoint))

if hingeRecreated then hingeRecreated:Destroy() end
hingeB.CFrame = CFrame.new(1610, 501, 500)
hingeA:MakeJoints()
task.wait(1)
out("hingeDetach joints=" .. tostring(#hingeA:GetJoints()))
hingeB.CFrame = CFrame.new(1600, 501, 500)
hingeA:MakeJoints()
task.wait(1)
out("hingeReattach class=" ..
    tostring(hingeA:GetJoints()[1] and hingeA:GetJoints()[1].ClassName))

local hingeOk, hingeErr = pcall(function() return Instance.new("Hinge") end)
out("hingeClass ok=" .. tostring(hingeOk) .. " refused=" ..
    tostring(not hingeOk and string.find(tostring(hingeErr),
        'Unable to create an Instance of type "Hinge"', 1, true) ~= nil))

out("write ok=" .. tostring(writeOk) .. " error=" ..
    tostring(writeErr) .. " after=" .. flags(wa, wb))

local enableOk, enableErr = pcall(function() wa.Enabled = false end)
out("disable ok=" .. tostring(enableOk) .. " error=" ..
    tostring(enableErr) .. " active=" .. flags(wa, wb))

out("done")
