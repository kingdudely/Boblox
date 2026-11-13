-- Native Model surface-joint probe: does Model:MakeJoints run the same pass
-- as BasePart:MakeJoints, do the lower-case aliases exist, and does
-- Model:BreakJoints remove rigid joints while leaving Rotate?
local ws = game:GetService("Workspace")
ws.Gravity = 0

local model = Instance.new("Model")
model.Name = "ModelJointsNative"
model.Parent = ws

local function part(name, y, size)
    local p = Instance.new("Part")
    p.Name = name
    p.Size = size
    p.CFrame = CFrame.new(0, y, 700)
    p.Anchored = false
    p.CanCollide = true
    p.Parent = model
    return p
end

local a = part("ModelHingeA", 500, Vector3.new(2, 1, 2))
local b = part("ModelHingeB", 501, Vector3.new(2, 2, 2))
a.TopSurface = Enum.SurfaceType.Hinge
b.BottomSurface = Enum.SurfaceType.Smooth
model:MakeJoints()
task.wait(1)
local j = a:GetJoints()[1]
print("MODEL make class=" .. tostring(j and j.ClassName) ..
      " parent=" .. tostring(j and j.Parent == a))

local aliasOk, aliasErr = pcall(function() a:makeJoints() end)
print("MODEL aliasMake ok=" .. tostring(aliasOk) .. " err=" ..
      tostring(aliasErr) .. " joints=" .. tostring(#a:GetJoints()))

local rigidA = part("ModelWeldA", 510, Vector3.new(2, 1, 2))
local rigidB = part("ModelWeldB", 511, Vector3.new(2, 1, 2))
local w = Instance.new("Weld")
w.Part0 = rigidA
w.Part1 = rigidB
w.Parent = rigidA
print("MODEL weld before=" .. tostring(#rigidA:GetJoints()))
model:BreakJoints()
task.wait(0.5)
print("MODEL break rot=" .. tostring(#a:GetJoints()) ..
      " weld=" .. tostring(#rigidA:GetJoints()))

local lowerOk, lowerErr = pcall(function() a:breakJoints() end)
print("MODEL aliasBreak ok=" .. tostring(lowerOk) .. " err=" ..
      tostring(lowerErr))

-- BasePart aliases outside a Model, plus WeldConstraint handling.
local aliasModel = Instance.new("Model")
aliasModel.Name = "ModelAliasNative"
aliasModel.Parent = ws
local aa = Instance.new("Part")
aa.Name = "AliasHingeA"
aa.Size = Vector3.new(2, 1, 2)
aa.CFrame = CFrame.new(0, 520, 700)
aa.Anchored = false
aa.CanCollide = true
aa.Parent = ws
local ab = Instance.new("Part")
ab.Name = "AliasHingeB"
ab.Size = Vector3.new(2, 1, 2)
ab.CFrame = CFrame.new(0, 521, 700)
ab.Anchored = false
ab.CanCollide = true
ab.Parent = ws
aa.TopSurface = Enum.SurfaceType.Hinge
aa:makeJoints()
task.wait(1)
print("MODEL aliasSurface class=" ..
      tostring(aa:GetJoints()[1] and aa:GetJoints()[1].ClassName))
aa:MakeJoints()
task.wait(1)
print("MODEL capitalSurface class=" ..
      tostring(aa:GetJoints()[1] and aa:GetJoints()[1].ClassName))

local aliasWeldA = Instance.new("Part")
aliasWeldA.Name = "AliasWeldA"
aliasWeldA.Size = Vector3.new(2, 2, 2)
aliasWeldA.CFrame = CFrame.new(20, 520, 700)
aliasWeldA.Parent = ws
local aliasWeldB = Instance.new("Part")
aliasWeldB.Name = "AliasWeldB"
aliasWeldB.Size = Vector3.new(2, 2, 2)
aliasWeldB.CFrame = CFrame.new(22, 520, 700)
aliasWeldB.Parent = ws
local aliasWeld = Instance.new("Weld")
aliasWeld.Part0 = aliasWeldA
aliasWeld.Part1 = aliasWeldB
aliasWeld.Parent = aliasWeldA
print("MODEL aliasWeld before=" .. tostring(#aliasWeldA:GetJoints()))
aliasWeldA:breakJoints()
task.wait(0.5)
print("MODEL aliasWeld after=" .. tostring(#aliasWeldA:GetJoints()))

local overlapA = Instance.new("Part")
overlapA.Name = "OverlapHingeA"
overlapA.Size = Vector3.new(2, 1, 2)
overlapA.CFrame = CFrame.new(40, 540, 700)
overlapA.Anchored = false
overlapA.CanCollide = true
overlapA.Parent = ws
local overlapB = Instance.new("Part")
overlapB.Name = "OverlapHingeB"
overlapB.Size = Vector3.new(2, 2, 2)
overlapB.CFrame = CFrame.new(40, 541, 700)
overlapB.Anchored = false
overlapB.CanCollide = true
overlapB.Parent = ws
overlapA.TopSurface = Enum.SurfaceType.Hinge
overlapA:MakeJoints()
task.wait(1)
print("MODEL overlap class=" ..
      tostring(overlapA:GetJoints()[1] and overlapA:GetJoints()[1].ClassName))

local gapA = Instance.new("Part")
gapA.Name = "GapHingeA"
gapA.Size = Vector3.new(2, 1, 2)
gapA.CFrame = CFrame.new(60, 550, 700)
gapA.Anchored = false
gapA.CanCollide = true
gapA.Parent = ws
local gapB = Instance.new("Part")
gapB.Name = "GapHingeB"
gapB.Size = Vector3.new(2, 2, 2)
gapB.CFrame = CFrame.new(60, 551.55, 700)
gapB.Anchored = false
gapB.CanCollide = true
gapB.Parent = ws
gapA.TopSurface = Enum.SurfaceType.Hinge
gapA:MakeJoints()
task.wait(1)
print("MODEL gap class=" ..
      tostring(gapA:GetJoints()[1] and gapA:GetJoints()[1].ClassName))

local wcA = part("ModelWCA", 530, Vector3.new(2, 2, 2))
local wcB = part("ModelWCB", 532, Vector3.new(2, 2, 2))
local wc = Instance.new("WeldConstraint")
wc.Part0 = wcA
wc.Part1 = wcB
wc.Parent = wcA
print("MODEL wc before=" .. tostring(#wcA:GetJoints()))
model:BreakJoints()
task.wait(0.5)
print("MODEL wc after=" .. tostring(#wcA:GetJoints()))

print("MODEL-DONE")
