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
    -- A TextScaled label sized by AutomaticSize with a non-automatic parent
    -- collapses to the native 1x1 floor.  The nested automatic-parent case
    -- follows below.
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

-- TextScaled and TextWrapped labels nested in an automatic parent.  Native
-- 2.743's TextScaled fallback is circular, so these rows pin how the parent
-- and child resolve it rather than treating it as an excluded edge.
do
    local f = auto("TextScaledParent", Enum.AutomaticSize.XY)
    local t = Instance.new("TextLabel")
    t.Name = "T"
    t.Size = UDim2.new(0, 0, 0, 0)
    t.BackgroundTransparency = 1
    t.Text = "Hello"
    t.TextSize = 20
    t.TextScaled = true
    t.AutomaticSize = Enum.AutomaticSize.XY
    t.Parent = f
    local _ = t.AbsolutePosition
    row("text_scaled_parent", f, {t})
    out(string.format("text_scaled_parent bounds=%.1fx%.1f", t.TextBounds.X, t.TextBounds.Y))
end
do
    local f = auto("TextScaledParentLong", Enum.AutomaticSize.XY)
    local t = Instance.new("TextLabel")
    t.Name = "T"
    t.Size = UDim2.new(0, 0, 0, 0)
    t.BackgroundTransparency = 1
    t.Text = "Hello world this is a long line"
    t.TextSize = 20
    t.TextScaled = true
    t.AutomaticSize = Enum.AutomaticSize.XY
    t.Parent = f
    local _ = t.AbsolutePosition
    row("text_scaled_parent_long", f, {t})
    out(string.format("text_scaled_parent_long bounds=%.1fx%.1f", t.TextBounds.X, t.TextBounds.Y))
end
do
    local f = auto("TextScaledParentMultiline", Enum.AutomaticSize.XY)
    local t = Instance.new("TextLabel")
    t.Name = "T"
    t.Size = UDim2.new(0, 0, 0, 0)
    t.BackgroundTransparency = 1
    t.Text = "Hello\nHello"
    t.TextSize = 20
    t.TextScaled = true
    t.AutomaticSize = Enum.AutomaticSize.XY
    t.Parent = f
    local _ = t.AbsolutePosition
    row("text_scaled_parent_multiline", f, {t})
    out(string.format("text_scaled_parent_multiline bounds=%.1fx%.1f", t.TextBounds.X, t.TextBounds.Y))
end
do
    local f = auto("TextScaledParentEmpty", Enum.AutomaticSize.XY)
    local t = Instance.new("TextLabel")
    t.Name = "T"
    t.Size = UDim2.new(0, 0, 0, 0)
    t.BackgroundTransparency = 1
    t.Text = ""
    t.TextSize = 20
    t.TextScaled = true
    t.AutomaticSize = Enum.AutomaticSize.XY
    t.Parent = f
    local _ = t.AbsolutePosition
    row("text_scaled_parent_empty", f, {t})
    out(string.format("text_scaled_parent_empty bounds=%.1fx%.1f", t.TextBounds.X, t.TextBounds.Y))
end
do
    local f = auto("TextScaledParentPad", Enum.AutomaticSize.XY)
    local t = Instance.new("TextLabel")
    t.Name = "T"
    t.Size = UDim2.new(0, 0, 0, 0)
    t.BackgroundTransparency = 1
    t.Text = "Hello"
    t.TextSize = 20
    t.TextScaled = true
    t.AutomaticSize = Enum.AutomaticSize.XY
    local p = Instance.new("UIPadding")
    p.PaddingLeft = UDim.new(0, 5); p.PaddingTop = UDim.new(0, 5)
    p.Parent = t
    t.Parent = f
    local _ = t.AbsolutePosition
    row("text_scaled_parent_pad", f, {t})
    out(string.format("text_scaled_parent_pad bounds=%.1fx%.1f", t.TextBounds.X, t.TextBounds.Y))
end
do
    local f = auto("TextScaledParentExplicit", Enum.AutomaticSize.XY)
    local t = Instance.new("TextLabel")
    t.Name = "T"
    t.Size = UDim2.new(0, 50, 0, 20)
    t.BackgroundTransparency = 1
    t.Text = "Hello"
    t.TextSize = 20
    t.TextScaled = true
    t.AutomaticSize = Enum.AutomaticSize.XY
    t.Parent = f
    local _ = t.AbsolutePosition
    row("text_scaled_parent_explicit", f, {t})
    out(string.format("text_scaled_parent_explicit bounds=%.1fx%.1f", t.TextBounds.X, t.TextBounds.Y))
