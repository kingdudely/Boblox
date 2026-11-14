-- ui_constraints_probe.lua — native/Boblox comparison for GUI scale and size
-- constraints.  Prints frame-relative geometry so the host topbar inset does
-- not affect the comparison.
if _G.__BOBLOX_UI_CONSTRAINTS_PROBE then return end
_G.__BOBLOX_UI_CONSTRAINTS_PROBE = true
local Players = game:GetService("Players")
local pg = Players.LocalPlayer:WaitForChild("PlayerGui", 15)
local function out(s) print("UIGEOM " .. s) end
if not pg then out("no PlayerGui") return end
local old = pg:FindFirstChild("UiConstraintsProbe")
if old then old:Destroy() end
local screen = Instance.new("ScreenGui")
screen.Name = "UiConstraintsProbe"
screen.IgnoreGuiInset = true
screen.ResetOnSpawn = false
screen.DisplayOrder = 1004
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
local function child(parent, name, x, y, w, h)
    local f = Instance.new("Frame")
    f.Name = name
    f.Position = UDim2.new(0, x, 0, y)
    f.Size = UDim2.new(0, w, 0, h)
    f.BackgroundTransparency = 1
    f.Parent = parent
    return f
end
local function dump(tag, root, kids)
    out(string.format("%s root=%.1fx%.1f", tag,
        root.AbsoluteSize.X, root.AbsoluteSize.Y))
    for _, c in ipairs(kids) do
        out(string.format("%s %s=%.1f,%.1f %.1fx%.1f", tag, c.Name,
            c.AbsolutePosition.X - root.AbsolutePosition.X,
            c.AbsolutePosition.Y - root.AbsolutePosition.Y,
            c.AbsoluteSize.X, c.AbsoluteSize.Y))
    end
end

-- UIScale scales the object's own size and its descendants, but not its own
-- top-left position.
do
    local f = frame("Scale", 0, 0, 400, 300)
    local c = child(f, "S", 10, 20, 100, 50)
    local s = Instance.new("UIScale")
    s.Scale = 2
    s.Parent = f
    dump("scale", f, {c})
end

do
    local f = frame("ScaleHalf", 0, 350, 400, 300)
    local c = child(f, "S", 10, 20, 100, 50)
    local s = Instance.new("UIScale")
    s.Scale = 0.5
    s.Parent = f
    dump("scalehalf", f, {c})
end

do
    local f = frame("ScalePos", 100, 100, 100, 50)
    local c = child(f, "S", 10, 20, 50, 25)
    local s = Instance.new("UIScale")
    s.Scale = 2
    s.Parent = f
    dump("scalepos", f, {c})
end

-- Nested UIScale multiplies for the nested object's children.
do
    local f = frame("ScaleNest", 0, 700, 400, 300)
    local m = child(f, "M", 10, 0, 100, 100)
    local c = child(m, "C", 10, 20, 50, 25)
    local s1 = Instance.new("UIScale"); s1.Scale = 2; s1.Parent = f
    local s2 = Instance.new("UIScale"); s2.Scale = 0.5; s2.Parent = m
    dump("scalenest", f, {m, c})
end

-- Constraint before own scale: min 300x200 then UIScale 2 -> 600x400.
do
    local f = frame("ScaleCon", 0, 1050, 100, 50)
    local c = child(f, "C", 10, 20, 100, 50)
    local s = Instance.new("UIScale"); s.Scale = 2; s.Parent = f
    local k = Instance.new("UISizeConstraint")
    k.MinSize = Vector2.new(300, 200)
    k.MaxSize = Vector2.new(350, 250)
    k.Parent = f
    dump("scalecon", f, {c})
end

-- Aspect after constraint, then own scale.
do
    local f = frame("ScaleAspect", 0, 1400, 100, 50)
    local c = child(f, "C", 10, 20, 100, 50)
    local s = Instance.new("UIScale"); s.Scale = 2; s.Parent = f
    local a = Instance.new("UIAspectRatioConstraint")
    a.AspectRatio = 2
    a.AspectType = Enum.AspectType.FitWithinMaxSize
    a.DominantAxis = Enum.DominantAxis.Width
    a.Parent = f
    dump("scaleaspect", f, {c})
end

do
    local f = frame("ConAspect", 0, 1750, 100, 50)
    local c = child(f, "C", 10, 20, 100, 50)
    local k = Instance.new("UISizeConstraint")
    k.MinSize = Vector2.new(300, 100)
    k.MaxSize = Vector2.new(350, 250)
    k.Parent = f
    local a = Instance.new("UIAspectRatioConstraint")
    a.AspectRatio = 2
    a.AspectType = Enum.AspectType.FitWithinMaxSize
    a.DominantAxis = Enum.DominantAxis.Width
    a.Parent = f
    dump("conaspect", f, {c})
end

-- UIAspectRatioConstraint's two modes.
do
    local f = frame("AspectFit", 450, 0, 200, 300)
    local a = Instance.new("UIAspectRatioConstraint")
    a.AspectRatio = 2
    a.AspectType = Enum.AspectType.FitWithinMaxSize
    a.DominantAxis = Enum.DominantAxis.Width
    a.Parent = f
    dump("aspectfit", f, {})
end

do
    local f = frame("AspectScale", 450, 350, 200, 300)
    local a = Instance.new("UIAspectRatioConstraint")
    a.AspectRatio = 2
    a.AspectType = Enum.AspectType.ScaleWithParentSize
    a.DominantAxis = Enum.DominantAxis.Height
    a.Parent = f
    dump("aspectscale", f, {})
end

-- Padding offsets are in the scaled child coordinate space.
do
    local f = frame("ScalePad", 450, 700, 200, 100)
    local c = child(f, "C", 0, 0, 100, 50)
    local s = Instance.new("UIScale"); s.Scale = 2; s.Parent = f
    local p = Instance.new("UIPadding")
    p.PaddingLeft = UDim.new(0, 10)
    p.PaddingTop = UDim.new(0, 10)
    p.Parent = f
    dump("scalepad", f, {c})
end

-- Zero scale collapses the subtree.
do
    local f = frame("ScaleZero", 450, 1050, 100, 50)
    local s = Instance.new("UIScale"); s.Scale = 0; s.Parent = f
    dump("scalezero", f, {})
end

-- Constraint defaults are unbounded, not the generated zeroes.
do
    local f = frame("SizeConDefault", 450, 1400, 100, 50)
    local k = Instance.new("UISizeConstraint")
    k.Parent = f
    out(string.format("sizecondefault min=%.1f,%.1f max=%.1f,%.1f",
        k.MinSize.X, k.MinSize.Y, k.MaxSize.X, k.MaxSize.Y))
    dump("sizecondefault", f, {})
end

do
    local a = Instance.new("UIAspectRatioConstraint")
    out(string.format("aspectdefault ratio=%.3f type=%s axis=%s",
        a.AspectRatio, tostring(a.AspectType), tostring(a.DominantAxis)))
end

do
    local s = Instance.new("UIScale")
    out(string.format("scaledefault scale=%.1f", s.Scale))
end

out("DONE")
