// gui_layout.cpp — see gui_layout.h.
#include "gui/gui_layout.h"

#include "instance/instance.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace rbxclient {

namespace {

using rbx::Color3;
using rbx::Instance;
using rbx::UDim2;
using rbx::Variant;
using rbx::Vector2;

constexpr double kPi = 3.14159265358979323846;

bool get(const Instance* inst, const char* name, Variant* out) {
    return inst && inst->get_prop(name, *out);
}

bool has(const Instance* inst, const char* name) {
    return inst && inst->has_prop_override(name);
}

double num(const Instance* inst, const char* name, double fallback) {
    if (!has(inst, name))
        return fallback;
    Variant v;
    if (!get(inst, name, &v))
        return fallback;
    if (const auto* d = std::get_if<double>(&v))
        return *d;
    if (const auto* i = std::get_if<int64_t>(&v))
        return double(*i);
    return fallback;
}

int integer(const Instance* inst, const char* name, int fallback) {
    Variant v;
    if (!get(inst, name, &v))
        return fallback;
    if (const auto* i = std::get_if<int64_t>(&v))
        return int(*i);
    if (const auto* d = std::get_if<double>(&v))
        return int(*d);
    return fallback;
}

bool boolean(const Instance* inst, const char* name, bool fallback) {
    if (!has(inst, name))
        return fallback;
    Variant v;
    if (!get(inst, name, &v))
        return fallback;
    if (const auto* b = std::get_if<bool>(&v))
        return *b;
    return fallback;
}

Color3 color(const Instance* inst, const char* name, Color3 fallback) {
    if (!has(inst, name))
        return fallback;
    Variant v;
    if (!get(inst, name, &v))
        return fallback;
    if (const auto* c = std::get_if<Color3>(&v))
        return *c;
    return fallback;
}

double transparency(const Instance* inst, const char* name) {
    return std::clamp(num(inst, name, 0.0), 0.0, 1.0);
}

UDim2 udim2(const Instance* inst, const char* name) {
    Variant v;
    if (!get(inst, name, &v))
        return UDim2{};
    if (const auto* u = std::get_if<UDim2>(&v))
        return *u;
    return UDim2{};
}

Vector2 vector2(const Instance* inst, const char* name) {
    Variant v;
    if (!get(inst, name, &v))
        return Vector2{};
    if (const auto* p = std::get_if<Vector2>(&v))
        return *p;
    return Vector2{};
}

std::string text_of(const Instance* inst, const char* name) {
    Variant v;
    if (!get(inst, name, &v))
        return {};
    if (const auto* s = std::get_if<std::string>(&v))
        return *s;
    return {};
}

int enum_value(const Instance* inst, const char* name, int fallback) {
    if (!has(inst, name))
        return fallback;
    Variant v;
    if (!get(inst, name, &v))
        return fallback;
    if (const auto* e = std::get_if<rbx::EnumItem>(&v))
        return e->value;
    return fallback;
}

std::string strip_rich_text(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool tag = false;
    for (char c : in) {
        if (c == '<') {
            tag = true;
            continue;
        }
        if (c == '>') {
            tag = false;
            continue;
        }
        if (!tag)
            out.push_back(c);
    }
    return out;
}

bool is_gui_object(const Instance* inst) {
    return inst->is_a("GuiObject") || inst->is_a("ScreenGui");
}

bool is_text(const Instance* inst) {
    return inst->is_a("TextLabel") || inst->is_a("TextButton") ||
           inst->is_a("TextBox");
}

bool is_image(const Instance* inst) {
    return inst->is_a("ImageLabel") || inst->is_a("ImageButton");
}

bool is_scrolling(const Instance* inst) {
    return inst->is_a("ScrollingFrame");
}

bool visible(const Instance* inst) {
    // The generated GuiObject.Visible placeholder is false; Roblox defaults
    // it to true.  An explicit false (script/replication) still hides.
    return boolean(inst, "Visible", true);
}

bool interactive(const Instance* inst) {
    const bool default_active =
        inst->is_a("GuiButton") || inst->is_a("TextBox") ||
        inst->is_a("ScrollingFrame");
    if (!boolean(inst, "Active", default_active))
        return false;
    return boolean(inst, "Interactable", true);
}

GuiRect intersect(const GuiRect& a, const GuiRect& b) {
    const double x0 = std::max(a.x, b.x);
    const double y0 = std::max(a.y, b.y);
    const double x1 = std::min(a.x + a.w, b.x + b.w);
    const double y1 = std::min(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0)
        return GuiRect{x0, y0, 0.0, 0.0};
    return GuiRect{x0, y0, x1 - x0, y1 - y0};
}

GuiRect absolute_rect(const Instance* inst, const GuiRect& parent) {
    const UDim2 pos = udim2(inst, "Position");
    const UDim2 size = udim2(inst, "Size");
    const Vector2 anchor = vector2(inst, "AnchorPoint");
    const double w = size.x.scale * parent.w + size.x.offset;
    const double h = size.y.scale * parent.h + size.y.offset;
    GuiRect r;
    r.x = parent.x + pos.x.scale * parent.w + pos.x.offset - anchor.x * w;
    r.y = parent.y + pos.y.scale * parent.h + pos.y.offset - anchor.y * h;
    r.w = w;
    r.h = h;
    return r;
}

const Instance* find_child_of_class(const Instance* parent, const char* cls) {
    if (!parent)
        return nullptr;
    for (const auto& child : parent->children())
        if (child->is_a(cls))
            return child.get();
    return nullptr;
}

const Instance* find_service(const Instance* root, const char* cls) {
    if (!root)
        return nullptr;
    if (root->is_a(cls))
        return root;
    std::vector<const Instance*> pending{root};
    while (!pending.empty()) {
        const Instance* inst = pending.back();
        pending.pop_back();
        if (inst->is_a(cls))
            return inst;
        for (const auto& child : inst->children())
            pending.push_back(child.get());
    }
    return nullptr;
}

const Instance* first_player_gui(const Instance* root) {
    if (!root)
        return nullptr;
    if (root->is_a("PlayerGui"))
        return root;
    std::vector<const Instance*> pending{root};
    while (!pending.empty()) {
        const Instance* inst = pending.back();
        pending.pop_back();
        if (inst->is_a("PlayerGui"))
            return inst;
        for (const auto& child : inst->children())
            pending.push_back(child.get());
    }
    return nullptr;
}

// Select the signed-in player's PlayerGui, never an arbitrary one: a live
// server replicates other Players, and attach_gui gives each a PlayerGui. A
// depth-first scan would pick whichever happened to be last in the tree (an
// empty screen list) while the probe's ScreenGui sits under LocalPlayer.
const Instance* find_player_gui(const Instance* root) {
    if (!root)
        return nullptr;
    if (root->is_a("PlayerGui"))
        return root;
    const Instance* players = find_service(root, "Players");
    if (!players)
        return first_player_gui(root);
    const Instance* local = nullptr;
    Variant v;
    if (players->get_prop("LocalPlayer", v))
        if (const auto* p = std::get_if<Instance*>(&v))
            local = *p;
    if (!local) {
        const long long uid = rbx::local_user_id();
        const std::string& uname = rbx::local_user_name();
        for (const auto& child : players->children()) {
            if (!child->is_a("Player"))
                continue;
            Variant id;
            if (uid > 0 && child->get_prop("UserId", id)) {
                if (const auto* p = std::get_if<int64_t>(&id);
                    p && *p == uid) {
                    local = child.get();
                    break;
                }
            }
            if (!uname.empty() && child->name() == uname) {
                local = child.get();
                break;
            }
        }
    }
    if (local) {
        if (const Instance* pg = find_child_of_class(local, "PlayerGui"))
            return pg;
    }
    // No LocalPlayer known (offline challenge): the first PlayerGui is the
    // only sensible GUI host. In live runs LocalPlayer always exists.
    for (const auto& child : players->children())
        if (const Instance* pg = find_child_of_class(child.get(), "PlayerGui"))
            return pg;
    return nullptr;
}

std::vector<const Instance*> sorted_children(const Instance* parent) {
    std::vector<const Instance*> children;
    for (const auto& child : parent->children())
        if (is_gui_object(child.get()))
            children.push_back(child.get());
    std::stable_sort(children.begin(), children.end(),
                     [](const Instance* a, const Instance* b) {
                         return integer(a, "ZIndex", 1) <
                                integer(b, "ZIndex", 1);
                     });
    return children;
}

void append_rect(const GuiNode& node, const GuiViewState* state,
                 std::vector<GuiDrawCommand>* out, bool force) {
    const Instance* inst = node.object;
    const double bg_alpha =
        1.0 - transparency(inst, "BackgroundTransparency");
    const int border_px = integer(inst, "BorderSizePixel", 0);
    if (!force && bg_alpha <= 0.001 && border_px <= 0)
        return;
    GuiDrawCommand c;
    c.kind = GuiKind::Rect;
    c.x = node.rect.x;
    c.y = node.rect.y;
    c.w = node.rect.w;
    c.h = node.rect.h;
    c.background =
        color(inst, "BackgroundColor3", Color3{0.639, 0.639, 0.639});
    c.background_alpha = bg_alpha;
    c.border = color(inst, "BorderColor3", Color3{0.105, 0.164, 0.207});
    c.border_alpha = bg_alpha;
    c.border_width = double(std::max(0, border_px));
    c.rotation = node.rotation;
    c.clip_x = node.clip.x;
    c.clip_y = node.clip.y;
    c.clip_w = node.clip.w;
    c.clip_h = node.clip.h;
    c.hovered = state && state->hovered == inst;
    c.pressed = state && state->pressed == inst;
    c.source = inst;
    out->push_back(std::move(c));
}

void refresh_text_metrics(const Instance* inst, const std::string& text,
                          double size, bool bold, double w, double h,
                          bool scaled) {
    const Vector2 bounds = measureGuiText(text, size, bold);
    bool changed = false;
    Variant old;
    if (!inst->get_prop("TextBounds", old) ||
        !std::holds_alternative<Vector2>(old) ||
        std::get<Vector2>(old).x != bounds.x ||
        std::get<Vector2>(old).y != bounds.y) {
        changed = true;
    }
    if (changed) {
        const_cast<Instance*>(inst)->set_internal_prop(
            "TextBounds", Variant(bounds));
    }
    const bool fits = scaled || (bounds.x <= w + 0.01 && bounds.y <= h + 0.01);
    Variant old_fit;
    if (!inst->get_prop("TextFits", old_fit) ||
        !std::holds_alternative<bool>(old_fit) ||
        std::get<bool>(old_fit) != fits) {
        const_cast<Instance*>(inst)->set_internal_prop(
            "TextFits", Variant(fits));
    }
}

void append_text(const GuiNode& node, const GuiViewState* state,
                 std::vector<GuiDrawCommand>* out) {
    const Instance* inst = node.object;
    const bool focused = state && state->focused == inst;
    std::string text = text_of(inst, "Text");
    if (boolean(inst, "RichText", false))
        text = strip_rich_text(text);
    const double size = std::max(1.0, num(inst, "TextSize", 14.0));
    const int font = enum_value(inst, "Font", 0);
    const bool bold = font == 2 || font == 5 || font == 7 || font == 10 ||
                      font == 13 || font == 16 || font == 18 || font == 20 ||
                      font == 21;
    const bool scaled = boolean(inst, "TextScaled", false);
    refresh_text_metrics(inst, text, size, bold, node.rect.w, node.rect.h,
                         scaled);

    bool placeholder = false;
    Color3 text_color = color(inst, "TextColor3", Color3{1.0, 1.0, 1.0});
    if (inst->is_a("TextBox") && text.empty() && !focused) {
        const std::string hint = text_of(inst, "PlaceholderText");
        if (!hint.empty()) {
            text = hint;
            placeholder = true;
            text_color =
                color(inst, "PlaceholderColor3", Color3{0.6, 0.6, 0.6});
        }
    }
    if (text.empty())
        return;

    GuiDrawCommand c;
    c.kind = GuiKind::Text;
    c.x = node.rect.x;
    c.y = node.rect.y;
    c.w = node.rect.w;
    c.h = node.rect.h;
    c.text = std::move(text);
    c.text_size = size;
    c.text_color = text_color;
    c.text_alpha =
        placeholder ? 1.0 : 1.0 - transparency(inst, "TextTransparency");
    c.h_align = std::clamp(
        enum_value(inst, "TextXAlignment", inst->is_a("TextBox") ? 0 : 1),
        0, 2);
    c.v_align = std::clamp(
        enum_value(inst, "TextYAlignment", inst->is_a("TextBox") ? 0 : 1),
        0, 2);
    c.wrap = boolean(inst, "TextWrapped", false) ||
             boolean(inst, "TextWrap", false);
    c.scaled = scaled;
    c.bold = bold;
    c.placeholder = placeholder;
    c.rotation = node.rotation;
    c.clip_x = node.clip.x;
    c.clip_y = node.clip.y;
    c.clip_w = node.clip.w;
    c.clip_h = node.clip.h;
    c.source = inst;
    c.focused = focused;
    c.caret = std::max(0, integer(inst, "CursorPosition", 0));
    c.selection_start = std::max(0, integer(inst, "SelectionStart", c.caret));
    c.caret_visible = state ? state->caret_visible : false;
    out->push_back(std::move(c));
}

void append_image(const GuiNode& node, std::vector<GuiDrawCommand>* out) {
    const Instance* inst = node.object;
    const std::string uri = text_of(inst, "Image");
    if (uri.empty())
        return;
    GuiDrawCommand c;
    c.kind = GuiKind::Image;
    c.x = node.rect.x;
    c.y = node.rect.y;
    c.w = node.rect.w;
    c.h = node.rect.h;
    // Resolution belongs to the presentation layer; layout stays asset-free.
    c.image_path = uri;
    c.image_color = color(inst, "ImageColor3", Color3{1.0, 1.0, 1.0});
    c.image_alpha = 1.0 - transparency(inst, "ImageTransparency");
    c.rotation = node.rotation;
    c.clip_x = node.clip.x;
    c.clip_y = node.clip.y;
    c.clip_w = node.clip.w;
    c.clip_h = node.clip.h;
    c.source = inst;
    out->push_back(std::move(c));
}

void append_scrollbars(const GuiNode& node,
                       std::vector<GuiDrawCommand>* out) {
    const Instance* inst = node.object;
    if (!is_scrolling(inst) ||
        !boolean(inst, "ScrollBarImageVisible", true))
        return;
    const ScrollMetrics m = scrollMetrics(inst, node.rect);
    if (m.thickness <= 0.0)
        return;
    const Color3 color = ::rbxclient::color(inst, "ScrollBarImageColor3",
                                            Color3{1.0, 1.0, 1.0});
    const double alpha =
        1.0 - transparency(inst, "ScrollBarImageTransparency");
    auto thumb = [&](const GuiRect& r, bool show) {
        if (!show || r.w <= 0.0 || r.h <= 0.0)
            return;
        GuiDrawCommand c;
        c.kind = GuiKind::Rect;
        c.x = r.x;
        c.y = r.y;
        c.w = r.w;
        c.h = r.h;
        c.background = color;
        c.background_alpha = alpha;
        c.border_width = 0.0;
        c.clip_x = node.clip.x;
        c.clip_y = node.clip.y;
        c.clip_w = node.clip.w;
        c.clip_h = node.clip.h;
        c.source = inst;
        out->push_back(std::move(c));
    };
    thumb(m.v_thumb, m.vertical && m.canvas_h > m.window_h);
    thumb(m.h_thumb, m.horizontal && m.canvas_w > m.window_w);
}

void visit_node(const Instance* inst, const GuiRect& parent,
                const GuiRect& clip, bool parent_visible,
                const GuiViewState* state, int* order,
                const GuiVisit& visit) {
    if (!inst || !is_gui_object(inst))
        return;
    const bool vis = parent_visible && visible(inst);
    if (!vis)
        return;
    GuiNode node;
    node.object = inst;
    node.rect = absolute_rect(inst, parent);
    if (node.rect.w <= 0.0 || node.rect.h <= 0.0)
        return;
    node.clip = clip;
    node.content = node.rect;
    node.child_clip = clip;
    node.rotation = num(inst, "Rotation", 0.0) * kPi / 180.0;
    node.interactive = interactive(inst);
    node.scrolling = is_scrolling(inst);

    const bool clips = node.scrolling || boolean(inst, "ClipsDescendants", false);
    if (clips)
        node.child_clip = intersect(clip, node.rect);

    if (node.scrolling) {
        const ScrollMetrics m = scrollMetrics(inst, node.rect);
        rbx::Vector2 extra{};
        if (state && state->scroll_offsets) {
            auto it = state->scroll_offsets->find(inst);
            if (it != state->scroll_offsets->end())
                extra = it->second;
        }
        node.content.x = node.rect.x - m.pos_x - extra.x;
        node.content.y = node.rect.y - m.pos_y - extra.y;
        node.content.w = m.canvas_w;
        node.content.h = m.canvas_h;
    }

    node.order = (*order)++;
    visit(node);

    for (const Instance* child : sorted_children(inst))
        visit_node(child, node.content, node.child_clip, vis, state, order,
                   visit);
}

} // namespace

