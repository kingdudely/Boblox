-- native_root_reverse_probe.lua — active-joint order in a two-anchor split.
local ws = game:GetService("Workspace")

local function out(s)
    rconsoleprint("ROOTREV " .. s .. "\n")
    print("ROOTREV " .. s)
end

local folder = Instance.new("Folder")
folder.Name = "RootReverseNative"
folder.Parent = ws

local function part(name, pos, size, anchored)
    local p = Instance.new("Part")
    p.Name = name
    p.Size = size or Vector3.new(2, 2, 2)
    p.CFrame = CFrame.new(pos or Vector3.new(0, 500, 0))
    p.Anchored = anchored == true
    p.CanCollide = false
    p.Parent = folder
    return p
end

local function weld(name, a, b)
    local j = Instance.new("Weld")
    j.Name = name
    j.Part0 = a
    j.Part1 = b
    j.Parent = folder
    return j
end

local function rootName(p)
    local ok, root = pcall(function() return p:GetRootPart() end)
    if not ok then return "ERR" end
    return root and root.Name or "nil"
end

local function report(tag, p)
    out(string.format("%s part=%s root=%s anchored=%s", tag, p.Name,
                      rootName(p), tostring(p.Anchored)))
end

-- Original order: weld A-middle older, weld middle-B newer.
local a1 = part("RootRev_A1", Vector3.new(1000, 500, 500),
                Vector3.new(1, 1, 1), true)
local m1 = part("RootRev_M1", Vector3.new(1002, 500, 500),
                Vector3.new(2, 2, 2), false)
local b1 = part("RootRev_B1", Vector3.new(1004, 500, 500),
                Vector3.new(12, 12, 12), true)
local wa1 = weld("RootRev_WA1", a1, m1)
local wb1 = weld("RootRev_WB1", m1, b1)

-- Reverse order: weld middle-B older, weld A-middle newer.
local a2 = part("RootRev_A2", Vector3.new(1010, 500, 500),
                Vector3.new(1, 1, 1), true)
local m2 = part("RootRev_M2", Vector3.new(1012, 500, 500),
                Vector3.new(2, 2, 2), false)
local b2 = part("RootRev_B2", Vector3.new(1014, 500, 500),
                Vector3.new(12, 12, 12), true)
local wb2 = weld("RootRev_WB2", m2, b2)
local wa2 = weld("RootRev_WA2", a2, m2)

task.wait(1.5)

report("ORD1-A", a1)
report("ORD1-M", m1)
report("ORD1-B", b1)
out("ORD1 active wa=" .. tostring(wa1.Active) .. " wb=" .. tostring(wb1.Active))
report("ORD2-A", a2)
report("ORD2-M", m2)
report("ORD2-B", b2)
out("ORD2 active wa=" .. tostring(wa2.Active) .. " wb=" .. tostring(wb2.Active))
out("done")
