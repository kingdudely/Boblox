-- physics_probe.lua — live shape of the replicated physics world.
--
-- Run through --exec after the world is applied. It reports what a local
-- solver can own (unanchored parts), what it must treat as server geometry
-- (anchored parts), and whether the server is already moving anything.
local ws = game:GetService("Workspace")

local function report(tag)
    local parts, anchored, unanchored, moving = 0, 0, 0, 0
    local max_speed, sample = 0, nil
    local owners, avatar_parts = {}, 0
    for _, d in ipairs(ws:GetDescendants()) do
        if d:IsA("BasePart") then
            parts = parts + 1
            if d.Anchored then
                anchored = anchored + 1
            else
                unanchored = unanchored + 1
                local p = d
                local in_character = false
                while p and p ~= ws do
                    if p:IsA("Model") and p:FindFirstChildOfClass("Humanoid") then
                        in_character = true
                        break
                    end
                    p = p.Parent
                end
                if in_character then avatar_parts = avatar_parts + 1 end
                local ok, owner = pcall(function() return d.NetworkOwnerV3 end)
                local key = ok and tostring(owner) or "<hidden>"
                owners[key] = (owners[key] or 0) + 1
                local v = d.AssemblyLinearVelocity
                local speed = v.Magnitude
                if speed > 0.1 then
                    moving = moving + 1
                    if speed > max_speed then
                        max_speed = speed
                        sample = d
                    end
                end
            end
        end
    end
    local owner_text = ""
    for k, v in pairs(owners) do
        owner_text = owner_text .. " " .. tostring(k) .. ":" .. v
    end
    print(string.format(
        "physics-probe %s parts=%d anchored=%d unanchored=%d avatars=%d " ..
        "moving=%d gravity=%.1f owners=%s",
        tag, parts, anchored, unanchored, avatar_parts, moving, ws.Gravity,
        owner_text))
    if sample then
        local p = sample.Position
        print(string.format(
            "physics-probe %s fastest='%s' class=%s speed=%.3f pos=(%.2f, %.2f, %.2f)",
            tag, sample:GetFullName(), sample.ClassName, max_speed, p.X, p.Y,
            p.Z))
    end
    print("physics-probe " .. tag .. " filtering=" ..
        tostring(ws.FilteringEnabled) .. " authority=" ..
        tostring(ws.AuthorityMode) .. " physics_fps=" ..
        tostring(ws:GetRealPhysicsFPS()))
end

-- The executor starts when the join data arrives, before the RCC has finished
-- streaming the place tree.  Wait for a populated Workspace so the first
-- report is not an empty tree.
local deadline = os.clock() + 20
while #ws:GetDescendants() < 100 and os.clock() < deadline do
    task.wait(0.25)
end
task.wait(2)
report("t8")
task.wait(10)
report("t18")
