-- native_root_long_chain_probe.lua — equal anchors with a longer unanchored
-- chain between them.  The query is the middle part, so it has no direct
-- anchor link; this tests how the native split chooses a side.
local ws = game:GetService("Workspace")

local function out(s)
    rconsoleprint("ROOTCHAIN " .. s .. "\n")
    print("ROOTCHAIN " .. s)
end

local folder = Instance.new("Folder")
folder.Name = "RootChainNative"
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

local function report(tag, p)
    out(string.format("%s part=%s root=%s", tag, p.Name, rootName(p)))
end

local function active(tag, joints)
    local bits = {}
    for i, j in ipairs(joints) do
        bits[i] = j.Name .. "=" .. tostring(j.Active)
    end
    out(tag .. " " .. table.concat(bits, " "))
end

-- G1: path joints created A->B; newest joint is B-M3.
local a1 = part("RootChain1_A", Vector3.new(1300, 500, 500), true)
local m11 = part("RootChain1_M1", Vector3.new(1302, 500, 500), false)
local m12 = part("RootChain1_M2", Vector3.new(1304, 500, 500), false)
local m13 = part("RootChain1_M3", Vector3.new(1306, 500, 500), false)
local b1 = part("RootChain1_B", Vector3.new(1308, 500, 500), true)
local j11 = weld("RootChain1_J1", a1, m11)
local j12 = weld("RootChain1_J2", m11, m12)
local j13 = weld("RootChain1_J3", m12, m13)
local j14 = weld("RootChain1_J4", m13, b1)

-- G2: same parts, path joints created B->A; newest joint is A-M1.
local a2 = part("RootChain2_A", Vector3.new(1320, 500, 500), true)
local m21 = part("RootChain2_M1", Vector3.new(1322, 500, 500), false)
local m22 = part("RootChain2_M2", Vector3.new(1324, 500, 500), false)
local m23 = part("RootChain2_M3", Vector3.new(1326, 500, 500), false)
local b2 = part("RootChain2_B", Vector3.new(1328, 500, 500), true)
local j24 = weld("RootChain2_J4", m23, b2)
local j23 = weld("RootChain2_J3", m22, m23)
local j22 = weld("RootChain2_J2", m21, m22)
local j21 = weld("RootChain2_J1", a2, m21)

-- G3: both anchor links old, middle link newest: A-M1, M3-B, M1-M2, M2-M3.
local a3 = part("RootChain3_A", Vector3.new(1340, 500, 500), true)
local m31 = part("RootChain3_M1", Vector3.new(1342, 500, 500), false)
local m32 = part("RootChain3_M2", Vector3.new(1344, 500, 500), false)
local m33 = part("RootChain3_M3", Vector3.new(1346, 500, 500), false)
local b3 = part("RootChain3_B", Vector3.new(1348, 500, 500), true)
local j31 = weld("RootChain3_J1", a3, m31)
local j34 = weld("RootChain3_J4", m33, b3)
local j32 = weld("RootChain3_J2", m31, m32)
local j33 = weld("RootChain3_J3", m32, m33)

task.wait(1.5)

report("G1-A", a1)
report("G1-B", b1)
report("G1-M1", m11)
report("G1-M2", m12)
report("G1-M3", m13)
active("G1", {j11, j12, j13, j14})
report("G2-A", a2)
report("G2-B", b2)
report("G2-M1", m21)
report("G2-M2", m22)
report("G2-M3", m23)
active("G2", {j21, j22, j23, j24})
report("G3-A", a3)
report("G3-B", b3)
report("G3-M1", m31)
report("G3-M2", m32)
report("G3-M3", m33)
active("G3", {j31, j32, j33, j34})
out("done")
