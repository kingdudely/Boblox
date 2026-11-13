-- native_gui_image_probe.lua — native ground truth for the GUI asset path.
-- Creates an ImageLabel with the same public image used by Boblox's renderer
-- probe and waits for the native IsLoaded transition.
local Players = game:GetService("Players")
local player = Players.LocalPlayer
local pg = player:WaitForChild("PlayerGui", 10)
if not pg then
    print("GUIIMG no PlayerGui")
    return
end

local screen = Instance.new("ScreenGui")
screen.Name = "AssetProbe"
screen.ResetOnSpawn = false
screen.Parent = pg

local image = Instance.new("ImageLabel")
image.Name = "AssetProbeImage"
image.Size = UDim2.new(0, 64, 0, 64)
image.Image = "rbxassetid://284402752"
image.Parent = screen

local thumb = Instance.new("ImageLabel")
thumb.Name = "ThumbProbeImage"
thumb.Size = UDim2.new(0, 64, 0, 64)
thumb.Position = UDim2.new(0, 80, 0, 0)
thumb.Image = "rbxthumb://type=Asset&w=150&h=150&id=284402752"
thumb.Parent = screen

for i = 1, 100 do
    if image.IsLoaded and thumb.IsLoaded then break end
    task.wait(0.1)
end
print("GUIIMG native loaded=" .. tostring(image.IsLoaded) .. " image=" ..
      image.Image .. " thumb_loaded=" .. tostring(thumb.IsLoaded) ..
      " thumb=" .. thumb.Image)