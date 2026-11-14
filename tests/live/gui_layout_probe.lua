-- gui_layout_probe.lua — native/Boblox comparison for GUI layout components.
--
-- Prints positions relative to each probe Frame so a host topbar/inset
-- difference does not hide the layout result.  Run with:
--   Boblox: --exec tests/live/gui_layout_probe.lua
--   native: ~/Xecutor/tests/xecutor_sess.sh run ... 'GUI_LAYOUT DONE'
local Players = game:GetService("Players")
local player = Players.LocalPlayer
local pg = player:WaitForChild("PlayerGui", 15)
local function out(s) print("GUI_LAYOUT " .. s) end
if not pg then out("no PlayerGui") return end

local old = pg:FindFirstChild("GuiLayoutProbe")
if old then old:Destroy() end
local screen = Instance.new("ScreenGui")
screen.Name = "GuiLayoutProbe"
screen.IgnoreGuiInset = true
screen.ResetOnSpawn = false
screen.DisplayOrder = 1000
screen.Parent = pg

local function frame(name, y, h)
    local f = Instance.new("Frame")
    f.Name = name
    f.Position = UDim2.new(0, 0, 0, y)
    f.Size = UDim2.new(0, 400, 0, h)
    f.BackgroundTransparency = 1
    f.Parent = screen
    return f
end

local function label(parent, name, w, h, order)
    local l = Instance.new("TextLabel")
    l.Name = name
    l.Size = UDim2.new(0, w, 0, h)
    l.LayoutOrder = order or 0
    l.BackgroundTransparency = 1
    l.Text = name
    l.Parent = parent
    return l
end

local function dump(tag, root, kids)
    out(tag .. " root=" .. string.format("%.1fx%.1f",
        root.AbsoluteSize.X, root.AbsoluteSize.Y))
    for _, c in ipairs(kids) do
        out(tag .. " " .. c.Name .. "=" .. string.format("%.1f,%.1f %.1fx%.1f",
            c.AbsolutePosition.X - root.AbsolutePosition.X,
            c.AbsolutePosition.Y - root.AbsolutePosition.Y,
            c.AbsoluteSize.X, c.AbsoluteSize.Y))
    end
end

-- Native defaults.
local defaults = Instance.new("UIListLayout")
out(string.format("defaults list fill=%s h=%s v=%s sort=%s wraps=%s pad=%s",
    tostring(defaults.FillDirection), tostring(defaults.HorizontalAlignment),
    tostring(defaults.VerticalAlignment), tostring(defaults.SortOrder),
    tostring(defaults.Wraps), tostring(defaults.Padding)))
local gd = Instance.new("UIGridLayout")
out(string.format("defaults grid cell=%s pad=%s fill=%s max=%d start=%s h=%s v=%s sort=%s",
    tostring(gd.CellSize), tostring(gd.CellPadding),
    tostring(gd.FillDirection), gd.FillDirectionMaxCells,
    tostring(gd.StartCorner), tostring(gd.HorizontalAlignment),
    tostring(gd.VerticalAlignment), tostring(gd.SortOrder)))

-- Vertical list, left/top, 10 px padding, LayoutOrder.
do
    local f = frame("ListV", 0, 300)
    local a = label(f, "A", 100, 40, 2)
    local b = label(f, "B", 50, 30, 1)
    local c = label(f, "C", 200, 20, 3)
    local l = Instance.new("UIListLayout")
    l.FillDirection = Enum.FillDirection.Vertical
    l.Padding = UDim.new(0, 10)
    l.HorizontalAlignment = Enum.HorizontalAlignment.Left
    l.VerticalAlignment = Enum.VerticalAlignment.Top
    l.SortOrder = Enum.SortOrder.LayoutOrder
    l.Parent = f
    out("listv content=" .. string.format("%.1fx%.1f",
        l.AbsoluteContentSize.X, l.AbsoluteContentSize.Y))
    dump("listv", f, {a, b, c})
end

-- Horizontal list, center/middle, name order.
do
    local f = frame("ListH", 320, 100)
    local a = label(f, "H2", 60, 40)
    local b = label(f, "H1", 80, 20)
    local c = label(f, "H3", 40, 30)
    local l = Instance.new("UIListLayout")
    l.FillDirection = Enum.FillDirection.Horizontal
    l.Padding = UDim.new(0, 7)
    l.HorizontalAlignment = Enum.HorizontalAlignment.Center
    l.VerticalAlignment = Enum.VerticalAlignment.Center
    l.SortOrder = Enum.SortOrder.Name
    l.Parent = f
    out("listh content=" .. string.format("%.1fx%.1f",
        l.AbsoluteContentSize.X, l.AbsoluteContentSize.Y))
    dump("listh", f, {a, b, c})
