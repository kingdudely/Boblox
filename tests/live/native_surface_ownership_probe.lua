-- Native surface-joint ownership: caller A has no Hinge, other B has Bottom
-- Hinge.  Print the joint's owner and Part0/Part1 after A:MakeJoints().
local ws = game:GetService("Workspace")
ws.Gravity = 0

local function describe(tag, a, b)
    local seen = {}
    for _, p in ipairs({ a, b }) do
        for _, j in ipairs(p:GetJoints()) do
            if not seen[j] then
                seen[j] = true
                print("OWN " .. tag .. " class=" .. j.ClassName ..
                      " parentA=" .. tostring(j.Parent == a) ..
                      " parentB=" .. tostring(j.Parent == b) ..
                      " p0A=" .. tostring(j.Part0 == a) ..
                      " p1B=" .. tostring(j.Part1 == b))
            end
        end
    end
end

local function run(tag, top)
    local a = Instance.new("Part")
    a.Name = tag .. "A"
    a.Size = Vector3.new(2, 1, 2)
    a.CFrame = CFrame.new(0, 500, 900)
    a.Anchored = false
    a.CanCollide = true
    a.TopSurface = Enum.SurfaceType[top]
    a.Parent = ws
    local b = Instance.new("Part")
    b.Name = tag .. "B"
    b.Size = Vector3.new(2, 1, 2)
    b.CFrame = CFrame.new(0, 501, 900)
    b.Anchored = false
    b.CanCollide = true
    b.BottomSurface = Enum.SurfaceType.Hinge
    b.Parent = ws
    a:MakeJoints()
    task.wait(1)
    describe(tag, a, b)
    a:Destroy()
    b:Destroy()
end

run("SmoothOther", "Smooth")
run("WeldOther", "Weld")
run("MotorOther", "Motor")
print("OWN-DONE")