end
do
    local f = auto("TextWrappedParent", Enum.AutomaticSize.XY)
    local t = Instance.new("TextLabel")
    t.Name = "T"
    t.Size = UDim2.new(0, 100, 0, 0)
    t.BackgroundTransparency = 1
    t.Text = "Hello world"
    t.TextSize = 20
    t.TextWrapped = true
    t.TextWrap = true
    t.AutomaticSize = Enum.AutomaticSize.XY
    t.Parent = f
    local _ = t.AbsolutePosition
    row("text_wrapped_parent", f, {t})
    out(string.format("text_wrapped_parent bounds=%.1fx%.1f", t.TextBounds.X, t.TextBounds.Y))
end

-- Default tables on GuiObject and the enum itself.
do
    local f = frame("Defaults", 0, 0, 0, 0)
    out(string.format("default_auto=%s", tostring(f.AutomaticSize)))
end

-- UITextSizeConstraint + TextScaled, measured against native 2.743.
local function utsc_label(tag, cls, w, h, text)
    local f = frame("UTSC_" .. tag, 0, 0, w, h)
    local t = Instance.new(cls or "TextLabel")
    t.Name = "T"
    t.Size = UDim2.new(1, 0, 1, 0)
    t.BackgroundTransparency = 1
    t.Text = text or "Hello"
    t.TextSize = 20
    t.TextScaled = true
    t.Parent = f
    local _ = t.AbsolutePosition
    return t
end
local function utsc_constraint(t, lo, hi)
    local c = Instance.new("UITextSizeConstraint")
    if lo ~= nil then c.MinTextSize = lo end
    if hi ~= nil then c.MaxTextSize = hi end
    c.Parent = t
    local _ = t.AbsolutePosition
    return c
end
local function utsc_info(t)
    local _ = t.AbsolutePosition
    return string.format("bounds=%.1fx%.1f fits=%s size=%.1fx%.1f hx=%s vy=%s",
        t.TextBounds.X, t.TextBounds.Y, tostring(t.TextFits),
        t.AbsoluteSize.X, t.AbsoluteSize.Y,
        tostring(t.TextXAlignment), tostring(t.TextYAlignment))
end
local function utsc_print(tag, t, c)
    out(string.format("utsc_%s %s min=%s max=%s", tag, utsc_info(t),
        c and tostring(c.MinTextSize) or "nil",
        c and tostring(c.MaxTextSize) or "nil"))
end
local function utsc_auto(tag, text)
    local f = auto("UTSCauto_" .. tag, Enum.AutomaticSize.XY)
    local t = Instance.new("TextLabel")
    t.Name = "T"
    t.Size = UDim2.new(0, 0, 0, 0)
    t.BackgroundTransparency = 1
    t.Text = text or "Hello"
    t.TextSize = 20
    t.TextScaled = true
    t.AutomaticSize = Enum.AutomaticSize.XY
    t.Parent = f
    local _ = t.AbsolutePosition
    return t
end
do
    local c = Instance.new("UITextSizeConstraint")
    out(string.format("utsc_defaults min=%d max=%d", c.MinTextSize, c.MaxTextSize))
end
do utsc_print("natural", utsc_label("natural", "TextLabel", 400, 200)) end
do
    local t = utsc_label("inrange", "TextLabel", 100, 20)
    utsc_print("inrange", t, utsc_constraint(t, 10, 20))
end
do
    local t = utsc_label("max20", "TextLabel", 400, 200)
    utsc_print("max20", t, utsc_constraint(t, 1, 20))
end
do
    local t = utsc_label("min40", "TextLabel", 100, 20)
    utsc_print("min40", t, utsc_constraint(t, 40, 100))
end
do
    local t = utsc_label("equal20", "TextLabel", 400, 200)
    utsc_print("equal20", t, utsc_constraint(t, 20, 20))
end
do
    local t = utsc_label("minwins", "TextLabel", 400, 200)
    utsc_print("minwins", t, utsc_constraint(t, 30, 10))
end
do
    local t = utsc_label("min1", "TextLabel", 400, 200)
    utsc_print("min1", t, utsc_constraint(t, 0, 0))
