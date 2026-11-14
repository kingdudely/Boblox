-- automatic_size_probe.lua — native/Boblox comparison for GuiObject.AutomaticSize.
-- Every case sits at y=0 and reports frame-relative geometry so the host topbar
-- inset and native's offscreen grid orientation quirk cannot affect the rows.
if _G.__BOBLOX_AUTOMATIC_SIZE_PROBE then return end
_G.__BOBLOX_AUTOMATIC_SIZE_PROBE = true
local Players = game:GetService("Players")
local pg = Players.LocalPlayer:WaitForChild("PlayerGui", 15)
local function out(s) print("AUTO_SIZE " .. s) end
if not pg then out("no PlayerGui") return end
local old = pg:FindFirstChild("AutoSizeProbe")
if old then old:Destroy() end
local screen = Instance.new("ScreenGui")
screen.Name = "AutoSizeProbe"
screen.IgnoreGuiInset = true
screen.ResetOnSpawn = false
screen.DisplayOrder = 1007
screen.Parent = pg

local function frame(name, x, y, w, h)
    local f = Instance.new("Frame")
    f.Name = name
    f.Position = UDim2.new(0, x, 0, y)
    f.Size = UDim2.new(0, w, 0, h)
    f.BackgroundTransparency = 1
    f.Parent = screen
    return f
end
local function kid(parent, name, x, y, w, h, class)
    local f = Instance.new(class or "Frame")
    f.Name = name
    f.Position = UDim2.new(0, x, 0, y)
    f.Size = UDim2.new(0, w, 0, h)
    f.BackgroundTransparency = 1
    f.Parent = parent
    return f
end
local function row(tag, f, kids)
    local size = f.AbsoluteSize
    local pos = f.AbsolutePosition
    out(string.format("%s auto=%s value=%d authored=%.1f,%.1f %.1fx%.1f size=%.1fx%.1f",
        tag, tostring(f.AutomaticSize), f.AutomaticSize.Value,
        f.Size.X.Scale, f.Size.X.Offset, f.Size.Y.Scale, f.Size.Y.Offset,
        size.X, size.Y))
    for _, c in ipairs(kids or {}) do
        local cs = c.AbsoluteSize
        local cp = c.AbsolutePosition
        out(string.format("%s %s rel=%.1f,%.1f size=%.1fx%.1f", tag, c.Name,
            cp.X - pos.X, cp.Y - pos.Y, cs.X, cs.Y))
    end
end
local function auto(name, mode, x, y)
    local f = frame(name, x or 0, y or 0, 0, 0)
    f.AutomaticSize = mode
    return f
end

-- Enum/None behavior and all four modes.
for _, mode in ipairs({Enum.AutomaticSize.None, Enum.AutomaticSize.X,
                       Enum.AutomaticSize.Y, Enum.AutomaticSize.XY}) do
    local tag = (tostring(mode):gsub("Enum.AutomaticSize.", ""))
    local f = auto("Mode_" .. tag, mode)
    local c = kid(f, "C", 10, 20, 100, 50)
    row("mode_" .. tag, f, {c})
end

-- Empty, padding, border, invisible children.
do
    local f = auto("Empty", Enum.AutomaticSize.XY)
    row("empty", f, {})
end
do
    local f = auto("Pad", Enum.AutomaticSize.XY)
    local c = kid(f, "C", 0, 0, 100, 50)
    local p = Instance.new("UIPadding")
    p.PaddingLeft = UDim.new(0, 10); p.PaddingRight = UDim.new(0, 10)
    p.PaddingTop = UDim.new(0, 10); p.PaddingBottom = UDim.new(0, 10)
    p.Parent = f
    row("pad", f, {c})
end
do
    local f = auto("Border", Enum.AutomaticSize.XY)
    f.BorderSizePixel = 3
    local c = kid(f, "C", 0, 0, 100, 50)
    row("border", f, {c})
