-- gui_input_probe.lua — deterministic ScreenGui exercised by RBX_GUI_TEST.
-- Coordinates match the synthetic driver in client/apps/raknet_main.cpp.
local Players = game:GetService("Players")
local player = Players.LocalPlayer
local pg = player:FindFirstChildOfClass("PlayerGui")
if not pg then
    pg = Instance.new("PlayerGui")
    pg.Name = "PlayerGui"
    pg.Parent = player
end

local screen = Instance.new("ScreenGui")
screen.Name = "GuiInputProbe"
screen.IgnoreGuiInset = true
screen.ResetOnSpawn = false
screen.DisplayOrder = 100
screen.Parent = pg

local function log(line)
    print("gui-probe: " .. line)
end

local button = Instance.new("TextButton")
button.Name = "ProbeButton"
button.Position = UDim2.new(0, 60, 0, 60)
button.Size = UDim2.new(0, 240, 0, 56)
button.BackgroundColor3 = Color3.fromRGB(42, 82, 150)
button.TextColor3 = Color3.fromRGB(255, 255, 255)
button.TextSize = 22
button.Text = "Click Me"
button.ZIndex = 2
button.Parent = screen
button.MouseButton1Down:Connect(function() log("MouseButton1Down") end)
button.MouseButton1Up:Connect(function() log("MouseButton1Up") end)
button.Activated:Connect(function() log("Activated") end)
button.MouseEnter:Connect(function() log("MouseEnter") end)
button.MouseLeave:Connect(function() log("MouseLeave") end)

local box = Instance.new("TextBox")
box.Name = "ProbeInput"
box.Position = UDim2.new(0, 60, 0, 130)
box.Size = UDim2.new(0, 360, 0, 44)
box.BackgroundColor3 = Color3.fromRGB(28, 32, 42)
box.TextColor3 = Color3.fromRGB(245, 245, 245)
box.PlaceholderColor3 = Color3.fromRGB(150, 150, 160)
box.PlaceholderText = "Type here"
box.TextSize = 20
box.TextEditable = true
box.ClearTextOnFocus = false
box.MultiLine = false
box.ZIndex = 3
box.Parent = screen
box.Focused:Connect(function() log("Focused") end)
box.FocusLost:Connect(function(enter) log("FocusLost:" .. tostring(enter)) end)
box.ReturnPressed:Connect(function() log("ReturnPressed") end)
box:GetPropertyChangedSignal("Text"):Connect(function()
    log("Text=" .. box.Text)
end)

local scroll = Instance.new("ScrollingFrame")
scroll.Name = "ProbeScroll"
scroll.Position = UDim2.new(0, 60, 0, 210)
scroll.Size = UDim2.new(0, 360, 0, 150)
scroll.BackgroundColor3 = Color3.fromRGB(32, 36, 48)
scroll.BorderSizePixel = 1
scroll.BorderColor3 = Color3.fromRGB(70, 76, 92)
scroll.CanvasSize = UDim2.new(0, 0, 0, 600)
scroll.CanvasPosition = Vector2.new(0, 0)
scroll.ScrollBarThickness = 12
scroll.ScrollBarImageColor3 = Color3.fromRGB(150, 180, 230)
scroll.ScrollBarImageVisible = true
scroll.ScrollingDirection = Enum.ScrollingDirection.XY
scroll.ScrollingEnabled = true
scroll.ElasticBehavior = Enum.ElasticBehavior.WhenScrollable
scroll.ZIndex = 1
scroll.Parent = screen

local content = Instance.new("Frame")
content.Name = "Canvas"
content.Size = UDim2.new(1, 0, 0, 600)
content.BackgroundTransparency = 1
content.Parent = scroll
for i = 1, 8 do
    local row = Instance.new("TextLabel")
    row.Name = "Row" .. i
    row.Position = UDim2.new(0, 8, 0, (i - 1) * 70)
    row.Size = UDim2.new(1, -24, 0, 54)
    row.BackgroundColor3 = Color3.fromRGB(48 + i * 6, 58 + i * 5, 82)
    row.TextColor3 = Color3.fromRGB(235, 238, 245)
    row.TextSize = 18
    row.Text = "Scroll row " .. i
    row.TextXAlignment = Enum.TextXAlignment.Left
    row.Parent = content
end
scroll:GetPropertyChangedSignal("CanvasPosition"):Connect(function()
    log(string.format("CanvasPosition=%.0f,%.0f",
        scroll.CanvasPosition.X, scroll.CanvasPosition.Y))
end)

log("ready parent=" .. screen.Parent:GetFullName() ..
    " enabled=" .. tostring(screen.Enabled) ..
    " btnVisible=" .. tostring(button.Visible) ..
    " btnActive=" .. tostring(button.Active) ..
    " ignoreInset=" .. tostring(screen.IgnoreGuiInset))
