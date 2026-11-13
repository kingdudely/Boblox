-- Does Model:MakeJoints perform a surface pass, and for which surfaces?
local ws = game:GetService("Workspace")
ws.Gravity = 0
local surfaces = {
    { 0, "Smooth" }, { 1, "Glue" }, { 2, "Weld" }, { 3, "Studs" },
    { 4, "Inlet" }, { 5, "Universal" }, { 6, "Hinge" }, { 7, "Motor" },
    { 8, "SteppingMotor" }, { 10, "SmoothNoOutlines" },
}
local model = Instance.new("Model")
model.Name = "ModelSurfaceMatrix"
model.Parent = ws
local rows = {}
local index = 0
for _, top in ipairs(surfaces) do
    for _, bottom in ipairs(surfaces) do
        index = index + 1
        local x = index * 8
        local a = Instance.new("Part")
        a.Name = "MM" .. top[1] .. "_" .. bottom[1] .. "_L"
        a.Size = Vector3.new(2, 1, 2)
        a.CFrame = CFrame.new(x, 500, 950)
        a.Anchored = false
        a.CanCollide = true
        a.TopSurface = Enum.SurfaceType[top[2]]
        a.Parent = model
        local b = Instance.new("Part")
        b.Name = "MM" .. top[1] .. "_" .. bottom[1] .. "_U"
        b.Size = Vector3.new(2, 1, 2)
        b.CFrame = CFrame.new(x, 501, 950)
        b.Anchored = false
        b.CanCollide = true
        b.BottomSurface = Enum.SurfaceType[bottom[2]]
        b.Parent = model
        rows[#rows + 1] = { top = top[1], bottom = bottom[1], a = a, b = b }
    end
end

model:MakeJoints()
task.wait(3)
for i = 1, 12 do print("MODEL-PAD") end
for _, row in ipairs(rows) do
    local j = row.a:GetJoints()[1] or row.b:GetJoints()[1]
    print("MODEL " .. row.top .. ":" .. row.bottom .. " class=" ..
          tostring(j and j.ClassName) .. " parentA=" ..
          tostring(j and j.Parent == row.a) .. " parentB=" ..
          tostring(j and j.Parent == row.b))
end
print("MODEL-DONE")
