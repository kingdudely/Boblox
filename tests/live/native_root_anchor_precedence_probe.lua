-- native_root_anchor_precedence_probe.lua — which anchor owns an unanchored
-- middle when the anchors differ in size or RootPriority but the active weld
-- points at the other anchor?
local ws = game:GetService("Workspace")

local function out(s)
    rconsoleprint("ROOTAPREC " .. s .. "\n")
    print("ROOTAPREC " .. s)
end

local folder = Instance.new("Folder")
folder.Name = "RootAPrecNative"
folder.Parent = ws

local function part(name, pos, size, anchored)
    local p = Instance.new("Part")
    p.Name = name
    p.Size = size
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
    out(string.format("%s part=%s root=%s priority=%d active1=%s active2=%s",
                      tag, p.Name, rootName(p), p.RootPriority,
                      j1 and tostring(j1.Active) or "?",
                      j2 and tostring(j2.Active) or "?"))
end

-- P1: small anchor A, large anchor B.  B-middle weld is older; A-middle
-- weld is newer, so the active weld should be the small anchor if newest
-- wins.  If size/root order wins, the middle follows B.
local p1a = part("RootAP_SmallA", Vector3.new(1200, 500, 500),
                 Vector3.new(1, 1, 1), true)
local p1m = part("RootAP_M1", Vector3.new(1202, 500, 500),
                 Vector3.new(2, 2, 2), false)
local p1b = part("RootAP_LargeB", Vector3.new(1204, 500, 500),
                 Vector3.new(12, 12, 12), true)
local p1wb = weld("RootAP1_WB", p1m, p1b)
local p1wa = weld("RootAP1_WA", p1a, p1m)

-- P2: same sizes, but the large anchor weld is newer (active large).
local p2a = part("RootAP_SmallA2", Vector3.new(1210, 500, 500),
                 Vector3.new(1, 1, 1), true)
local p2m = part("RootAP_M2", Vector3.new(1212, 500, 500),
                 Vector3.new(2, 2, 2), false)
local p2b = part("RootAP_LargeB2", Vector3.new(1214, 500, 500),
                 Vector3.new(12, 12, 12), true)
local p2wa = weld("RootAP2_WA", p2a, p2m)
local p2wb = weld("RootAP2_WB", p2m, p2b)

-- P3: equal anchors, high RootPriority on A; B-middle weld newer, so the
-- active weld points at B.  If priority wins, middle follows A.
local p3a = part("RootAP_PriorityA", Vector3.new(1220, 500, 500),
                 Vector3.new(2, 2, 2), true)
p3a.RootPriority = 100
local p3m = part("RootAP_M3", Vector3.new(1222, 500, 500),
                 Vector3.new(2, 2, 2), false)
local p3b = part("RootAP_PlainB", Vector3.new(1224, 500, 500),
                 Vector3.new(2, 2, 2), true)
local p3wa = weld("RootAP3_WA", p3a, p3m)
local p3wb = weld("RootAP3_WB", p3m, p3b)

-- P4: equal anchors, high RootPriority on B; A-middle weld newer.
local p4a = part("RootAP_PlainA", Vector3.new(1230, 500, 500),
                 Vector3.new(2, 2, 2), true)
local p4m = part("RootAP_M4", Vector3.new(1232, 500, 500),
                 Vector3.new(2, 2, 2), false)
local p4b = part("RootAP_PriorityB", Vector3.new(1234, 500, 500),
                 Vector3.new(2, 2, 2), true)
p4b.RootPriority = 100
local p4wb = weld("RootAP4_WB", p4m, p4b)
local p4wa = weld("RootAP4_WA", p4a, p4m)

-- P5: small anchor A has high RootPriority, large anchor B is plain; the
-- newer link points at A.  This distinguishes priority from size.
local p5a = part("RootAP_PrioritySmallA", Vector3.new(1240, 500, 500),
                 Vector3.new(1, 1, 1), true)
p5a.RootPriority = 100
local p5m = part("RootAP_M5", Vector3.new(1242, 500, 500),
                 Vector3.new(2, 2, 2), false)
local p5b = part("RootAP_LargePlainB", Vector3.new(1244, 500, 500),
                 Vector3.new(12, 12, 12), true)
local p5wb = weld("RootAP5_WB", p5m, p5b)
local p5wa = weld("RootAP5_WA", p5a, p5m)

task.wait(1.5)

report("P1-middle-small-active", p1m, p1wa, p1wb)
report("P1-smallA", p1a)
report("P1-largeB", p1b)
report("P2-middle-large-active", p2m, p2wa, p2wb)
report("P3-middle-priority-active-other", p3m, p3wa, p3wb)
report("P4-middle-priority-anchor", p4m, p4wa, p4wb)
report("P5-middle-priority-small", p5m, p5wa, p5wb)
out("done")