end
do
    local t = utsc_label("maxneg", "TextLabel", 400, 200)
    utsc_print("maxneg", t, utsc_constraint(t, 20, -5))
end
do
    local t = utsc_label("wrapped", "TextLabel", 100, 20)
    t.TextWrapped = true
    utsc_print("wrapped", t, utsc_constraint(t, 40, 100))
end
do
    local left = utsc_label("align_left", "TextLabel", 100, 20)
    left.TextXAlignment = Enum.TextXAlignment.Left
    left.TextYAlignment = Enum.TextYAlignment.Top
    utsc_constraint(left, 40, 100)
    local right = utsc_label("align_right", "TextLabel", 100, 20)
    right.TextXAlignment = Enum.TextXAlignment.Right
    right.TextYAlignment = Enum.TextYAlignment.Bottom
    utsc_constraint(right, 40, 100)
    out(string.format("utsc_align left=%s right=%s", utsc_info(left), utsc_info(right)))
end
do
    local b = utsc_label("button", "TextButton", 100, 20)
    utsc_constraint(b, 40, 100)
    local x = utsc_label("textbox", "TextBox", 100, 20)
    utsc_constraint(x, 40, 100)
    out(string.format("utsc_classes button=%s textbox=%s", utsc_info(b), utsc_info(x)))
end
do
    local t = utsc_label("noscale", "TextLabel", 100, 20)
    t.TextScaled = false
    t.TextWrapped = true
    utsc_print("noscale", t, utsc_constraint(t, 40, 100))
end
do
    local t = utsc_label("order", "TextLabel", 400, 200)
    local function brief()
        local _ = t.AbsolutePosition
        return string.format("%.1fx%.1f/%s", t.TextBounds.X, t.TextBounds.Y,
            tostring(t.TextFits))
    end
    local before = brief()
    local c = utsc_constraint(t, 1, 20)
    local max20 = brief()
    c.MinTextSize = 30; c.MaxTextSize = 30
    local changed30 = brief()
    c:Destroy()
    local removed = brief()
    t.TextScaled = false
    local unscaled = brief()
    t.TextScaled = true
    local rescaled = brief()
    utsc_constraint(t, 40, 40)
    out(string.format("utsc_order before=%s max20=%s changed30=%s removed=%s unscaled=%s rescaled=%s readd40=%s",
        before, max20, changed30, removed, unscaled, rescaled, brief()))
end
do
    local t = utsc_label("prescale", "TextLabel", 400, 200)
    t.TextScaled = false
    utsc_constraint(t, 1, 20)
    local function brief()
        local _ = t.AbsolutePosition
        return string.format("%.1fx%.1f/%s", t.TextBounds.X, t.TextBounds.Y,
            tostring(t.TextFits))
    end
    local unscaled = brief()
    t.TextScaled = true
    out(string.format("utsc_prescale unscaled=%s scaled=%s", unscaled, brief()))
end
do
    local natural = utsc_auto("natural")
    local max20 = utsc_auto("max20")
    utsc_constraint(max20, 1, 20)
    local min40 = utsc_auto("min40")
    utsc_constraint(min40, 40, 100)
    local min1 = utsc_auto("min1")
    utsc_constraint(min1, 0, 0)
    local multi = utsc_auto("multiline", "Hello\nHello")
    utsc_constraint(multi, 1, 20)
    out(string.format("utsc_auto natural=%s max20=%s min40=%s min1=%s multiline=%s",
        utsc_info(natural), utsc_info(max20), utsc_info(min40), utsc_info(min1),
        utsc_info(multi)))
end
do
    local t = utsc_auto("noscale_min40")
    t.TextScaled = false
    utsc_constraint(t, 40, 100)
    out(string.format("utsc_auto_noscale %s", utsc_info(t)))
end
do
    local t = utsc_label("couple", "TextLabel", 400, 200)
    local initial = tostring(t.TextScaled) .. "/" .. tostring(t.TextWrapped)
    t.TextScaled = true
    local scaled = tostring(t.TextScaled) .. "/" .. tostring(t.TextWrapped)
    t.TextWrapped = false
    local cleared = tostring(t.TextScaled) .. "/" .. tostring(t.TextWrapped)
    out(string.format("utsc_couple initial=%s scaled=%s cleared=%s", initial, scaled, cleared))
end
out("DONE")
