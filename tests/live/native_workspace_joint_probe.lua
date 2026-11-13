-- Native Workspace:MakeJoints / BreakJoints entry points.  The Hinge probe
-- proved both reject nil/userdata with reflection's table-argument error;
-- this probe establishes what a real table of parts does.
local ws = game:GetService("Workspace")
ws.Gravity = 0

local function part(name, x, y)
    local p = Instance.new("Part")
    p.Name = name
    p.Size = Vector3.new(2, 1, 2)
    p.CFrame = CFrame.new(x, y, 700)
    p.Anchored = false
    p.CanCollide = true
    p.Parent = ws
    return p
end

local function class(p)
    local j = p:GetJoints()[1]
    return tostring(j and j.ClassName)
end

local w = part("WorkspaceHingeA", 0, 500)
local z = part("WorkspaceHingeB", 0, 501)
w.TopSurface = Enum.SurfaceType.Hinge
z.BottomSurface = Enum.SurfaceType.Smooth

local ok, err = pcall(function() return ws:MakeJoints(nil) end)
print("WS nil ok=" .. tostring(ok) .. " err=" .. tostring(err))
ok, err = pcall(function() return ws:MakeJoints(w) end)
print("WS userdata ok=" .. tostring(ok) .. " err=" .. tostring(err))

ok, err = pcall(function() return ws:MakeJoints({ w }) end)
task.wait(1)
print("WS tableOne ok=" .. tostring(ok) .. " err=" .. tostring(err) ..
      " a=" .. class(w) .. " b=" .. class(z))

ok, err = pcall(function() return ws:MakeJoints({ w, z }) end)
task.wait(1)
print("WS tableTwo ok=" .. tostring(ok) .. " err=" .. tostring(err) ..
      " a=" .. class(w) .. " b=" .. class(z))

ok, err = pcall(function() return ws:BreakJoints({ w }) end)
task.wait(0.5)
print("WS breakOne ok=" .. tostring(ok) .. " err=" .. tostring(err) ..
      " a=" .. class(w) .. " b=" .. class(z))

ok, err = pcall(function() return ws:BreakJoints({ w, z }) end)
task.wait(0.5)
print("WS breakTwo ok=" .. tostring(ok) .. " err=" .. tostring(err) ..
      " a=" .. class(w) .. " b=" .. class(z))

ok, err = pcall(function() return ws:BreakJoints(nil) end)
print("WS breakNil ok=" .. tostring(ok) .. " err=" .. tostring(err))

print("WS-DONE")
