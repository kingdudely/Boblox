-- joint_probe.lua — live proof for BasePart assembly queries.
--
-- Run through --exec on a live session.  Creates a two-part assembly with a
-- Weld, a WeldConstraint to a third part, and a fourth part that only
-- overlaps the first.  Reports the direct/recursive connected sets and
-- GetJoints so the parser/physics API can be checked against real Engine
-- state rather than only a synthetic unit test.
local ws = game:GetService("Workspace")

local deadline = os.clock() + 20
while #ws:GetDescendants() < 100 and os.clock() < deadline do
    task.wait(0.25)
end

local function makePart(name, cf, size)
    local p = Instance.new("Part")
    p.Name = name
    p.Anchored = true
    p.Size = size
    p.CFrame = cf
    p.Parent = ws
    return p
end

local a = makePart("JointProbeA", CFrame.new(0, 50, 0), Vector3.new(4, 1, 2))
local b = makePart("JointProbeB", CFrame.new(1, 50, 0), Vector3.new(2, 2, 2))
local c = makePart("JointProbeC", CFrame.new(0, 50, 10), Vector3.new(2, 2, 2))
local d = makePart("JointProbeOverlap", CFrame.new(0.2, 50, 0), Vector3.new(2, 2, 2))

local weld = Instance.new("Weld")
weld.Name = "JointProbeWeld"
weld.Part0 = a
weld.Part1 = b
weld.Parent = ws

local constraint = Instance.new("WeldConstraint")
constraint.Name = "JointProbeConstraint"
constraint.Part0 = b
constraint.Part1 = c
constraint.Parent = ws

local disabled = Instance.new("Weld")
disabled.Name = "JointProbeDisabled"
disabled.Part0 = a
disabled.Part1 = d
disabled.Enabled = false
disabled.Parent = ws

task.wait(0.25)

local direct = a:GetConnectedParts()
local recursive = a:GetConnectedParts(true)
local joints = a:GetJoints()
local bJoints = b:GetJoints()
local directNames, recursiveNames, jointNames = {}, {}, {}
for _, p in ipairs(direct) do directNames[#directNames + 1] = p.Name end
for _, p in ipairs(recursive) do recursiveNames[#recursiveNames + 1] = p.Name end
for _, j in ipairs(joints) do jointNames[#jointNames + 1] = j.Name end
table.sort(directNames)
table.sort(recursiveNames)
table.sort(jointNames)

print(string.format(
    "joint-probe direct=%d [%s] recursive=%d [%s] joints=%d [%s] bJoints=%d overlapDirect=%s overlapRecursive=%s defaults=%s/%s disabledIgnored=%s",
    #direct, table.concat(directNames, ","), #recursive,
    table.concat(recursiveNames, ","), #joints,
    table.concat(jointNames, ","), #bJoints,
    tostring(table.find(direct, d) ~= nil),
    tostring(table.find(recursive, d) ~= nil),
    tostring(weld.Enabled), tostring(constraint.Enabled),
    tostring(table.find(direct, d) == nil and #direct == 2)))
