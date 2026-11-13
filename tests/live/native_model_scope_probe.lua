-- What does Model:MakeJoints scan: the model's subtree, or the workspace?
local ws = game:GetService("Workspace")
ws.Gravity = 0
local function part(name, x, y, z, parent)
    local p = Instance.new("Part")
    p.Name = name
    p.Size = Vector3.new(2, 1, 2)
    p.CFrame = CFrame.new(x, y, z)
    p.Anchored = false
    p.CanCollide = true
    p.Parent = parent
    return p
end

-- A model part touching an outside workspace part.
local model = Instance.new("Model")
model.Name = "ScopeModel"
model.Parent = ws
local inside = part("InsideA", 0, 500, 1000, model)
inside.TopSurface = Enum.SurfaceType.Weld
local outside = part("OutsideB", 0, 501, 1000, ws)
outside.BottomSurface = Enum.SurfaceType.Smooth

-- A nested model part touching the model part.
local nested = Instance.new("Model")
nested.Name = "Nested"
nested.Parent = model
local nestedPart = part("NestedC", 10, 501, 1000, nested)
nestedPart.BottomSurface = Enum.SurfaceType.Glue
local inside2 = part("InsideD", 10, 500, 1000, model)
inside2.TopSurface = Enum.SurfaceType.Smooth

model:MakeJoints()
task.wait(2)
local function joint(a, b)
    for _, j in ipairs(a:GetJoints()) do
        if (j.Part0 == a and j.Part1 == b) or (j.Part0 == b and j.Part1 == a) then
            return j.ClassName .. "/" .. tostring(j.Parent == a)
        end
    end
    return "nil"
end
print("SCOPE outside=" .. joint(inside, outside) ..
      " nested=" .. joint(inside2, nestedPart))
print("SCOPE-DONE")
