-- Surface pair matrix, with one pair per caller order.  A lower TopSurface
-- faces an upper BottomSurface; A pairs call lower first, B pairs call upper
-- first.  Compact output exposes class, owner and Part0/Part1.
local ws = game:GetService("Workspace")
ws.Gravity = 0
local surfaces = {
    { 0, "Smooth" }, { 1, "Glue" }, { 2, "Weld" }, { 3, "Studs" },
    { 4, "Inlet" }, { 5, "Universal" }, { 6, "Hinge" }, { 7, "Motor" },
    { 8, "SteppingMotor" }, { 10, "SmoothNoOutlines" },
}
local folder = Instance.new("Folder")
folder.Name = "SurfaceOrderAll"
folder.Parent = ws
local rows = {}
local index = 0
for _, top in ipairs(surfaces) do
    for _, bottom in ipairs(surfaces) do
        index = index + 1
        local function makePair(prefix, x)
            local lower = Instance.new("Part")
            lower.Name = prefix .. "L"
            lower.Size = Vector3.new(2, 1, 2)
            lower.CFrame = CFrame.new(x, 500, 820)
            lower.Anchored = false
            lower.CanCollide = true
            lower.TopSurface = Enum.SurfaceType[top[2]]
            lower.Parent = folder
            local upper = Instance.new("Part")
            upper.Name = prefix .. "U"
            upper.Size = Vector3.new(2, 1, 2)
            upper.CFrame = CFrame.new(x, 501, 820)
            upper.Anchored = false
            upper.CanCollide = true
            upper.BottomSurface = Enum.SurfaceType[bottom[2]]
            upper.Parent = folder
            return { lower = lower, upper = upper }
        end
        rows[#rows + 1] = {
            top = top[1], bottom = bottom[1], lowerFirst = true,
            pair = makePair("L" .. top[1] .. "_" .. bottom[1] .. "_",
                            index * 8),
        }
        rows[#rows + 1] = {
            top = top[1], bottom = bottom[1], lowerFirst = false,
            pair = makePair("U" .. top[1] .. "_" .. bottom[1] .. "_",
                            index * 8 + 4),
        }
    end
end

local function label(part, pair)
    if part == pair.lower then return "L" end
    if part == pair.upper then return "U" end
    if part == nil then return "nil" end
    return "X"
end

local function describe(row)
    -- Space the output: native drops leading lines of a dense burst.
    task.wait(0.1)
    local pair = row.pair
    local found = {}
    local seen = {}
    local seenRows = {}
    for _, part in ipairs({ pair.lower, pair.upper }) do
        for _, j in ipairs(part:GetJoints()) do
            if not seen[j] then
                seen[j] = true
                local row = j.ClassName .. "|" ..
                    label(j.Parent, pair) .. "|" ..
                    label(j.Part0, pair) .. "|" ..
                    label(j.Part1, pair)
                if not seenRows[row] then
                    seenRows[row] = true
                    found[#found + 1] = row
                end
            end
        end
    end
    table.sort(found)
    print("ORDER " .. (row.lowerFirst and "L" or "U") .. " " ..
          row.top .. ":" .. row.bottom .. " n=" .. tostring(#found) ..
          " " .. table.concat(found, ";"))
end

for _, row in ipairs(rows) do
    if row.lowerFirst then
        row.pair.lower:MakeJoints()
    else
        row.pair.upper:MakeJoints()
    end
end
task.wait(6)
print("ORDER-BEGIN")
for _, row in ipairs(rows) do describe(row) end
print("ORDER-DONE")
