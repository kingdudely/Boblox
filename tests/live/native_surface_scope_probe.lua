-- Does one part's MakeJoints assemble every surface pair in the Workspace,
-- or only parts touching the receiver?
local ws = game:GetService("Workspace")
ws.Gravity = 0

local function pair(name, x)
    local a = Instance.new("Part")
    a.Name = name .. "A"
    a.Size = Vector3.new(2, 1, 2)
    a.CFrame = CFrame.new(x, 500, 950)
    a.Anchored = false
    a.CanCollide = true
    a.TopSurface = Enum.SurfaceType.Hinge
    a.Parent = ws
    local b = Instance.new("Part")
    b.Name = name .. "B"
    b.Size = Vector3.new(2, 1, 2)
    b.CFrame = CFrame.new(x, 501, 950)
    b.Anchored = false
    b.CanCollide = true
    b.BottomSurface = Enum.SurfaceType.Smooth
    b.Parent = ws
    return a, b
end

local a1, b1 = pair("ScopeOne", 0)
local a2, b2 = pair("ScopeTwo", 20)

a1:MakeJoints()
task.wait(1)
print("SCOPE first=" .. tostring(a1:GetJoints()[1] and
      a1:GetJoints()[1].ClassName) .. " second=" ..
      tostring(a2:GetJoints()[1] and a2:GetJoints()[1].ClassName))

a2:MakeJoints()
task.wait(1)
print("SCOPE secondCall=" .. tostring(a2:GetJoints()[1] and
      a2:GetJoints()[1].ClassName))

-- Ownership of the other Hinge case without calling the Hinge owner.
local c = Instance.new("Part")
c.Name = "ScopeSmooth"
c.Size = Vector3.new(2, 1, 2)
c.CFrame = CFrame.new(40, 500, 950)
c.Anchored = false
c.CanCollide = true
c.TopSurface = Enum.SurfaceType.Smooth
c.Parent = ws
local d = Instance.new("Part")
d.Name = "ScopeHinge"
d.Size = Vector3.new(2, 1, 2)
d.CFrame = CFrame.new(40, 501, 950)
d.Anchored = false
d.CanCollide = true
d.BottomSurface = Enum.SurfaceType.Hinge
d.Parent = ws
c:MakeJoints()
task.wait(1)
local j = d:GetJoints()[1]
print("SCOPE otherHinge=" .. tostring(j and j.ClassName) ..
      " parentD=" .. tostring(j and j.Parent == d) ..
      " p0D=" .. tostring(j and j.Part0 == d) ..
      " p1C=" .. tostring(j and j.Part1 == c))

print("SCOPE-DONE")