end
do
    local f = auto("Invisible", Enum.AutomaticSize.XY)
    local a = kid(f, "A", 0, 0, 40, 30)
    local b = kid(f, "B", 100, 100, 100, 50)
    b.Visible = false
    row("invisible", f, {a})
end

-- Explicit sizes: offset and scale components, with a fixed-size parent so
-- percentage resolution is independent of the host viewport.
do
    local holder = frame("Holder", 0, 0, 400, 300)
    local f = kid(holder, "F", 10, 10, 0, 0)
    f.AutomaticSize = Enum.AutomaticSize.XY
    f.Size = UDim2.new(0.5, 0, 0.5, 0)
    local c = kid(f, "C", 0, 0, 100, 50)
    row("explicit_scale", f, {c})
end
do
    local f = auto("ExplicitSmall", Enum.AutomaticSize.XY)
    f.Size = UDim2.new(0, 20, 0, 10)
    local c = kid(f, "C", 0, 0, 100, 50)
    row("explicit_small", f, {c})
end
do
    local f = auto("ExplicitBig", Enum.AutomaticSize.XY)
    f.Size = UDim2.new(0, 140, 0, 90)
    local c = kid(f, "C", 0, 0, 100, 50)
    row("explicit_big", f, {c})
end

-- Negative and degenerate child geometry.
do
    local f = auto("Negative", Enum.AutomaticSize.XY)
    local a = kid(f, "A", -20, -10, 50, 40)
    local b = kid(f, "B", 100, 100, 30, 20)
    row("negative", f, {a, b})
end
do
    local f = auto("NegativeOnly", Enum.AutomaticSize.XY)
    local a = kid(f, "A", -20, -10, 10, 10)
    row("negative_only", f, {a})
end
do
    local f = auto("ZeroChild", Enum.AutomaticSize.XY)
    local a = kid(f, "A", 10, 20, 0, 0)
    row("zero_child", f, {a})
end

-- Nested automatic parents are resolved inside out.
do
    local f = auto("Nested", Enum.AutomaticSize.XY)
    local m = kid(f, "M", 5, 5, 0, 0)
    m.AutomaticSize = Enum.AutomaticSize.XY
    local c = kid(m, "C", 10, 20, 100, 50)
    row("nested", f, {m, c})
end

-- GuiBase2d child kinds contribute their resolved sizes.
do
    local f = auto("Kinds", Enum.AutomaticSize.XY)
    local a = kid(f, "Frame", 0, 0, 40, 20)
    local b = kid(f, "TextButton", 50, 10, 30, 20, "TextButton")
    local c = kid(f, "ImageLabel", 0, 40, 20, 25, "ImageLabel")
    row("kinds", f, {a, b, c})
end

-- UIListLayout: child sizes, padding, and parent UIPadding.
do
    local f = auto("List", Enum.AutomaticSize.XY)
    local a = kid(f, "A", 0, 0, 50, 20)
    local b = kid(f, "B", 0, 0, 70, 30)
    local l = Instance.new("UIListLayout")
    l.Padding = UDim.new(0, 10)
    l.Parent = f
    row("list", f, {a, b})
end
do
    local f = auto("ListPad", Enum.AutomaticSize.XY)
    local a = kid(f, "A", 0, 0, 40, 20)
    local b = kid(f, "B", 0, 0, 60, 30)
    local l = Instance.new("UIListLayout")
    l.Padding = UDim.new(0, 5)
    l.HorizontalAlignment = Enum.HorizontalAlignment.Center
    l.Parent = f
    local p = Instance.new("UIPadding")
    p.PaddingLeft = UDim.new(0, 7); p.PaddingRight = UDim.new(0, 3)
    p.PaddingTop = UDim.new(0, 9); p.PaddingBottom = UDim.new(0, 1)
    p.Parent = f
    row("list_pad", f, {a, b})