void walkGui(const Instance* root, int width, int height,
             const GuiVisit& visit, const GuiViewState* state) {
    if (!root || width <= 0 || height <= 0 || !visit)
        return;
    const GuiRect viewport{0.0, 0.0, double(width), double(height)};
    const Instance* gui_root = find_player_gui(root);
    if (!gui_root)
        gui_root = root;

    std::vector<const Instance*> screens;
    if (gui_root->is_a("ScreenGui")) {
        screens.push_back(gui_root);
    } else {
        std::vector<const Instance*> stack{gui_root};
        while (!stack.empty()) {
            const Instance* inst = stack.back();
            stack.pop_back();
            if (inst->is_a("ScreenGui")) {
                screens.push_back(inst);
                continue;
            }
            for (const auto& child : inst->children())
                stack.push_back(child.get());
        }
    }
    std::stable_sort(screens.begin(), screens.end(),
                     [](const Instance* a, const Instance* b) {
                         return integer(a, "DisplayOrder", 0) <
                                integer(b, "DisplayOrder", 0);
                     });

    int order = 0;
    for (const Instance* screen : screens) {
        if (!boolean(screen, "Enabled", true))
            continue;
        GuiRect canvas = viewport;
        if (!boolean(screen, "IgnoreGuiInset", false)) {
            canvas.y += 36.0;
            canvas.h = std::max(0.0, canvas.h - 36.0);
        }
        if (canvas.w <= 0.0 || canvas.h <= 0.0)
            continue;
        for (const Instance* child : sorted_children(screen))
            visit_node(child, canvas, canvas, true, state, &order, visit);
    }
}

