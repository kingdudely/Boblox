-- Native Motor/SteppingMotor surface pass: the class, owner and frames.
local ws = game:GetService("Workspace")
ws.Gravity = 0

local function cf(c)
    local t = {c:GetComponents()}
    for i = 1, 12 do
        if math.abs(t[i]) < 1e-6 then t[i] = 0 end
        t[i] = string.format("%.3f", t[i])
    end
    return table.concat(t, ",")
end

local function run(label, top, bottom)
    local a = Instance.new("Part")
    a.Name = label .. "A"
    a.Size = Vector3.new(2, 1, 2)
    a.CFrame = CFrame.new(0, 500, 800)
    a.Anchored = false
    a.CanCollide = true
    a.TopSurface = Enum.SurfaceType[top]
    a.Parent = ws
    local b = Instance.new("Part")
    b.Name = label .. "B"
    b.Size = Vector3.new(2, 1, 2)
    b.CFrame = CFrame.new(0, 501, 800)
    b.Anchored = false
    b.CanCollide = true
    b.BottomSurface = Enum.SurfaceType[bottom]
    b.Parent = ws
    a:MakeJoints()
    task.wait(1)
    local j = a:GetJoints()[1]
    if not j then
        print("DYN " .. label .. " class=nil")
    else
        print("DYN " .. label .. " class=" .. j.ClassName .. " name=" .. j.Name ..
              " parentA=" .. tostring(j.Parent == a) ..
              " parentB=" .. tostring(j.Parent == b) ..
              " p0A=" .. tostring(j.Part0 == a) ..
              " p1B=" .. tostring(j.Part1 == b) ..
              " c0=" .. cf(j.C0) .. " c1=" .. cf(j.C1) ..
              " enabled=" .. tostring(j.Enabled) ..
              " active=" .. tostring(j.Active) ..
              " connA=" .. tostring(#a:GetConnectedParts(true)) ..
              " connB=" .. tostring(#b:GetConnectedParts(true)) ..
              " rootA=" .. tostring(a:GetRootPart() == a) ..
              " rootB=" .. tostring(b:GetRootPart() == b))
    end
    a:Destroy()
    b:Destroy()
end

run("MotorHinge", "Motor", "Hinge")
run("StepHinge", "SteppingMotor", "Hinge")
run("MotorSmooth", "Motor", "Smooth")
run("StepSmooth", "SteppingMotor", "Smooth")
print("DYN-DONE")