end

-- Grid, three columns, 100x50 cells, 10 px padding.
do
    local f = frame("Grid", 440, 300)
    local kids = {}
    for i = 1, 5 do
        kids[i] = label(f, "G" .. i, 10 + i, 10 + i, i)
    end
    local g = Instance.new("UIGridLayout")
    g.CellSize = UDim2.new(0, 100, 0, 50)
    g.CellPadding = UDim2.new(0, 10, 0, 10)
    g.FillDirection = Enum.FillDirection.Horizontal
    g.FillDirectionMaxCells = 3
    g.StartCorner = Enum.StartCorner.TopLeft
    g.HorizontalAlignment = Enum.HorizontalAlignment.Left
    g.VerticalAlignment = Enum.VerticalAlignment.Top
    g.SortOrder = Enum.SortOrder.LayoutOrder
    g.Parent = f
    out("grid content=" .. string.format("%.1fx%.1f",
        g.AbsoluteContentSize.X, g.AbsoluteContentSize.Y))
    out("grid count=" .. string.format("%.1fx%.1f",
        g.AbsoluteCellCount.X, g.AbsoluteCellCount.Y))
    dump("grid", f, kids)
end

-- UIPadding around a list: 10 left/right, 20 top/bottom.
do
    local f = frame("Pad", 0, 200)
    f.Position = UDim2.new(0, 420, 0, 0)
    local a = label(f, "P1", 200, 30)
    local b = label(f, "P2", 400, 20)
    a.Size = UDim2.new(0.5, 0, 0, 30)
    b.Size = UDim2.new(1, 0, 0, 20)
    local p = Instance.new("UIPadding")
    p.PaddingLeft = UDim.new(0, 10)
    p.PaddingRight = UDim.new(0, 10)
    p.PaddingTop = UDim.new(0, 20)
    p.PaddingBottom = UDim.new(0, 20)
    p.Parent = f
    local l = Instance.new("UIListLayout")
    l.Padding = UDim.new(0, 5)
    l.Parent = f
    out("pad content=" .. string.format("%.1fx%.1f",
        l.AbsoluteContentSize.X, l.AbsoluteContentSize.Y))
    dump("pad", f, {a, b})
end

-- Vertical grid, two rows, bottom-right start.
do
    local f = frame("GridV", 760, 300)
    local kids = {}
    for i = 1, 5 do
        kids[i] = label(f, "V" .. i, 10 + i, 10 + i, i)
    end
    local g = Instance.new("UIGridLayout")
    g.CellSize = UDim2.new(0, 100, 0, 50)
    g.CellPadding = UDim2.new(0, 10, 0, 10)
    g.FillDirection = Enum.FillDirection.Vertical
    g.FillDirectionMaxCells = 2
    g.StartCorner = Enum.StartCorner.BottomRight
    g.SortOrder = Enum.SortOrder.LayoutOrder
    g.Parent = f
    out("gridv content=" .. string.format("%.1fx%.1f",
        g.AbsoluteContentSize.X, g.AbsoluteContentSize.Y))
    out("gridv count=" .. string.format("%.1fx%.1f",
        g.AbsoluteCellCount.X, g.AbsoluteCellCount.Y))
    dump("gridv", f, kids)
end

-- Horizontal grid with automatic columns (FillDirectionMaxCells = 0).
do
    local f = frame("GridAuto", 300, 300)
    f.Position = UDim2.new(0, 420, 0, 300)
    local kids = {}
    for i = 1, 5 do
        kids[i] = label(f, "A" .. i, 10, 10, i)
    end
    local g = Instance.new("UIGridLayout")
    g.CellSize = UDim2.new(0, 90, 0, 40)
    g.CellPadding = UDim2.new(0, 10, 0, 10)
    g.FillDirectionMaxCells = 0
    g.SortOrder = Enum.SortOrder.LayoutOrder
    g.Parent = f
    out("gridauto content=" .. string.format("%.1fx%.1f",
        g.AbsoluteContentSize.X, g.AbsoluteContentSize.Y))
    out("gridauto count=" .. string.format("%.1fx%.1f",
        g.AbsoluteCellCount.X, g.AbsoluteCellCount.Y))
    dump("gridauto", f, kids)
end

