local function wait_game()
    for _ = 1, 60 do
        local ok, p = pcall(function() return game:GetService("Players").LocalPlayer end)
        if ok and p and p.Character then return p end
        task.wait(1)
    end
end
wait_game()
local remote
local function find(o, d)
    if d > 5 or remote then return end
    local ok, kids = pcall(function() return o:GetChildren() end)
    if not ok then return end
    for _, c in ipairs(kids) do
        local okIs, is = pcall(function() return c:IsA("RemoteEvent") end)
        if okIs and is then remote = c return end
        find(c, d + 1)
    end
end
find(game:GetService("ReplicatedStorage"), 0)
if not remote then print("[probe] no remote") return end
print("[probe] remote", remote:GetFullName())
local payloads = {
    { "STRINGARG_0001" },
    { 1234.5 },
    { true },
    { Vector3.new(1, 2, 3) },
    { nil },
}
for i, args in ipairs(payloads) do
    local ok, err = pcall(function() remote:FireServer(table.unpack(args)) end)
    print("[probe] arg", i, ok, tostring(err))
    task.wait(1.2)
end
print("[probe] done")