end
do
    local f = auto("ListWrap", Enum.AutomaticSize.XY)
    local a = kid(f, "A", 0, 0, 80, 20)
    local b = kid(f, "B", 0, 0, 80, 30)
    local l = Instance.new("UIListLayout")
    l.Wraps = true
    l.Padding = UDim.new(0, 5)
    l.Parent = f
    row("list_wrap", f, {a, b})
end

-- UIGridLayout in both fill directions, plus padding.
do
    local f = auto("GridH", Enum.AutomaticSize.XY)
    local a = kid(f, "A", 0, 0, 1, 1)
    local b = kid(f, "B", 0, 0, 1, 1)
    local c = kid(f, "C", 0, 0, 1, 1)
    local l = Instance.new("UIGridLayout")
    l.CellSize = UDim2.new(0, 60, 0, 40)
    l.CellPadding = UDim2.new(0, 4, 0, 6)
    l.FillDirection = Enum.FillDirection.Horizontal
    l.Parent = f
    row("grid_h", f, {a, b, c})
end
do
    local f = auto("GridV", Enum.AutomaticSize.XY)
    local a = kid(f, "A", 0, 0, 1, 1)
    local b = kid(f, "B", 0, 0, 1, 1)
    local c = kid(f, "C", 0, 0, 1, 1)
    local l = Instance.new("UIGridLayout")
    l.CellSize = UDim2.new(0, 60, 0, 40)
    l.CellPadding = UDim2.new(0, 4, 0, 6)
    l.FillDirection = Enum.FillDirection.Vertical
    l.Parent = f
    local p = Instance.new("UIPadding")
    p.PaddingLeft = UDim.new(0, 7); p.PaddingTop = UDim.new(0, 9)
    p.Parent = f
    row("grid_v", f, {a, b, c})
end

-- UISizeConstraint / UIAspectRatioConstraint / UIScale ordering.
do
    local f = auto("ConstMin", Enum.AutomaticSize.XY)
    local c = kid(f, "C", 0, 0, 100, 50)
    local k = Instance.new("UISizeConstraint")
    k.MinSize = Vector2.new(300, 200)
    k.MaxSize = Vector2.new(1000, 1000)
    k.Parent = f
    row("constraint_min", f, {c})
end
do
    local f = auto("ConstMax", Enum.AutomaticSize.XY)
    local c = kid(f, "C", 0, 0, 100, 50)
    local k = Instance.new("UISizeConstraint")
    k.MinSize = Vector2.new(0, 0)
    k.MaxSize = Vector2.new(50, 40)
    k.Parent = f
    row("constraint_max", f, {c})
end
do
    local f = auto("AspectFit", Enum.AutomaticSize.XY)
    local c = kid(f, "C", 0, 0, 100, 50)
    local a = Instance.new("UIAspectRatioConstraint")
    a.AspectRatio = 3
    a.AspectType = Enum.AspectType.FitWithinMaxSize
    a.DominantAxis = Enum.DominantAxis.Width
    a.Parent = f
    row("aspect_fit_xy", f, {c})
end
do
    local f = auto("AspectX", Enum.AutomaticSize.X)
    f.Size = UDim2.new(0, 0, 0, 50)
    local c = kid(f, "C", 0, 0, 100, 50)
    local a = Instance.new("UIAspectRatioConstraint")
    a.AspectRatio = 3
    a.AspectType = Enum.AspectType.FitWithinMaxSize
    a.DominantAxis = Enum.DominantAxis.Width
    a.Parent = f
    row("aspect_fit_x", f, {c})
end
do
    local f = auto("AspectScale", Enum.AutomaticSize.XY)
    local c = kid(f, "C", 0, 0, 100, 50)
    local a = Instance.new("UIAspectRatioConstraint")
    a.AspectRatio = 3
    a.AspectType = Enum.AspectType.ScaleWithParentSize
    a.DominantAxis = Enum.DominantAxis.Height
    a.Parent = f
    row("aspect_scale_xy", f, {c})