-- FillDirectionMaxCells is a cap: the content still fits only three columns.
do
    local f = frame("GridMax", 0, 300)
    f.Position = UDim2.new(0, 840, 0, 300)
    local kids = {}
    for i = 1, 5 do
        kids[i] = label(f, "M" .. i, 10, 10, i)
    end
    local g = Instance.new("UIGridLayout")
    g.CellSize = UDim2.new(0, 100, 0, 50)
    g.CellPadding = UDim2.new(0, 10, 0, 10)
    g.FillDirectionMaxCells = 10
    g.SortOrder = Enum.SortOrder.LayoutOrder
    g.Parent = f
    out("gridmax content=" .. string.format("%.1fx%.1f",
        g.AbsoluteContentSize.X, g.AbsoluteContentSize.Y))
    out("gridmax count=" .. string.format("%.1fx%.1f",
        g.AbsoluteCellCount.X, g.AbsoluteCellCount.Y))
    dump("gridmax", f, kids)
end

-- Wrapped horizontal list.
do
    local f = frame("Wrap", 1080, 200)
    local kids = {
        label(f, "W1", 150, 40, 1),
        label(f, "W2", 150, 30, 2),
        label(f, "W3", 150, 20, 3),
        label(f, "W4", 100, 50, 4),
    }
    local l = Instance.new("UIListLayout")
    l.FillDirection = Enum.FillDirection.Horizontal
    l.Padding = UDim.new(0, 10)
    l.Wraps = true
    l.SortOrder = Enum.SortOrder.LayoutOrder
    l.Parent = f
    out("wrap content=" .. string.format("%.1fx%.1f",
        l.AbsoluteContentSize.X, l.AbsoluteContentSize.Y))
    dump("wrap", f, kids)
end

-- Horizontal main-axis flex: equal extra width per item, not proportional.
do
    local f = frame("HFill", 1280, 200)
    local kids = {
        label(f, "F1", 50, 20, 1),
        label(f, "F2", 100, 20, 2),
        label(f, "F3", 150, 20, 3),
    }
    local l = Instance.new("UIListLayout")
    l.FillDirection = Enum.FillDirection.Horizontal
    l.HorizontalFlex = Enum.UIFlexAlignment.Fill
    l.SortOrder = Enum.SortOrder.LayoutOrder
    l.Parent = f
    out("hfill content=" .. string.format("%.1fx%.1f",
        l.AbsoluteContentSize.X, l.AbsoluteContentSize.Y))
    dump("hfill", f, kids)
end

-- Vertical main-axis flex: every child grows by the same remainder.
do
    local f = frame("VFill", 1500, 300)
    local kids = {
        label(f, "F1", 50, 20, 1),
        label(f, "F2", 100, 30, 2),
        label(f, "F3", 150, 40, 3),
    }
    local l = Instance.new("UIListLayout")
    l.FillDirection = Enum.FillDirection.Vertical
    l.VerticalFlex = Enum.UIFlexAlignment.Fill
    l.SortOrder = Enum.SortOrder.LayoutOrder
    l.Parent = f
    out("vfill content=" .. string.format("%.1fx%.1f",
        l.AbsoluteContentSize.X, l.AbsoluteContentSize.Y))
    dump("vfill", f, kids)
end

-- Wrapped line alignment: Center offsets each child inside its line box.
do
    local f = frame("LAlign", 1800, 200)
    local kids = {
        label(f, "W1", 150, 40, 1),
        label(f, "W2", 150, 30, 2),
        label(f, "W3", 150, 20, 3),
        label(f, "W4", 100, 50, 4),
    }
    local l = Instance.new("UIListLayout")
    l.FillDirection = Enum.FillDirection.Horizontal
    l.Padding = UDim.new(0, 10)
    l.Wraps = true
    l.ItemLineAlignment = Enum.ItemLineAlignment.Center
    l.SortOrder = Enum.SortOrder.LayoutOrder
    l.Parent = f
    out("lalign content=" .. string.format("%.1fx%.1f",
        l.AbsoluteContentSize.X, l.AbsoluteContentSize.Y))
    dump("lalign", f, kids)
end

