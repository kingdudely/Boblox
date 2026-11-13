-- native_mesh_probe.lua — MeshPart/SpecialMesh rendering parity.
--
-- Both clients load the same native mesh files from rbxasset:// and place a
-- textured MeshPart and a SpecialMesh in front of the local character.  The
-- probe logs the replicated properties and a stable placement; the screenshot
-- shows the mesh silhouette and texture that the native renderer draws.
local function log(...)
    print("MESH " .. table.concat({...}, " "))
end

local Players = game:GetService("Players")
local player = Players.LocalPlayer
local deadline = os.clock() + 20
while (not player.Character) and os.clock() < deadline do
    task.wait(0.1)
end
local character = player.Character
local root = character and character:FindFirstChild("HumanoidRootPart")
if not root then
    log("no-root", "character=" .. tostring(character))
    return
end

-- In front of the avatar, at eye height; fixed values keep both captures
-- comparable even when the camera is still interpolating in.
local forward = root.CFrame.LookVector
local base = root.Position + forward * 10 + Vector3.new(0, 2, 0)

local mesh = Instance.new("MeshPart")
mesh.Name = "MeshProbe"
mesh.MeshId = "rbxasset://avatar/meshes/torso.mesh"
mesh.TextureID = "rbxasset://content/5a7912dc5a2b6e67ab01e464037d51624a5a9ebd.png"
mesh.Size = Vector3.new(4, 6, 3)
mesh.Anchored = true
mesh.CanCollide = false
mesh.Color = Color3.fromRGB(210, 120, 60)
mesh.CFrame = CFrame.new(base + Vector3.new(-4, 0, 0))
mesh.Parent = workspace

local specialPart = Instance.new("Part")
specialPart.Name = "SpecialProbe"
specialPart.Size = Vector3.new(1, 1, 1)
specialPart.Anchored = true
specialPart.CanCollide = false
specialPart.Transparency = 0
specialPart.CFrame = CFrame.new(base + Vector3.new(4, 0, 0))
specialPart.Parent = workspace

local special = Instance.new("SpecialMesh")
special.MeshId = "rbxasset://avatar/heads/head.mesh"
special.TextureId = "rbxasset://content/5a7912dc5a2b6e67ab01e464037d51624a5a9ebd.png"
special.Scale = Vector3.new(4, 4, 4)
special.Parent = specialPart

log("ready", "meshpart=" .. tostring(mesh.MeshId),
    "texture=" .. tostring(mesh.TextureID),
    "size=" .. tostring(mesh.Size),
    "special=" .. tostring(special.MeshId),
    "scale=" .. tostring(special.Scale),
    "position=" .. tostring(mesh.Position))

for i = 1, 60 do
    task.wait(0.1)
end
log("still", "mesh=" .. tostring(mesh.Parent ~= nil),
    "special=" .. tostring(special.Parent ~= nil))
