-- a6_move_probe.lua — drive the local PlayerControlState used by the native
-- 141-byte 0xA6 frame and report what the engine exposes.
--
-- Run with --exec on Boblox and through Xecutor on the native client. The
-- native equivalent drives the same field through the PlayerModule; this
-- probe reports the observable ControlState, not a fabricated wire value.
local Players = game:GetService("Players")

local function out(s)
    print("A6PROBE " .. s)
    if rconsoleprint then
        rconsoleprint("A6PROBE " .. s .. "\n")
    end
end

local deadline = os.clock() + 30
local p = Players.LocalPlayer
while os.clock() < deadline and not (p and p.Character) do
    task.wait(0.25)
    p = Players.LocalPlayer
end
if not p then
    out("NO_LOCAL_PLAYER")
    return
end
out(string.format("PLAYER userId=%s character=%s", tostring(p.UserId),
                  tostring(p.Character ~= nil)))

local ok, cs = pcall(function()
    return p:GetControlState()
end)
out(string.format("CONTROL_STATE ok=%s type=%s", tostring(ok), type(cs)))
if ok and cs then
    local fields = cs:GetState(Enum.StateReferenceFrame.CurrentState)
    local names = {}
    for name, value in pairs(fields or {}) do
        names[#names + 1] = name .. "=" .. tostring(value)
    end
    table.sort(names)
    out("FIELDS " .. table.concat(names, ","))
end

-- Hold a world-space forward vector long enough for several 60 Hz A6 frames.
-- Boblox's boblox.move is the executor-side input shim; the native probe uses
-- Humanoid:Move, which the client PlayerModule turns into the same control
-- state. Both paths report the resulting field below.
local moved = false
if boblox and boblox.move then
    for _ = 1, 80 do
        boblox.move(0, 0, -1)
        task.wait(0.05)
    end
    moved = true
elseif p.Character then
    local humanoid = p.Character:FindFirstChildWhichIsA("Humanoid")
    if humanoid then
        for _ = 1, 80 do
            humanoid:Move(Vector3.new(0, 0, -1))
            task.wait(0.05)
        end
        moved = true
    end
end
out(moved and "MOVED forward" or "NO_MOVE_SOURCE")

local ok2, cs2 = pcall(function()
    return p:GetControlState()
end)
if ok2 and cs2 then
    local fields = cs2:GetState(Enum.StateReferenceFrame.CurrentState)
    local names = {}
    for name, value in pairs(fields or {}) do
        names[#names + 1] = name .. "=" .. tostring(value)
    end
    table.sort(names)
    out("FIELDS_AFTER " .. table.concat(names, ","))
end
out("A6PROBE-DONE")
