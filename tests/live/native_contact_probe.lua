-- Native contact probe: dynamic/static contact queries, grounding, and touch
-- transitions.  Run the same script against native 189707/1818 and Boblox.
local function log(...)
    print("CONTACT " .. table.concat({...}, " "))
end

-- The live join streams properties asynchronously; wait for the replicated
-- gravity instead of sampling the class defaults before they arrive.
local deadline = os.clock() + 20
while workspace.Gravity <= 0 and os.clock() < deadline do
    task.wait(0.1)
end
log("workspace", "gravity=" .. tostring(workspace.Gravity),
    "authority=" .. tostring(workspace.AuthorityMode),
    "children=" .. tostring(#workspace:GetChildren()))

local a = Instance.new("Part")
a.Name = "ContactProbeA"
a.Size = Vector3.new(2, 2, 2)
a.Anchored = true
a.CanCollide = true
a.CFrame = CFrame.new(0, 50, 0)
a.Parent = workspace

local b = Instance.new("Part")
b.Name = "ContactProbeB"
b.Size = Vector3.new(2, 2, 2)
b.Anchored = false
b.CanCollide = true
b.CFrame = CFrame.new(0, 60, 0)
b.Parent = workspace

local touchedA, touchedB = 0, 0
local endedA, endedB = 0, 0
a.Touched:Connect(function(hit)
    if hit == b then touchedA = touchedA + 1 end
end)
b.Touched:Connect(function(hit)
    if hit == a then touchedB = touchedB + 1 end
end)
a.TouchEnded:Connect(function(hit)
    if hit == b then endedA = endedA + 1 end
end)
b.TouchEnded:Connect(function(hit)
    if hit == a then endedB = endedB + 1 end
end)

for i = 1, 5 do
    task.wait(0.1)
end
local beforeA, beforeB = touchedA, touchedB
-- Drop the unanchored part onto the anchored one.  The native client reports
-- touch events only for solver contacts; the static/static case is covered by
-- the separate query behaviour in unit_physics.
for i = 1, 30 do
    task.wait(0.1)
end
log("anchored", "touchA=" .. tostring(#a:GetTouchingParts()),
    "touchB=" .. tostring(#b:GetTouchingParts()),
    "groundA=" .. tostring(a:IsGrounded()),
    "events=" .. touchedA .. "/" .. touchedB .. "/" .. endedA .. "/" ..
        endedB,
    "delta=" .. tostring(touchedA - beforeA) .. "/" ..
        tostring(touchedB - beforeB),
    "bpos=" .. tostring(b.Position.Y))

-- Remove the contacting part after the connection is installed.  This is the
-- native TouchEnded path for both the solver contact and the query cache.
local endedBeforeA, endedBeforeB = endedA, endedB
b:Destroy()
for i = 1, 20 do
    task.wait(0.1)
end

local hrp = nil
local player = game:GetService("Players").LocalPlayer
if player and player.Character then
    hrp = player.Character:FindFirstChild("HumanoidRootPart")
end
log("destroyed", "touchA=" .. tostring(#a:GetTouchingParts()),
    "events=" .. touchedA .. "/" .. touchedB .. "/" .. endedA .. "/" ..
        endedB,
    "deltaEnd=" .. tostring(endedA - endedBeforeA) .. "/" ..
        tostring(endedB - endedBeforeB),
    "bDestroyed=" .. tostring(b.Parent == nil),
    "playerHRP=" .. tostring(hrp ~= nil),
    "playerGrounded=" .. tostring(hrp and hrp:IsGrounded()))
for _, hit in ipairs(a:GetTouchingParts()) do
    print("CONTACT destroyedHit " .. hit.Name)
end
