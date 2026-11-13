-- native_root_anchor_ties_probe.lua — equal anchored candidates with an
-- unanchored middle.  Establishes which anchor owns the middle when the
-- anchors have identical mass/size/priority and only the weld creation order
-- differs.
local ws = game:GetService("Workspace")

local function out(s)
    rconsoleprint("ROOTATIE " .. s .. "\n")
    print("ROOTATIE " .. s)
end

local folder = Instance.new("Folder")
folder.Name = "RootAnchorTieNative"
folder.Parent = ws

local function part(name, pos, anchored)
    local p = Instance.new("Part")
    p.Name = name
    p.Size = Vector3.new(2, 2, 2)
    p.CFrame = CFrame.new(pos)
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

local function report(tag, p, j1, j2)
    out(string.format("%s part=%s root=%s anchored=%s activeA=%s activeB=%s",
                      tag, p.Name, rootName(p), tostring(p.Anchored),
                      j1 and tostring(j1.Active) or "?",
                      j2 and tostring(j2.Active) or "?"))
end

-- A created first, B second; A-middle weld created before middle-B weld.
local a1 = part("RootATie1_A", Vector3.new(1100, 500, 500), true)
local m1 = part("RootATie1_M", Vector3.new(1102, 500, 500), false)
local b1 = part("RootATie1_B", Vector3.new(1104, 500, 500), true)
local wa1 = weld("RootATie1_WA", a1, m1)
local wb1 = weld("RootATie1_WB", m1, b1)

-- Same creation order, welds reversed: middle-B older, A-middle newer.
local a2 = part("RootATie2_A", Vector3.new(1110, 500, 500), true)
local m2 = part("RootATie2_M", Vector3.new(1112, 500, 500), false)
local b2 = part("RootATie2_B", Vector3.new(1114, 500, 500), true)
local wb2 = weld("RootATie2_WB", m2, b2)
local wa2 = weld("RootATie2_WA", a2, m2)

-- B created first, A second; A-middle weld older.
local b3 = part("RootATie3_B", Vector3.new(1120, 500, 500), true)
local m3 = part("RootATie3_M", Vector3.new(1122, 500, 500), false)
local a3 = part("RootATie3_A", Vector3.new(1124, 500, 500), true)
local wa3 = weld("RootATie3_WA", a3, m3)
local wb3 = weld("RootATie3_WB", m3, b3)

-- Both anchor-to-middle welds, no middle between: they share the middle part.
-- (This is the same shape as above; retained to exercise the active flag.)
local a4 = part("RootATie4_A", Vector3.new(1130, 500, 500), true)
local m4 = part("RootATie4_M", Vector3.new(1132, 500, 500), false)
local b4 = part("RootATie4_B", Vector3.new(1134, 500, 500), true)
local wa4 = weld("RootATie4_WA", a4, m4)
local wb4 = weld("RootATie4_WB", b4, m4)

task.wait(1.5)

report("T1-middle", m1, wa1, wb1)
report("T1-anchorA", a1)
report("T1-anchorB", b1)
report("T2-middle", m2, wa2, wb2)
report("T2-anchorA", a2)
report("T2-anchorB", b2)
report("T3-middle", m3, wa3, wb3)
report("T3-anchorA", a3)
report("T3-anchorB", b3)
report("T4-middle", m4, wa4, wb4)
report("T4-anchorA", a4)
report("T4-anchorB", b4)
out("done")