bool guiPointInside(const GuiNode& node, double x, double y) {
    if (x < node.clip.x || y < node.clip.y ||
        x > node.clip.x + node.clip.w || y > node.clip.y + node.clip.h)
        return false;
    const double cx = node.rect.x + node.rect.w * 0.5;
    const double cy = node.rect.y + node.rect.h * 0.5;
    const double dx = x - cx;
    const double dy = y - cy;
    const double c = std::cos(node.rotation);
    const double s = std::sin(node.rotation);
    const double lx = c * dx + s * dy;
    const double ly = -s * dx + c * dy;
    return std::abs(lx) <= node.rect.w * 0.5 + 0.001 &&
           std::abs(ly) <= node.rect.h * 0.5 + 0.001;
}

const Instance* hitTestGui(const Instance* root, int width, int height,
                           double x, double y) {
    const Instance* hit = nullptr;
    walkGui(root, width, height, [&](const GuiNode& node) {
        if (node.interactive && guiPointInside(node, x, y))
            hit = node.object;
    });
    return hit;
}

ScrollMetrics scrollMetrics(const Instance* frame, const GuiRect& rect) {
    ScrollMetrics m;
    m.window_w = rect.w;
    m.window_h = rect.h;
    const UDim2 canvas = udim2(frame, "CanvasSize");
    m.canvas_w = canvas.x.scale * rect.w + canvas.x.offset;
    m.canvas_h = canvas.y.scale * rect.h + canvas.y.offset;
    if (m.canvas_w <= 0.0)
        m.canvas_w = rect.w;
    if (m.canvas_h <= 0.0)
        m.canvas_h = rect.h;

    const int direction = enum_value(frame, "ScrollingDirection", 4);
    // Enum.ScrollingDirection is X=1, Y=2, XY=4 (not a bitmask).
    m.horizontal = direction == 1 || direction == 4;
    m.vertical = direction == 2 || direction == 4;
    if (!boolean(frame, "ScrollingEnabled", true)) {
        m.horizontal = m.vertical = false;
    }
    const Vector2 pos = vector2(frame, "CanvasPosition");
    m.max_x = m.horizontal ? std::max(0.0, m.canvas_w - m.window_w) : 0.0;
    m.max_y = m.vertical ? std::max(0.0, m.canvas_h - m.window_h) : 0.0;
    m.pos_x = std::clamp(pos.x, 0.0, m.max_x);
    m.pos_y = std::clamp(pos.y, 0.0, m.max_y);

    m.thickness = std::max(0.0, num(frame, "ScrollBarThickness", 12.0));
    if (m.thickness > std::min(m.window_w, m.window_h) * 0.5)
        m.thickness = std::max(0.0, std::min(m.window_w, m.window_h) * 0.5);
    if (m.thickness > 0.0) {
        if (m.max_y > 0.0) {
            const double track_h = std::max(0.0, m.window_h - m.thickness);
            const double thumb_h =
                std::min(track_h, std::max(24.0, track_h * track_h /
                                                     std::max(1.0, m.canvas_h)));
            m.v_track = GuiRect{rect.x + rect.w - m.thickness, rect.y,
                                m.thickness, m.window_h};
            m.v_thumb = GuiRect{m.v_track.x, rect.y,
                                m.thickness, thumb_h};
            if (m.max_y > 0.0)
                m.v_thumb.y += (std::max(0.0, track_h - thumb_h)) *
                               (m.pos_y / m.max_y);
        }
        if (m.max_x > 0.0) {
            const double track_w = std::max(0.0, m.window_w - m.thickness);
            const double thumb_w =
                std::min(track_w, std::max(24.0, track_w * track_w /
                                                     std::max(1.0, m.canvas_w)));
            m.h_track = GuiRect{rect.x, rect.y + rect.h - m.thickness,
                                m.window_w, m.thickness};
            m.h_thumb = GuiRect{rect.x, m.h_track.y, thumb_w,
                                m.thickness};
            if (m.max_x > 0.0)
                m.h_thumb.x += (std::max(0.0, track_w - thumb_w)) *
                               (m.pos_x / m.max_x);
        }
    }
    return m;
}

