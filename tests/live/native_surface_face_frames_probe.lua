-- Native face frames and side ownership.  Each case places A and B so the
-- named A face touches the named B face; A calls MakeJoints.
local ws = game:GetService("Workspace")
ws.Gravity = 0
local folder = Instance.new("Folder")
folder.Name = "SurfaceFaceFrames"
folder.Parent = ws

local function cfString(c)
    if not c then return "nil" end
    local t = { c:GetComponents() }
    for i = 1, 12 do
        if math.abs(t[i]) < 1e-6 then t[i] = 0 end
        t[i] = string.format("%.3f", t[i])
    end
    return table.concat(t, ",")
end
local function label(part, a, b)
    if part == a then return "A" end
    if part == b then return "B" end
    if part == nil then return "nil" end
    return "X"
end
local function run(tag, a, b, owner)
    (owner or a):MakeJoints()
    task.wait(0.3)
    local j = a:GetJoints()[1] or b:GetJoints()[1]
    if not j then
        print("FACE " .. tag .. " class=nil")
        return
    end
    print("FACE " .. tag .. " class=" .. tostring(j.ClassName) ..
          " name=" .. tostring(j.Name) ..
          " parent=" .. label(j.Parent, a, b) ..
          " p0=" .. label(j.Part0, a, b) ..
          " p1=" .. label(j.Part1, a, b) ..
          " c0=" .. cfString(j.C0) .. " c1=" .. cfString(j.C1))
end
local index = 0
local function pair(tag, ax, az, bx, bz, aface, bface, asurf, bsurf, owner)
    owner = owner or "a"
    index = index + 1
    local y = 500 + index * 2
    local a = Instance.new("Part")
    a.Name = tag .. "A"
    a.Size = Vector3.new(2, 2, 2)
    a.CFrame = CFrame.new(ax, y, az)
    a.Anchored = false
    a.CanCollide = true
    if aface then a[aface] = Enum.SurfaceType[asurf] end
    a.Parent = folder
    local b = Instance.new("Part")
    b.Name = tag .. "B"
    b.Size = Vector3.new(2, 2, 2)
    b.CFrame = CFrame.new(bx, y, bz)
    b.Anchored = false
    b.CanCollide = true
    if bface then b[bface] = Enum.SurfaceType[bsurf] end
    b.Parent = folder
    run(tag, a, b, owner == "b" and b or a)
end

pair("RightWeld", 0, 0, 2, 0, "RightSurface", "LeftSurface", "Weld", "Weld")
pair("RightWeldOther", 20, 0, 22, 0, "RightSurface", "LeftSurface",
     "Smooth", "Weld")
pair("RightGlue", 40, 0, 42, 0, "RightSurface", "LeftSurface", "Glue", "Glue")
pair("RightSnap", 60, 0, 62, 0, "RightSurface", "LeftSurface", "Studs", "Inlet")
pair("RightHinge", 80, 0, 82, 0, "RightSurface", "LeftSurface", "Hinge",
     "Smooth")
pair("RightHingeOther", 100, 0, 102, 0, "RightSurface", "LeftSurface",
     "Smooth", "Hinge")
pair("LeftWeld", 122, 0, 120, 0, "LeftSurface", "RightSurface", "Weld", "Weld")
pair("LeftHinge", 142, 0, 140, 0, "LeftSurface", "RightSurface", "Hinge",
     "Smooth")
pair("FrontWeld", 160, 0, 160, -2, "FrontSurface", "BackSurface", "Weld", "Weld")
pair("FrontHinge", 180, 0, 180, -2, "FrontSurface", "BackSurface", "Hinge",
     "Smooth")
pair("BackWeld", 200, 0, 200, 2, "BackSurface", "FrontSurface", "Weld", "Weld")
pair("BackHinge", 220, 0, 220, 2, "BackSurface", "FrontSurface", "Hinge",
     "Smooth")
pair("RightWeldUpper", 240, 0, 242, 0, "RightSurface", "LeftSurface",
     "Smooth", "Weld", "b")
pair("RightHingeUpper", 260, 0, 262, 0, "RightSurface", "LeftSurface",
     "Smooth", "Hinge", "b")
pair("LeftHingeOther", 280, 0, 278, 0, "LeftSurface", "RightSurface",
     "Smooth", "Hinge")
print("FACE-DONE")
