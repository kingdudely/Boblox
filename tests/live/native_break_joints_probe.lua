local ws = game:GetService("Workspace")
ws.Gravity = 0
local function out(s) print("BRK " .. s) rconsoleprint("BRK " .. s .. "\n") end
local function part(name, pos)
    local p = Instance.new("Part")
    p.Name = name
    p.Size = Vector3.new(4, 1, 4)
    p.CFrame = CFrame.new(pos)
    p.Anchored = false
    p.CanCollide = true
    p.Parent = ws
    return p
end
local function names(p)
    local bits = {}
    local ok, joints = pcall(function() return p:GetJoints() end)
    if ok then
        for _, j in ipairs(joints) do
            bits[#bits + 1] = j.ClassName .. "/" .. j.Name
        end
    end
    return table.concat(bits, ",")
end
local function case(label, top, bottom)
    local a = part("Brk" .. label .. "A", Vector3.new(500, 500, 800))
    local b = part("Brk" .. label .. "B", Vector3.new(500, 501, 800))
    a.TopSurface = top
    b.BottomSurface = bottom
    task.wait(0.4)
    a:MakeJoints()
    task.wait(0.4)
    out(label .. " before=" .. names(a) .. "|" .. names(b))
    a:BreakJoints()
    task.wait(0.4)
    out(label .. " after=" .. names(a) .. "|" .. names(b))
    a.Parent = nil
    b.Parent = nil
end
case("Hinge", Enum.SurfaceType.Hinge, Enum.SurfaceType.Smooth)
case("Weld", Enum.SurfaceType.Weld, Enum.SurfaceType.Smooth)
case("Glue", Enum.SurfaceType.Glue, Enum.SurfaceType.Smooth)

local m = part("BrkManualA", Vector3.new(520, 500, 800))
local n = part("BrkManualB", Vector3.new(520, 501, 800))
local w = Instance.new("Weld")
w.Name = "ManualW"
w.Part0 = m
w.Part1 = n
w.Parent = m
task.wait(0.3)
out("Manual before=" .. names(m))
m:BreakJoints()
task.wait(0.4)
out("Manual after=" .. names(m))
out("done")