end
do
    local f = auto("Scale", Enum.AutomaticSize.XY)
    local c = kid(f, "C", 0, 0, 100, 50)
    local s = Instance.new("UIScale")
    s.Scale = 2
    s.Parent = f
    row("scale", f, {c})
end

-- Text measuring: plain auto, wrapped AutoY, TextScaled, and UIPadding.
do
    local f = auto("TextChild", Enum.AutomaticSize.XY)
    local t = Instance.new("TextLabel")
    t.Name = "T"
    t.Position = UDim2.new(0, 3, 0, 4)
    t.Size = UDim2.new(0, 0, 0, 0)
    t.BackgroundTransparency = 1
    t.Text = "Hello"
    t.TextSize = 20
    t.AutomaticSize = Enum.AutomaticSize.XY
    t.Parent = f
    local _ = t.AbsolutePosition
    row("text_child", f, {t})
    out(string.format("text_child bounds=%.1fx%.1f", t.TextBounds.X, t.TextBounds.Y))
end
do
    local f = auto("TextWrap", Enum.AutomaticSize.XY)
    local t = Instance.new("TextLabel")
    t.Name = "T"
    t.Size = UDim2.new(0, 100, 0, 0)
    t.BackgroundTransparency = 1
    t.Text = "Hello world this is a long line"
    t.TextSize = 20
    t.TextWrapped = true
    t.TextWrap = true
    t.AutomaticSize = Enum.AutomaticSize.Y
    t.Parent = f
    local _ = t.AbsolutePosition
    row("text_wrap", f, {t})
    out(string.format("text_wrap bounds=%.1fx%.1f", t.TextBounds.X, t.TextBounds.Y))
end
do
    -- A TextScaled label sized by AutomaticSize alone collapses to the native
    -- 1x1 floor.  The holder is intentionally not automatic; an automatic
    -- parent makes native 2.743 resolve TextScaled through its own circular
    -- 200x100 base and is not a stable AutomaticSize contract.
    local holder = frame("TextScaledHolder", 0, 0, 0, 0)
    local t = Instance.new("TextLabel")
    t.Name = "T"
    t.Size = UDim2.new(0, 0, 0, 0)
    t.BackgroundTransparency = 1
    t.Text = "Hello"
    t.TextSize = 20
    t.TextScaled = true
    t.AutomaticSize = Enum.AutomaticSize.XY
    t.Parent = holder
    local _ = t.AbsolutePosition
    out(string.format("text_scaled auto=%s value=%d size=%.1fx%.1f",
        tostring(t.AutomaticSize), t.AutomaticSize.Value,
        t.AbsoluteSize.X, t.AbsoluteSize.Y))
    out(string.format("text_scaled bounds=%.1fx%.1f", t.TextBounds.X, t.TextBounds.Y))
end
do
    local f = auto("TextPad", Enum.AutomaticSize.XY)
    local t = Instance.new("TextLabel")
    t.Name = "T"
    t.Size = UDim2.new(0, 0, 0, 0)
    t.BackgroundTransparency = 1
    t.Text = "Hello"
    t.TextSize = 20
    t.AutomaticSize = Enum.AutomaticSize.XY
    local p = Instance.new("UIPadding")
    p.PaddingLeft = UDim.new(0, 5); p.PaddingTop = UDim.new(0, 6)
    p.Parent = t
    t.Parent = f
    local _ = t.AbsolutePosition
    row("text_pad", f, {t})
    out(string.format("text_pad bounds=%.1fx%.1f", t.TextBounds.X, t.TextBounds.Y))
end

-- Default tables on GuiObject and the enum itself.
do
    local f = frame("Defaults", 0, 0, 0, 0)
    out(string.format("default_auto=%s", tostring(f.AutomaticSize)))
end
out("DONE")
