local ws = game:GetService("Workspace")
ws.Gravity = 0
local shapes = { "Ball", "Block", "Cylinder", "Wedge", "CornerWedge" }
for _, name in ipairs(shapes) do
    local p = Instance.new("Part")
    p.Name = "CW" .. name
    p.Size = Vector3.new(4, 1, 2)
    p.Anchored = true
    p.CanCollide = false
    p.Shape = Enum.PartType[name]
    p.Parent = ws
    print("CW " .. name .. " shape=" .. tostring(p.Shape.Value) ..
          " mass=" .. tostring(p:GetMass()))
end
print("CW-DONE")
