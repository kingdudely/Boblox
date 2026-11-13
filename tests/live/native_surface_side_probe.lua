-- Side faces: A.RightSurface and B.LeftSurface touch at x.  All other faces
-- are Smooth.  This tests whether native MakeJoints considers side surfaces
-- at all and what it creates for each pair.
local ws = game:GetService("Workspace")
ws.Gravity = 0
local surfaces = {
    { 0, "Smooth" }, { 1, "Glue" }, { 2, "Weld" }, { 3, "Studs" },
    { 4, "Inlet" }, { 5, "Universal" }, { 6, "Hinge" }, { 7, "Motor" },
    { 8, "SteppingMotor" }, { 10, "SmoothNoOutlines" },
}
local folder = Instance.new("Folder")
folder.Name = "SurfaceSide"
folder.Parent = ws
local rows = {}
local index = 0
for _, right in ipairs(surfaces) do
    for _, left in ipairs(surfaces) do
        index = index + 1
        local x = index * 8
        local a = Instance.new("Part")
        a.Name = "S" .. right[1] .. "_" .. left[1] .. "_A"
        a.Size = Vector3.new(2, 1, 2)
        a.CFrame = CFrame.new(x, 500, 840)
        a.Anchored = false
        a.CanCollide = true
        a.RightSurface = Enum.SurfaceType[right[2]]
        a.Parent = folder
        local b = Instance.new("Part")
        b.Name = "S" .. right[1] .. "_" .. left[1] .. "_B"
        b.Size = Vector3.new(2, 1, 2)
        b.CFrame = CFrame.new(x + 2, 500, 840)
        b.Anchored = false
        b.CanCollide = true
        b.LeftSurface = Enum.SurfaceType[left[2]]
        b.Parent = folder
        rows[#rows + 1] = { right = right[1], left = left[1], a = a, b = b }
    end
end

local function label(part, row)
    if part == row.a then return "A" end
    if part == row.b then return "B" end
    if part == nil then return "nil" end
    return "X"
end
for _, row in ipairs(rows) do row.a:MakeJoints() end
task.wait(3)
for i = 1, 8 do print("SIDE-PAD") end
print("SIDE-BEGIN")
for _, row in ipairs(rows) do
    local found = {}
    local seen = {}
    local seenRows = {}
    for _, part in ipairs({ row.a, row.b }) do
        for _, j in ipairs(part:GetJoints()) do
            if not seen[j] then
                seen[j] = true
                local desc = j.ClassName .. "|" ..
                    label(j.Parent, row) .. "|" ..
                    label(j.Part0, row) .. "|" ..
                    label(j.Part1, row)
                if not seenRows[desc] then
                    seenRows[desc] = true
                    found[#found + 1] = desc
                end
            end
        end
    end
    table.sort(found)
    print("SIDE " .. row.right .. ":" .. row.left .. " n=" ..
          tostring(#found) .. " " .. table.concat(found, ";"))
end
print("SIDE-DONE")
