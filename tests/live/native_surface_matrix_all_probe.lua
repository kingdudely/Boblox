-- Complete native surface-joint matrix.  Each pair is a lower part with
-- TopSurface=<top> and an upper part with BottomSurface=<bottom>.  First all
-- lowers call MakeJoints(), then all uppers call it; the descriptor is
-- printed after each phase so both entry points are covered.
local ws = game:GetService("Workspace")
ws.Gravity = 0

local surfaces = {
    { 0, "Smooth" },
    { 1, "Glue" },
    { 2, "Weld" },
    { 3, "Studs" },
    { 4, "Inlet" },
    { 5, "Universal" },
    { 6, "Hinge" },
    { 7, "Motor" },
    { 8, "SteppingMotor" },
    { 10, "SmoothNoOutlines" },
}

local folder = Instance.new("Folder")
folder.Name = "SurfaceMatrixAll"
folder.Parent = ws

local pairsList = {}
local index = 0
for _, top in ipairs(surfaces) do
    for _, bottom in ipairs(surfaces) do
        index = index + 1
        local x = index * 8
        local lower = Instance.new("Part")
        lower.Name = "M" .. top[1] .. "_" .. bottom[1] .. "_L"
        lower.Size = Vector3.new(2, 1, 2)
        lower.CFrame = CFrame.new(x, 500, 800)
        lower.Anchored = false
        lower.CanCollide = true
        lower.TopSurface = Enum.SurfaceType[top[2]]
        lower.Parent = folder

        local upper = Instance.new("Part")
        upper.Name = "M" .. top[1] .. "_" .. bottom[1] .. "_U"
        upper.Size = Vector3.new(2, 1, 2)
        upper.CFrame = CFrame.new(x, 501, 800)
        upper.Anchored = false
        upper.CanCollide = true
        upper.BottomSurface = Enum.SurfaceType[bottom[2]]
        upper.Parent = folder
        pairsList[#pairsList + 1] = { top = top[1], bottom = bottom[1],
                                       lower = lower, upper = upper }
    end
end

local function cfString(c)
    if not c then return "nil" end
    local t = { c:GetComponents() }
    for i = 1, 12 do
        if math.abs(t[i]) < 1e-6 then t[i] = 0 end
        t[i] = string.format("%.3f", t[i])
    end
    return table.concat(t, ",")
end

local function describe(pair, phase)
    -- Space the output: native drops leading lines of a dense burst.
    task.wait(0.1)
    local lower, upper = pair.lower, pair.upper
    local seen = {}
    local seenRows = {}
    local rows = {}
    local function label(part)
        if part == lower then return "L" end
        if part == upper then return "U" end
        if part == nil then return "nil" end
        return "X"
    end
    for _, part in ipairs({ lower, upper }) do
        for _, j in ipairs(part:GetJoints()) do
            if not seen[j] then
                seen[j] = true
                local row = "class=" .. tostring(j.ClassName) ..
                    ",name=" .. tostring(j.Name) ..
                    ",parent=" .. label(j.Parent) ..
                    ",p0=" .. label(j.Part0) ..
                    ",p1=" .. label(j.Part1) ..
                    ",c0=" .. cfString(j.C0) ..
                    ",c1=" .. cfString(j.C1) ..
                    ",enabled=" .. tostring(j.Enabled) ..
                    ",active=" .. tostring(j.Active)
                if not seenRows[row] then
                    seenRows[row] = true
                    rows[#rows + 1] = row
                end
            end
        end
    end
    table.sort(rows)
    print("SURF " .. phase .. " " .. pair.top .. ":" .. pair.bottom ..
          " count=" .. tostring(#rows) ..
          " connL=" .. tostring(#lower:GetConnectedParts(true)) ..
          " connU=" .. tostring(#upper:GetConnectedParts(true)) ..
          " rootL=" .. tostring(lower:GetRootPart() == lower) ..
          " rootU=" .. tostring(upper:GetRootPart() == upper) ..
          " " .. table.concat(rows, " | "))
end

for _, pair in ipairs(pairsList) do
    pair.lower:MakeJoints()
end
task.wait(6)
print("SURF-BEGIN")
for _, pair in ipairs(pairsList) do
    describe(pair, "L")
end

for _, pair in ipairs(pairsList) do
    pair.upper:MakeJoints()
end
task.wait(6)
for _, pair in ipairs(pairsList) do
    describe(pair, "U")
end

print("SURF-DONE")