-- Main-axis flex with wrapping: each line fills the content width.
do
    local f = frame("FWrapMain", 2100, 200)
    local kids = {
        label(f, "W1", 150, 40, 1),
        label(f, "W2", 150, 30, 2),
        label(f, "W3", 150, 20, 3),
        label(f, "W4", 150, 50, 4),
    }
    local l = Instance.new("UIListLayout")
    l.FillDirection = Enum.FillDirection.Horizontal
    l.Padding = UDim.new(0, 10)
    l.Wraps = true
    l.HorizontalFlex = Enum.UIFlexAlignment.Fill
    l.SortOrder = Enum.SortOrder.LayoutOrder
    l.Parent = f
    out("fwmain content=" .. string.format("%.1fx%.1f",
        l.AbsoluteContentSize.X, l.AbsoluteContentSize.Y))
    dump("fwmain", f, kids)
end

-- Cross-axis flex with wrapping: free cross space is shared by the lines.
do
    local f = frame("FWrapCross", 2320, 200)
    local kids = {
        label(f, "W1", 150, 40, 1),
        label(f, "W2", 150, 30, 2),
        label(f, "W3", 150, 20, 3),
        label(f, "W4", 150, 50, 4),
    }
    local l = Instance.new("UIListLayout")
    l.FillDirection = Enum.FillDirection.Horizontal
    l.Padding = UDim.new(0, 10)
    l.Wraps = true
    l.VerticalFlex = Enum.UIFlexAlignment.Fill
    l.SortOrder = Enum.SortOrder.LayoutOrder
    l.Parent = f
    out("fwcross content=" .. string.format("%.1fx%.1f",
        l.AbsoluteContentSize.X, l.AbsoluteContentSize.Y))
    dump("fwcross", f, kids)
end

-- Space modes ignore Padding when distributing free main-axis space.
do
    local f = frame("PAround", 2540, 300)
    local kids = {
        label(f, "P1", 50, 20, 1),
        label(f, "P2", 100, 30, 2),
        label(f, "P3", 150, 40, 3),
    }
    local l = Instance.new("UIListLayout")
    l.FillDirection = Enum.FillDirection.Vertical
    l.Padding = UDim.new(0, 10)
    l.VerticalFlex = Enum.UIFlexAlignment.SpaceAround
    l.SortOrder = Enum.SortOrder.LayoutOrder
    l.Parent = f
    out("paround content=" .. string.format("%.1fx%.1f",
        l.AbsoluteContentSize.X, l.AbsoluteContentSize.Y))
    dump("paround", f, kids)
end

-- Fill shrinks proportionally when the content is too small.
do
    local f = frame("ShrinkFill", 2860, 100)
    local kids = {
        label(f, "S1", 50, 20, 1),
        label(f, "S2", 100, 30, 2),
        label(f, "S3", 150, 40, 3),
    }
    local l = Instance.new("UIListLayout")
    l.FillDirection = Enum.FillDirection.Vertical
    l.Padding = UDim.new(0, 10)
    l.VerticalFlex = Enum.UIFlexAlignment.Fill
    l.SortOrder = Enum.SortOrder.LayoutOrder
    l.Parent = f
    out("shrink content=" .. string.format("%.1fx%.1f",
        l.AbsoluteContentSize.X, l.AbsoluteContentSize.Y))
    dump("shrink", f, kids)
end

-- Custom sort uses the script comparator (descending names here).
do
    local f = frame("Custom", 3200, 200)
    local kids = {
        label(f, "C", 50, 20, 1),
        label(f, "A", 50, 20, 2),
        label(f, "B", 50, 20, 3),
    }
    local l = Instance.new("UIListLayout")
    l.SortOrder = Enum.SortOrder.Custom
    l.Parent = f
    l:SetCustomSortFunction(function(x, y)
        return x.Name > y.Name
    end)
    out("custom content=" .. string.format("%.1fx%.1f",
        l.AbsoluteContentSize.X, l.AbsoluteContentSize.Y))
    dump("custom", f, kids)
end

-- SpaceAround with too little room centers the overflowing block.
do
    local f = frame("ShrinkAround", 2980, 100)
    local kids = {
        label(f, "S1", 50, 20, 1),
        label(f, "S2", 100, 30, 2),
        label(f, "S3", 150, 40, 3),
    }
    local l = Instance.new("UIListLayout")
    l.FillDirection = Enum.FillDirection.Vertical
    l.Padding = UDim.new(0, 10)
    l.VerticalFlex = Enum.UIFlexAlignment.SpaceAround
    l.SortOrder = Enum.SortOrder.LayoutOrder
    l.Parent = f
    out("shrinkaround content=" .. string.format("%.1fx%.1f",
        l.AbsoluteContentSize.X, l.AbsoluteContentSize.Y))
    dump("shrinkaround", f, kids)
end

out("DONE")