void layoutGuiObject(const rbx::Instance* object, const GuiRect& parent,
                     const GuiRect& clip, std::vector<GuiDrawCommand>* out,
                     const GuiViewState* state) {
    if (!out || !object)
        return;
    int order = 0;
    visit_node(object, parent, clip, true, state, &order,
               [&](const GuiNode& node) {
                   append_rect(node, state, out, is_text(node.object) ||
                                                    is_image(node.object));
                   if (is_text(node.object))
                       append_text(node, state, out);
                   if (is_image(node.object))
                       append_image(node, out);
                   append_scrollbars(node, out);
               });
}

std::vector<GuiDrawCommand> collectGui(const rbx::Instance* root, int width,
                                       int height,
                                       const GuiViewState* state) {
    std::vector<GuiDrawCommand> out;
    if (!root || width <= 0 || height <= 0)
        return out;
    walkGui(root, width, height,
            [&](const GuiNode& node) {
                append_rect(node, state, &out,
                            is_text(node.object) || is_image(node.object));
                if (is_text(node.object))
                    append_text(node, state, &out);
                if (is_image(node.object))
                    append_image(node, &out);
            },
            state);
    // Scrollbars are overlays; draw them after their contents so they stay
    // reachable and visible while the canvas is scrolled.
    walkGui(root, width, height,
            [&](const GuiNode& node) { append_scrollbars(node, &out); }, state);
    return out;
}

