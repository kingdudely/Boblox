-- font_metrics_probe.lua — wide-size native/Boblox text-metrics comparison.
--
-- Covers every native Legacy face (Legacy, Arial, ArialBold, SourceSans,
-- SourceSansBold) at TextSize 1, 8, and every integer 9..100.  Widths exercise
-- plain advances, GPOS pairs (AV/VA/AT/TA), spaces, and punctuation; the
-- wrapped/fits/automatic rows exercise layout, line breaks and TextFits from
-- the same metric source.
--
-- Each size is read after a Heartbeat so the native TextBounds pipeline has
-- committed the new TextSize.  The output is one line per face/size:
--   FONT_METRIC f=<font> s=<size> w=<8 comma-separated widths> wrap=WxH fits=<bool> auto=WxH
--
-- Native output is captured in chunks when using Xecutor's ring buffer; split
-- the size loop and concatenate the TXT rows.  Boblox can run the file whole.
if _G.__BOBLOX_FONT_METRICS_PROBE then return end
_G.__BOBLOX_FONT_METRICS_PROBE = true

local Players = game:GetService("Players")
local pg = Players.LocalPlayer:WaitForChild("PlayerGui", 15)
local function out(s) print("FONT_METRIC " .. s) end
if not pg then out("no PlayerGui") return end

local old = pg:FindFirstChild("FontMetricsProbe")
if old then old:Destroy() end
local screen = Instance.new("ScreenGui")
screen.Name = "FontMetricsProbe"
screen.IgnoreGuiInset = true
screen.ResetOnSpawn = false
screen.DisplayOrder = 1011
screen.Parent = pg

local face_enums = {
    Enum.Font.Legacy, Enum.Font.Arial, Enum.Font.ArialBold,
    Enum.Font.SourceSans, Enum.Font.SourceSansBold,
}
local strings = {
    "A", "AVATAR", "Hello", "Hello world",
    "The quick brown fox jumps over the lazy dog",
    "To Wa", "WAVE", "| ||",
}

local labels = {}
local wrap_labels = {}
local fit_labels = {}
local auto_labels = {}
for font = 0, 4 do
    labels[font] = {}
    for i, text in ipairs(strings) do
        local l = Instance.new("TextLabel")
        l.Name = "M" .. font .. "_" .. i
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
    local w = Instance.new("TextLabel")
    w.Name = "W" .. font
    w.Size = UDim2.new(0, 120, 0, 400)
    w.BackgroundTransparency = 1
    w.TextWrapped = true
    w.TextXAlignment = Enum.TextXAlignment.Left
    w.Text = "Hello world this is a long line"
    w.Font = face_enums[font + 1]
    w.Parent = screen
    wrap_labels[font] = w

    local f = Instance.new("TextLabel")
    f.Name = "F" .. font
    f.Size = UDim2.new(0, 120, 0, 12)
    f.BackgroundTransparency = 1
    f.TextWrapped = true
    f.Text = "Hello world this is a long line"
    f.Font = face_enums[font + 1]
    f.Parent = screen
    fit_labels[font] = f

    local a = Instance.new("TextLabel")
    a.Name = "A" .. font
    a.Size = UDim2.new(0, 0, 0, 0)
    a.BackgroundTransparency = 1
    a.Text = "Hello world"
    a.Font = face_enums[font + 1]
    a.AutomaticSize = Enum.AutomaticSize.XY
    a.Parent = screen
    auto_labels[font] = a
end

local rs = game:GetService("RunService")
local sizes = {1, 8}
for s = 9, 100 do sizes[#sizes + 1] = s end
for _, s in ipairs(sizes) do
    for font = 0, 4 do
        for i = 1, #strings do labels[font][i].TextSize = s end
        wrap_labels[font].TextSize = s
        fit_labels[font].TextSize = s
        fit_labels[font].Size = UDim2.new(0, 120, 0,
            math.floor((font == 0 and 1.5 or 1.0) * s) * 2)
        auto_labels[font].TextSize = s
    end
    rs.Heartbeat:Wait()
    -- AbsolutePosition reads synchronously materialize the layout in Boblox;
    -- native has already committed it on the frame above.
    for font = 0, 4 do
        for i = 1, #strings do
            local _ = labels[font][i].AbsolutePosition
        end
        local _ = wrap_labels[font].AbsolutePosition
        local _ = fit_labels[font].AbsolutePosition
        local _ = auto_labels[font].AbsolutePosition
    end
    for font = 0, 4 do
        local widths = {}
        for i = 1, #strings do
            widths[i] = string.format("%.0f", labels[font][i].TextBounds.X)
        end
        local wb = wrap_labels[font].TextBounds
        local ab = auto_labels[font].TextBounds
        out(string.format(
            "f=%d s=%d w=%s wrap=%.0fx%.0f fits=%s auto=%.0fx%.0f",
            font, s, table.concat(widths, ","), wb.X, wb.Y,
            tostring(fit_labels[font].TextFits), ab.X, ab.Y))
    end
end
out("DONE")
