-- font_metrics_extended_probe.lua — non-ASCII and out-of-range TextSize
-- comparison against native TextBounds and AutomaticSize.
--
-- Native clamps TextSize to 1..100 and floors fractional values.  The strings
-- cover Latin-1, Latin Extended-A, combining marks, punctuation and the common
-- symbol set captured by font_metrics_capture.lua.  Sizes below 1, between
-- integers, and above 100 exercise the native clamp/floor path.
if _G.__BOBLOX_FONT_METRICS_EXTENDED_PROBE then return end
_G.__BOBLOX_FONT_METRICS_EXTENDED_PROBE = true

local Players = game:GetService("Players")
local pg = Players.LocalPlayer:WaitForChild("PlayerGui", 15)
local function out(s) print("FONT_EDGE " .. s) end
if not pg then out("no PlayerGui") return end

local old = pg:FindFirstChild("FontMetricsExtendedProbe")
if old then old:Destroy() end
local screen = Instance.new("ScreenGui")
screen.Name = "FontMetricsExtendedProbe"
screen.IgnoreGuiInset = true
screen.ResetOnSpawn = false
screen.DisplayOrder = 1012
screen.Parent = pg

local face_enums = {
    Enum.Font.Legacy, Enum.Font.Arial, Enum.Font.ArialBold,
    Enum.Font.SourceSans, Enum.Font.SourceSansBold,
}
-- Every character here is in the generated native charset fixture.
local strings = {
    "Café", "naïve", "Straße", "Ærø", "Łódź", "Œuvre",
    "“Hello”—world", "€ £ ¥ © ® ™", "→ ★ ✓ • …", "A★B",
    "e\204\129", "Vé", "Ångström", "The quick brown fox",
    "ÀÉÎÕÜ", "¡Hola!", "¿Qué?", "Café ★",
}
local sizes = {0.25, 1.99, 13.01, 13.99, 50.5, 99.99, 100.0, 100.01,
               150, 1000}

local labels = {}
local auto_labels = {}
for font = 0, 4 do
    labels[font] = {}
    for i, text in ipairs(strings) do
        local l = Instance.new("TextLabel")
        l.Name = "E" .. font .. "_" .. i
        l.Size = UDim2.new(0, 4000, 0, 400)
        l.BackgroundTransparency = 1
        l.TextScaled = false
        l.TextWrapped = false
        l.TextXAlignment = Enum.TextXAlignment.Left
        l.Text = text
        l.Font = face_enums[font + 1]
        l.Parent = screen
        labels[font][i] = l
    end
    local a = Instance.new("TextLabel")
    a.Name = "EA" .. font
    a.Size = UDim2.new(0, 0, 0, 0)
    a.BackgroundTransparency = 1
    a.Text = "Café ★"
    a.Font = face_enums[font + 1]
    a.AutomaticSize = Enum.AutomaticSize.XY
    a.Parent = screen
    auto_labels[font] = a
end

local rs = game:GetService("RunService")
for _, s in ipairs(sizes) do
    for font = 0, 4 do
        for i = 1, #strings do labels[font][i].TextSize = s end
        auto_labels[font].TextSize = s
    end
    rs.Heartbeat:Wait()
    -- Boblox materializes layout on an absolute-geometry read; native has
    -- already committed it on the frame above.
    for font = 0, 4 do
        for i = 1, #strings do
            local _ = labels[font][i].AbsolutePosition
        end
        local _ = auto_labels[font].AbsolutePosition
    end
    for font = 0, 4 do
        local widths = {}
        for i = 1, #strings do
            widths[i] = string.format("%.0f", labels[font][i].TextBounds.X)
        end
        local ab = auto_labels[font].TextBounds
        out(string.format("f=%d s=%.2f w=%s auto=%.0fx%.0f",
            font, s, table.concat(widths, ","), ab.X, ab.Y))
    end
end
out("DONE")