namespace {
GuiTextMeasure g_text_measure;
GuiImageLoadedQuery g_image_loaded;
}

void setGuiTextMeasure(GuiTextMeasure measure) {
    g_text_measure = std::move(measure);
}

rbx::Vector2 measureGuiText(const std::string& text, double size, bool bold) {
    if (g_text_measure)
        return g_text_measure(text, size, bold);
    // Deterministic fallback for headless scripts: grapheme width ~0.56em,
    // line height 1.2em.  The client replaces this with NanoVG's real font.
    const double em = std::max(1.0, size) * (bold ? 0.58 : 0.56);
    double width = 0.0;
    double line = 0.0;
    double max_width = 0.0;
    for (unsigned char c : text) {
        if (c == '\n') {
            max_width = std::max(max_width, width);
            width = 0.0;
            line += 1.0;
            continue;
        }
        width += em;
    }
    max_width = std::max(max_width, width);
    const double lines = std::max(1.0, line + 1.0);
    return rbx::Vector2{max_width, lines * std::max(1.0, size) * 1.2};
}

void setGuiImageLoadedQuery(GuiImageLoadedQuery query) {
    g_image_loaded = std::move(query);
}

bool guiImageLoaded(const std::string& uri) {
    return g_image_loaded ? g_image_loaded(uri) : false;
}

} // namespace rbxclient
