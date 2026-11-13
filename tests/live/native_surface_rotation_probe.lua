-- Native surface-Hinge probe for callers rotated away from +Y.
-- Establishes whether native still creates the Rotate edge when the calling
-- part's TopSurface normal is not world-up.  Prints one ROT line per case.
local ws = game:GetService("Workspace")
ws.Gravity = 0

local folder = Instance.new("Folder")
folder.Name = "SurfaceRotationNative"
folder.Parent = ws

local function part(name, cf, size)
    local p = Instance.new("Part")
    p.Name = name
    p.Size = size
    p.CFrame = cf
    p.Anchored = false
    p.CanCollide = true
    p.Parent = folder
    return p
end

local function run(name, acf, bcf)
    local a = part(name .. "A", acf, Vector3.new(2, 1, 2))
    local b = part(name .. "B", bcf, Vector3.new(2, 2, 2))
    a.TopSurface = Enum.SurfaceType.Hinge
    b.BottomSurface = Enum.SurfaceType.Smooth
    a:MakeJoints()
    task.wait(1)
    local j = a:GetJoints()[1]
    local c0 = "nil"
    if j then
        local got = {j.C0:GetComponents()}
        for i = 1, 12 do
            if math.abs(got[i]) < 1e-6 then got[i] = 0 end
            got[i] = string.format("%.3f", got[i])
        end
        c0 = table.concat(got, ",")
    end
    print("ROT " .. name .. " class=" .. tostring(j and j.ClassName) ..
          " c0=" .. c0)
    a:Destroy()
    b:Destroy()
end

run("Z90", CFrame.new(0, 500, 0) * CFrame.Angles(0, 0, math.rad(90)),
    CFrame.new(-1.5, 500, 0))
run("X90", CFrame.new(10, 500, 0) * CFrame.Angles(math.rad(90), 0, 0),
    CFrame.new(10, 500, 1.5))
run("Z180", CFrame.new(20, 500, 0) * CFrame.Angles(0, 0, math.rad(180)),
    CFrame.new(20, 498.5, 0))

print("ROT-DONE")
