-- BreakJoints matrix for surface-generated Weld, Glue, Snap, Rotate,
-- RotateV and RotateP.  Every path prints the joint set before and after.
local ws = game:GetService("Workspace")
ws.Gravity = 0
local folder = Instance.new("Folder")
folder.Name = "SurfaceBreakAll"
folder.Parent = ws
local seq = 0

local function names(part)
    if not part then return "nil" end
    local bits = {}
    for _, j in ipairs(part:GetJoints()) do
        bits[#bits + 1] = tostring(j.ClassName) .. "/" .. tostring(j.Name)
    end
    table.sort(bits)
    return table.concat(bits, ",")
end

local function makePair(top, bottom)
    seq = seq + 1
    local x = seq * 8
    local a = Instance.new("Part")
    a.Name = "B" .. seq .. "L"
    a.Size = Vector3.new(2, 1, 2)
    a.CFrame = CFrame.new(x, 500, 900)
    a.Anchored = false
    a.CanCollide = true
    a.TopSurface = Enum.SurfaceType[top]
    a.Parent = folder
    local b = Instance.new("Part")
    b.Name = "B" .. seq .. "U"
    b.Size = Vector3.new(2, 1, 2)
    b.CFrame = CFrame.new(x, 501, 900)
    b.Anchored = false
    b.CanCollide = true
    b.BottomSurface = Enum.SurfaceType[bottom]
    b.Parent = folder
    return a, b
end

local function waitJoint(a, b)
    task.wait(0.5)
    return a:GetJoints()[1] or b:GetJoints()[1]
end

local function show(label, a, b)
    print("BRK " .. label .. " a=" .. names(a) .. " b=" .. names(b))
end

local cases = {
    { "Weld", "Weld", "Smooth" },
    { "Glue", "Glue", "Smooth" },
    { "Snap", "Studs", "Inlet" },
    { "Hinge", "Hinge", "Smooth" },
    { "RotateV", "Motor", "Smooth" },
    { "RotateP", "SteppingMotor", "Smooth" },
}

-- BasePart on the joint owner and on the opposite endpoint.
for _, c in ipairs(cases) do
    local a, b = makePair(c[2], c[3])
    a:MakeJoints()
    local j = waitJoint(a, b)
    show(c[1] .. " baseBefore", a, b)
    a:BreakJoints()
    task.wait(0.5)
    show(c[1] .. " baseAfterOwner", a, b)

    local d, e = makePair(c[2], c[3])
    e:MakeJoints()
    local j2 = waitJoint(d, e)
    show(c[1] .. " baseBeforeOther", d, e)
    d:BreakJoints()
    task.wait(0.5)
    show(c[1] .. " baseAfterOther", d, e)
end

-- Model:MakeJoints runs the BasePart surface pass for its subtree;
-- Model:BreakJoints clears every created joint.
local model = Instance.new("Model")
model.Name = "SurfaceBreakModel"
model.Parent = ws
local ma, mb = makePair("Weld", "Smooth")
ma.Parent = model
mb.Parent = model
model:MakeJoints()
task.wait(0.5)
show("modelMakeWeld", ma, mb)
ma:MakeJoints()
waitJoint(ma, mb)
show("modelBefore", ma, mb)
model:BreakJoints()
task.wait(0.5)
show("modelAfter", ma, mb)

local aliasModel = Instance.new("Model")
aliasModel.Name = "SurfaceBreakModelAlias"
aliasModel.Parent = ws
local aa, ab = makePair("Weld", "Smooth")
aa.Parent = aliasModel
ab.Parent = aliasModel
aa:MakeJoints()
waitJoint(aa, ab)
show("modelAliasBefore", aa, ab)
aliasModel:breakJoints()
task.wait(0.5)
show("modelAliasAfter", aa, ab)

-- Workspace table paths.
for _, c in ipairs(cases) do
    local a, b = makePair(c[2], c[3])
    local ok, err = pcall(function() ws:MakeJoints({ a }) end)
    waitJoint(a, b)
    show(c[1] .. " wsMake ok=" .. tostring(ok) .. " err=" .. tostring(err),
         a, b)
    local bok, berr = pcall(function() ws:BreakJoints({ a }) end)
    task.wait(0.5)
    show(c[1] .. " wsBreak ok=" .. tostring(bok) .. " err=" .. tostring(berr),
         a, b)
end

local wa, wb = makePair("Weld", "Smooth")
wa:MakeJoints()
waitJoint(wa, wb)
show("wsBreakOtherBefore", wa, wb)
ws:BreakJoints({ wb })
task.wait(0.5)
show("wsBreakOtherAfter", wa, wb)

for _, name in ipairs({ "makeJoints", "breakJoints" }) do
    local ok, err = pcall(function() return ws[name](ws, {}) end)
    print("BRK wsAlias " .. name .. " ok=" .. tostring(ok) ..
          " err=" .. tostring(err))
end

for _, name in ipairs({ "makeJoints", "breakJoints" }) do
    local ok, err = pcall(function() return model[name](model, {}) end)
    print("BRK modelAlias " .. name .. " ok=" .. tostring(ok) ..
          " err=" .. tostring(err))
end

print("BRK-DONE")
