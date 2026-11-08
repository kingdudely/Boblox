for _ = 1, 40 do
    local ok, p = pcall(function() return game:GetService("Players").LocalPlayer end)
    if ok and p and p.Character then break end
    task.wait(1)
end
local remote
local function find(o, d)
    if d > 5 or remote then return end
    local ok, kids = pcall(function() return o:GetChildren() end)
    if not ok then return end
    for _, c in ipairs(kids) do
        if remote then return end
        local okIs, is = pcall(function() return c:IsA("RemoteEvent") end)
        if okIs and is then remote = c return end
        find(c, d + 1)
    end
end
find(game:GetService("ReplicatedStorage"), 0)
if not remote then print("[probe] no remote") return end
local part = Instance.new("Part")
part.Name = "P"
local M = "INSTAGPROBE1234567890"
local ok, err = pcall(function() remote:FireServer(part, M) end)
print("[probe] fire", ok, tostring(err))
task.wait(2)
print("[probe] done")
