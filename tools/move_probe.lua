local plr
for _ = 1, 45 do
    local ok, p = pcall(function() return game:GetService("Players").LocalPlayer end)
    if ok and p then plr = p end
    if plr and plr.Character and plr.Character:FindFirstChildOfClass("Humanoid") then break end
    task.wait(1)
end
local hum = plr and plr.Character and plr.Character:FindFirstChildOfClass("Humanoid")
print("[mp] hum=", tostring(hum))
if not hum then return end
for i = 1, 30 do hum:Move(Vector3.new(0, 0, -1), false) task.wait(0.1) end
print("[mp] moved")
