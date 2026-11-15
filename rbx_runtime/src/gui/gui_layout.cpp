// gui_layout.cpp — see gui_layout.h.
#include "gui/gui_layout.h"

#include "gui/gui_font_metrics.h"

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

int g_viewport_w = 1280;
int g_viewport_h = 720;
bool g_layout_in_progress = false;

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

int text_font(const Instance* inst) {
    return enum_value(inst, "Font", 0);
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

const Instance* find_child_of_class(const Instance* parent, const char* cls);

struct ChildPlacement {
    const Instance* child = nullptr;
    GuiRect rect;
};

bool layout_children_for(const Instance* parent, const GuiRect& content,
                         std::vector<ChildPlacement>* out, double parent_scale);

struct GuiInsets {
    double left = 0.0, right = 0.0, top = 0.0, bottom = 0.0;
};

// UIPadding in the object's base coordinate space.  The auto-size pass runs
// before the object's UIScale, so only the UDim scale component uses the base
// size; offsets remain pixels here.
GuiInsets padding_insets(const Instance* inst, double w, double h) {
    GuiInsets out;
    const Instance* padding = find_child_of_class(inst, "UIPadding");
    if (!padding)
        return out;
    auto get_u = [padding](const char* name) {
        Variant v;
        if (!padding->get_prop(name, v))
            return rbx::UDim{};
        if (const auto* u = std::get_if<rbx::UDim>(&v))
            return *u;
        return rbx::UDim{};
    };
    const rbx::UDim left = get_u("PaddingLeft");
    const rbx::UDim right = get_u("PaddingRight");
    const rbx::UDim top = get_u("PaddingTop");
    const rbx::UDim bottom = get_u("PaddingBottom");
    out.left = left.scale * w + left.offset;
    out.right = right.scale * w + right.offset;
    out.top = top.scale * h + top.offset;
    out.bottom = bottom.scale * h + bottom.offset;
    return out;
}

GuiRect constrained_size(const Instance* inst, double parent_w, double parent_h,
                         double parent_scale);

Vector2 wrapped_text_bounds(const std::string& text, double size, int face,
                            double available_w) {
    if (text.empty())
        return Vector2{0.0, 0.0};
    if (available_w <= 0.0)
        return measureGuiText(text, size, face);
    const double line_h = std::max(1.0, measureGuiText("A", size, face).y);
    double max_w = 0.0;
    int lines = 0;
    bool wrapped = false;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        const std::string source = text.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        std::string line;
        size_t word_start = 0;
        auto flush = [&]() {
            max_w = std::max(max_w, measureGuiText(line, size, face).x);
            line.clear();
            ++lines;
        };
        // Native's greedy wrapped TextBounds breaks a word wider than the
        // authored box at codepoint boundaries instead of overflowing it.
        auto append_word = [&](const std::string& word) {
            if (line.empty()) {
                for (std::size_t pos = 0; pos < word.size();) {
                    const std::size_t length =
                        font_metrics::codepointLength(word, pos);
                    const std::string ch = word.substr(pos, length);
                    pos += length;
                    const std::string candidate = line + ch;
                    if (!line.empty() &&
                        measureGuiText(candidate, size, face).x >
                            available_w) {
                        flush();
                        line = ch;
                        wrapped = true;
                    } else {
                        line = candidate;
                    }
                }
                return;
            }
            const std::string candidate = line + " " + word;
            if (measureGuiText(candidate, size, face).x <= available_w) {
                line = candidate;
                return;
            }
            flush();
            wrapped = true;
            line.clear();
            for (std::size_t pos = 0; pos < word.size();) {
                const std::size_t length =
                    font_metrics::codepointLength(word, pos);
                const std::string ch = word.substr(pos, length);
                pos += length;
                const std::string part = line + ch;
                if (!line.empty() &&
                    measureGuiText(part, size, face).x > available_w) {
                    flush();
                    line = ch;
                } else {
                    line = part;
                }
            }
        };
        while (word_start <= source.size()) {
            const size_t word_end = source.find(' ', word_start);
            const std::string word = source.substr(
                word_start, word_end == std::string::npos ? std::string::npos
                                                         : word_end - word_start);
            append_word(word);
            if (word_end == std::string::npos)
                break;
            word_start = word_end + 1;
        }
        flush();
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    // Native reports the widest line, clamped to the authored width; it does
    // not pad the box out to `available_w` when shorter lines wrap.
    max_w = std::min(max_w, available_w);
    return Vector2{std::round(max_w),
                   std::floor(double(std::max(1, lines)) * line_h)};
}

// Native 2.743's TextScaled automatic fallback measures a single legacy line
// at 67 px and reports a 100 px line height (floor(1.5 * 67)).  The value is
// only used while both the label and its parent are automatic on both axes;
// that is the circular case where the parent cannot supply a content box.
constexpr double kScaledAutoTextSize = 67.0;
constexpr double kScaledAutoLineHeight = 100.0;

bool scaled_auto_parent_context(const Instance* inst) {
    const Instance* parent = inst->parent();
    return parent &&
           (enum_value(parent, "AutomaticSize", 0) & 3) == 3 &&
           (enum_value(inst, "AutomaticSize", 0) & 3) == 3;
}

Vector2 scaled_auto_bounds(const std::string& text, int face) {
    int lines = 1;
    for (char c : text)
        if (c == '\n')
            ++lines;
    const double width =
        text.empty() ? 0.0 : measureGuiText(text, kScaledAutoTextSize, face).x;
    return Vector2{width, double(lines) * kScaledAutoLineHeight};
}

// Native TextScaled fits text to the label, but its natural scale is capped
// at a 100 px line (66.67 px font) until a UITextSizeConstraint raises it.
constexpr double kMaxScaledFont = kScaledAutoLineHeight / 1.5;

struct TextSizeLimits {
    double min = 1.0;
    double max = 100.0;
    bool present = false;
};

TextSizeLimits text_size_limits(const Instance* inst) {
    TextSizeLimits limits;
    const Instance* constraint =
        find_child_of_class(inst, "UITextSizeConstraint");
    if (!constraint)
        return limits;
    limits.present = true;
    limits.min = std::clamp(
        double(integer(constraint, "MinTextSize", 1)), 1.0, 100.0);
    limits.max = std::clamp(
        double(integer(constraint, "MaxTextSize", 100)), 1.0, 100.0);
    return limits;
}

struct TextLayout {
    double font = 14.0;
    Vector2 bounds;
    bool fits = true;
};

// TextScaled wraps character-by-character when a constraint forces the font
// beyond the natural width fit; native does not fall back to the whole word.
std::vector<std::string> wrap_text_chars(const std::string& text, double font,
                                         int face, double available_w) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        const std::string source = text.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        std::string line;
        for (std::size_t pos = 0; pos < source.size();) {
            const std::size_t length =
                font_metrics::codepointLength(source, pos);
            const std::string ch = source.substr(pos, length);
            pos += length;
            std::string candidate = line + ch;
            if (!line.empty() && available_w > 0.0 &&
                measureGuiText(candidate, font, face).x > available_w + 0.01) {
                lines.push_back(line);
                line = ch;
            } else {
                line = std::move(candidate);
            }
        }
        lines.push_back(line);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return lines;
}

TextLayout make_text_layout(const Instance* inst, const std::string& text,
                            int face, double available_w, double available_h,
                            double font_scale, bool auto_w, bool auto_h) {
    TextLayout layout;
    const bool scaled = boolean(inst, "TextScaled", false);
    const TextSizeLimits limits = text_size_limits(inst);
    const double authored =
        std::max(1.0, num(inst, "TextSize", 14.0) * font_scale);

    double natural = authored;
    if (scaled) {
        const double height_fit =
            auto_h || available_h <= 0.0
                ? kMaxScaledFont
                : std::min(available_h / 1.5, kMaxScaledFont);
        double width_fit = kMaxScaledFont;
        if (!auto_w && available_w > 0.0 &&
            measureGuiText(text, kMaxScaledFont, face).x > available_w + 0.01) {
            width_fit = 1.0;
            for (int f = int(std::floor(kMaxScaledFont)); f >= 1; --f) {
                if (measureGuiText(text, double(f), face).x <=
                    available_w + 0.01) {
                    width_fit = double(f);
                    break;
                }
            }
        }
        natural = std::min(height_fit, width_fit);
    }

    double font = natural;
    if (scaled || limits.present)
        font = std::max(limits.min, std::min(limits.max, natural));
    layout.font = font;

    const bool cap_fallback = scaled &&
                              natural >= kMaxScaledFont - 1.0e-6 &&
                              std::abs(font - natural) <= 1.0e-6;
    if (cap_fallback) {
        // The natural cap uses the native fallback's integer width metric and
        // a fixed 100 px line, while a raised MinTextSize uses the raised
        // font's ordinary bounds below.
        layout.bounds = scaled_auto_bounds(text, face);
        layout.fits = auto_w || auto_h
                          ? true
                          : (layout.bounds.x <= available_w + 0.01 &&
                             layout.bounds.y <= available_h + 0.01);
        return layout;
    }

    if (text.empty()) {
        layout.bounds = Vector2{0.0, 0.0};
        layout.fits = true;
        return layout;
    }

    const Vector2 raw = measureGuiText(text, font, face);
    const bool wraps = boolean(inst, "TextWrap", false) &&
                       (scaled || limits.present);
    if (wraps && !auto_w && available_w > 0.0 &&
        raw.x > available_w + 0.01) {
        const auto lines = wrap_text_chars(text, font, face, available_w);
        const double line_h =
            std::max(1.0, font_metrics::lineHeight(font, face));
        double full_w = 0.0;
        for (const auto& line : lines)
            full_w = std::max(full_w, measureGuiText(line, font, face).x);
        const double full_h = line_h * double(lines.size());
        const int capacity = auto_h
                                 ? int(lines.size())
                                 : std::max(1, int(std::floor(
                                                    available_h / line_h)));
        const int visible = std::max(
            1, std::min(int(lines.size()), capacity));
        double visible_w = 0.0;
        for (int i = 0; i < visible; ++i)
            visible_w = std::max(
                visible_w, measureGuiText(lines[size_t(i)], font, face).x);
        layout.bounds = Vector2{visible_w, line_h * double(visible)};
        layout.fits = auto_w || auto_h
                          ? true
                          : (full_w <= available_w + 0.01 &&
                             full_h <= available_h + 0.01);
        return layout;
    }

    layout.bounds = raw;
    layout.fits = auto_w || auto_h
                      ? true
                      : (raw.x <= available_w + 0.01 &&
                         raw.y <= available_h + 0.01);
    return layout;
}

Vector2 text_auto_bounds(const Instance* inst, double available_w) {
    std::string text = text_of(inst, "Text");
    if (boolean(inst, "RichText", false))
        text = strip_rich_text(text);
    double size = std::max(1.0, num(inst, "TextSize", 14.0));
    const TextSizeLimits limits = text_size_limits(inst);
    if (limits.present)
        size = std::max(limits.min, std::min(limits.max, size));
    if (boolean(inst, "TextScaled", false)) {
        if (scaled_auto_parent_context(inst)) {
            const TextLayout layout = make_text_layout(
                inst, text, text_font(inst), available_w, 0.0, 1.0, true,
                true);
            return layout.bounds;
        }
        return Vector2{1.0, 1.0};
    }
    if (text.empty())
        return Vector2{0.0, 0.0};
    // TextWrapped is stored as the same native flag as TextWrap.
    if (!boolean(inst, "TextWrap", false))
        return measureGuiText(text, size, text_font(inst));
    // Automatic X removes the authored wrapping width: native measures the
    // whole line and only Automatic Y can introduce wrapping.
    if (enum_value(inst, "AutomaticSize", 0) & 1)
        return measureGuiText(text, size, text_font(inst));
    return wrapped_text_bounds(text, size, text_font(inst), available_w);
}

// Children that contribute to an automatic parent.  Invisible children are
// absent from native AutomaticSize geometry; layout components are not
// children in the measured sense.
std::vector<const Instance*> auto_children(const Instance* inst) {
    std::vector<const Instance*> out;
    for (const auto& child : inst->children())
        if (child->is_a("GuiObject") && visible(child.get()))
            out.push_back(child.get());
    return out;
}

// Base-space content extent before the parent's own UIScale.  Layouts are
// measured with an effectively unbounded content rect: native AutomaticSize
// lets a horizontal grid grow to one row and a vertical grid to one column
// instead of using the authored (often zero) content size as the capacity.
GuiRect auto_content_size(const Instance* inst, double base_w, double base_h) {
    const int mode = enum_value(inst, "AutomaticSize", 0);
    if ((mode & 3) == 0)
        return GuiRect{};
    const GuiInsets pad = padding_insets(inst, base_w, base_h);
    double content_w = 0.0;
    double content_h = 0.0;

    if (is_text(inst)) {
        const Vector2 bounds = text_auto_bounds(inst, base_w);
        content_w = bounds.x;
        content_h = bounds.y;
    } else if (const Instance* layout = find_child_of_class(inst, "UIGridLayout")) {
        // Native AutomaticSize lets a grid grow along its fill direction:
        // Horizontal puts every child in one row, Vertical in one column.
        // Using layout_grid's normal capacity with an unbounded probe would
        // instead create thousands of empty cells.
        const auto udim2_prop = [&](const char* name, rbx::UDim2 fallback) {
            Variant v;
            if (!layout->get_prop(name, v))
                return fallback;
            if (const auto* u = std::get_if<rbx::UDim2>(&v))
                return *u;
            return fallback;
        };
        const rbx::UDim2 cell_u =
            udim2_prop("CellSize", rbx::UDim2{rbx::UDim{0.0, 100.0},
                                               rbx::UDim{0.0, 100.0}});
        const rbx::UDim2 pad_u =
            udim2_prop("CellPadding", rbx::UDim2{rbx::UDim{0.0, 5.0},
                                                  rbx::UDim{0.0, 5.0}});
        const double cell_w = cell_u.x.scale * base_w + cell_u.x.offset;
        const double cell_h = cell_u.y.scale * base_h + cell_u.y.offset;
        const double pad_x = pad_u.x.scale * base_w + pad_u.x.offset;
        const double pad_y = pad_u.y.scale * base_h + pad_u.y.offset;
        const auto children = auto_children(inst);
        const size_t count = children.size();
        if (count > 0) {
            const bool horizontal =
                enum_value(layout, "FillDirection", 0) == 0;
            const size_t cols = horizontal ? count : 1;
            const size_t rows = horizontal ? 1 : count;
            content_w = double(cols) * cell_w +
                        double(cols > 0 ? cols - 1 : 0) * pad_x;
            content_h = double(rows) * cell_h +
                        double(rows > 0 ? rows - 1 : 0) * pad_y;
        }
    } else if (const Instance* layout = find_child_of_class(inst, "UIListLayout")) {
        std::vector<ChildPlacement> placements;
        const GuiRect probe{0.0, 0.0, 1.0e6, 1.0e6};
        layout_children_for(inst, probe, &placements, 1.0);
        Variant v;
        if (layout->get_prop("AbsoluteContentSize", v))
            if (const auto* size = std::get_if<Vector2>(&v)) {
                content_w = size->x;
                content_h = size->y;
            }
    } else {
        for (const Instance* child : auto_children(inst)) {
            const GuiRect size = constrained_size(child, base_w, base_h, 1.0);
            const UDim2 pos = udim2(child, "Position");
            const Vector2 anchor = vector2(child, "AnchorPoint");
            const double x = pos.x.scale * base_w + pos.x.offset -
                             anchor.x * size.w;
            const double y = pos.y.scale * base_h + pos.y.offset -
                             anchor.y * size.h;
            content_w = std::max(content_w, x + size.w);
            content_h = std::max(content_h, y + size.h);
        }
    }
    return GuiRect{0.0, 0.0,
                   std::max(0.0, pad.left + content_w + pad.right),
                   std::max(0.0, pad.top + content_h + pad.bottom)};
}

// UIScale is a UIComponent on the object it scales.  Multiple components
// multiply, and a negative/zero scale collapses the subtree (native pivots the
// object's own size but not its top-left position).
double ui_scale(const Instance* inst) {
    double scale = 1.0;
    if (!inst)
        return scale;
    for (const auto& child : inst->children())
        if (child->is_a("UIScale"))
            scale *= std::max(0.0, num(child.get(), "Scale", 1.0));
    return scale;
}

// Base Size in the parent's effective pixel space, before the object's own
// UIScale.  Native applies UISizeConstraint and UIAspectRatioConstraint to this
// pre-scale size (observed with min 300x200 plus UIScale 2 -> 600x400), then
// multiplies by the object's UIScale.
GuiRect constrained_size(const Instance* inst, double parent_w, double parent_h,
                         double parent_scale) {
    const UDim2 size = udim2(inst, "Size");
    double w = size.x.scale * parent_w + size.x.offset * parent_scale;
    double h = size.y.scale * parent_h + size.y.offset * parent_scale;

    const int auto_mode = enum_value(inst, "AutomaticSize", 0);
    const GuiRect auto_size =
        auto_content_size(inst, w, h);
    // The content floor participates in aspect fitting on automatic axes.  It
    // is applied again after the constraint/aspect stage so a constraint
    // maximum can never shrink below the measured content.
    if (auto_mode & 1)
        w = std::max(w, auto_size.w * parent_scale);
    if (auto_mode & 2)
        h = std::max(h, auto_size.h * parent_scale);

    if (const Instance* constraint =
            find_child_of_class(inst, "UISizeConstraint")) {
        const Vector2 lo = vector2(constraint, "MinSize");
        const Vector2 hi = vector2(constraint, "MaxSize");
        const double min_w = std::max(0.0, lo.x);
        const double min_h = std::max(0.0, lo.y);
        const double max_w = std::max(min_w, hi.x);
        const double max_h = std::max(min_h, hi.y);
        w = std::clamp(w, min_w, max_w);
        h = std::clamp(h, min_h, max_h);
    }

    if (const Instance* aspect =
            find_child_of_class(inst, "UIAspectRatioConstraint")) {
        const double ratio = num(aspect, "AspectRatio", 1.0);
        if (ratio > 0.0 && std::isfinite(ratio)) {
            const int type = enum_value(aspect, "AspectType", 0);
            if (type == 1) { // ScaleWithParentSize: keep the dominant axis.
                if (enum_value(aspect, "DominantAxis", 0) == 1)
                    w = h * ratio;
                else
                    h = w / ratio;
            } else if (h > 0.0 && w / h > ratio) {
                w = h * ratio; // FitWithinMaxSize
            } else if (w > 0.0) {
                h = w / ratio;
            }
        }
    }

    // AutomaticSize is native's floor at the end of the constraint/aspect
    // stage and before the object's UIScale.  A UISizeConstraint maximum can
    // shrink the authored size, but never below the measured content; an
    // aspect constraint can adjust a non-automatic axis and may grow an
    // automatic one, but the measured axis remains a minimum.
    if (auto_mode & 1)
        w = std::max(w, auto_size.w * parent_scale);
    if (auto_mode & 2)
        h = std::max(h, auto_size.h * parent_scale);

    const double scale = ui_scale(inst);
    return GuiRect{0.0, 0.0, std::max(0.0, w * scale),
                   std::max(0.0, h * scale)};
}

GuiRect absolute_rect(const Instance* inst, const GuiRect& parent,
                      double parent_scale) {
    const GuiRect sized = constrained_size(inst, parent.w, parent.h, parent_scale);
    const UDim2 pos = udim2(inst, "Position");
    const Vector2 anchor = vector2(inst, "AnchorPoint");
    GuiRect r;
    r.x = parent.x + pos.x.scale * parent.w +
          pos.x.offset * parent_scale - anchor.x * sized.w;
    r.y = parent.y + pos.y.scale * parent.h +
          pos.y.offset * parent_scale - anchor.y * sized.h;
    r.w = sized.w;
    r.h = sized.h;
    return r;
}

double udim_value(const rbx::UDim& u, double basis) {
    return u.scale * basis + u.offset;
}

void write_absolute(const Instance* inst, const GuiRect& rect,
                    const GuiRect& clip, double rotation) {
    if (!inst)
        return;
    Instance* mutable_inst = const_cast<Instance*>(inst);
    mutable_inst->set_internal_prop(
        "AbsolutePosition", Variant(Vector2{rect.x, rect.y}));
    mutable_inst->set_internal_prop(
        "AbsoluteSize", Variant(Vector2{rect.w, rect.h}));
    mutable_inst->set_internal_prop(
        "AbsoluteRotation", Variant(rotation * 180.0 / kPi));
    mutable_inst->set_internal_prop(
        "RawRect2D",
        Variant(rbx::Rect{Vector2{rect.x, rect.y},
                          Vector2{rect.x + rect.w, rect.y + rect.h}}));
    const GuiRect clipped = clip.w > 0.0 && clip.h > 0.0
                                ? intersect(rect, clip)
                                : GuiRect{rect.x, rect.y, 0.0, 0.0};
    mutable_inst->set_internal_prop(
        "ClippedRect",
        Variant(rbx::Rect{Vector2{clipped.x, clipped.y},
                          Vector2{clipped.x + clipped.w,
                                  clipped.y + clipped.h}}));
}

// UIPadding is a UIComponent child that changes the coordinate space its
// siblings resolve against.  It does not move or resize the parent itself.
GuiRect padded_rect(const Instance* inst, const GuiRect& rect,
                    double scale) {
    const Instance* padding = find_child_of_class(inst, "UIPadding");
    if (!padding)
        return rect;
    Variant v;
    const auto get_u = [&](const char* name) {
        if (!padding->get_prop(name, v))
            return rbx::UDim{};
        if (const auto* u = std::get_if<rbx::UDim>(&v))
            return *u;
        return rbx::UDim{};
    };
    const rbx::UDim left = get_u("PaddingLeft");
    const rbx::UDim right = get_u("PaddingRight");
    const rbx::UDim top = get_u("PaddingTop");
    const rbx::UDim bottom = get_u("PaddingBottom");
    GuiRect out = rect;
    const auto pad = [scale](const rbx::UDim& u, double basis) {
        return u.scale * basis + u.offset * scale;
    };
    out.x += pad(left, rect.w);
    out.y += pad(top, rect.h);
    out.w = std::max(0.0, out.w - pad(left, rect.w) - pad(right, rect.w));
    out.h = std::max(0.0, out.h - pad(top, rect.h) - pad(bottom, rect.h));
    return out;
}

GuiRect size_rect(const Instance* inst, const GuiRect& parent,
                  double parent_scale) {
    return constrained_size(inst, parent.w, parent.h, parent_scale);
}

int sort_order(const Instance* layout) {
    return layout ? enum_value(layout, "SortOrder", 0) : 0;
}

std::vector<const Instance*> layout_children(const Instance* parent,
                                             const Instance* layout) {
    std::vector<const Instance*> children;
    if (!parent)
        return children;
    for (const auto& child : parent->children())
        if (child->is_a("GuiObject"))
            children.push_back(child.get());
    const int order = sort_order(layout);
    if (order == 1) {
        // Custom: use the script comparator when one is installed; otherwise
        // the native fallback is insertion order.
        if (lua_State* L = layout->callback_lua()) {
            std::stable_sort(
                children.begin(), children.end(),
                [L, layout](const Instance* a, const Instance* b) {
                    return rbx::instance_callback_compare(
                        L, const_cast<Instance*>(layout), a, b);
                });
        }
        return children;
    }
    std::stable_sort(children.begin(), children.end(),
                     [order](const Instance* a, const Instance* b) {
                         if (order == 2)
                             return integer(a, "LayoutOrder", 0) <
                                    integer(b, "LayoutOrder", 0);
                         return a->name() < b->name();
                     });
    return children;
}

double align_offset(int alignment, double available, double content) {
    // HorizontalAlignment/VerticalAlignment use Center=0, Left/Top=1,
    // Right/Bottom=2 in the native enum table.
    if (alignment == 1)
        return 0.0;
    if (alignment == 2)
        return available - content;
    return (available - content) * 0.5;
}

void set_vector_prop(const Instance* inst, const char* name, Vector2 v) {
    if (!inst)
        return;
    const_cast<Instance*>(inst)->set_internal_prop(name, Variant(v));
}

bool layout_list(const Instance* layout, const GuiRect& content,
                 std::vector<ChildPlacement>* out, double parent_scale) {
    if (!layout)
        return false;
    const bool vertical = enum_value(layout, "FillDirection", 1) == 1;
    const int h_align = enum_value(layout, "HorizontalAlignment", 1);
    const int v_align = enum_value(layout, "VerticalAlignment", 1);
    const int h_flex = enum_value(layout, "HorizontalFlex", 0);
    const int v_flex = enum_value(layout, "VerticalFlex", 0);
    const int main_flex = vertical ? v_flex : h_flex;
    const int cross_flex = vertical ? h_flex : v_flex;
    const int line_align = enum_value(layout, "ItemLineAlignment", 0);
    double gap = 0.0;
    {
        Variant v;
        if (layout->get_prop("Padding", v))
            if (const auto* u = std::get_if<rbx::UDim>(&v))
                gap = u->scale * (vertical ? content.h : content.w) +
                      u->offset * parent_scale;
    }
    const auto children = layout_children(layout->parent(), layout);
    if (children.empty()) {
        set_vector_prop(layout, "AbsoluteContentSize", Vector2{0.0, 0.0});
        return true;
    }

    struct Item {
        const Instance* child = nullptr;
        double w = 0.0, h = 0.0;
    };
    std::vector<Item> items;
    items.reserve(children.size());
    for (const Instance* child : children) {
        const GuiRect r = size_rect(child, content, parent_scale);
        items.push_back(Item{child, r.w, r.h});
    }

    const bool wraps = boolean(layout, "Wraps", false);
    if (!wraps) {
        double base_main = 0.0;
        double cross = 0.0;
        for (const Item& item : items) {
            base_main += vertical ? item.h : item.w;
            cross = std::max(cross, vertical ? item.w : item.h);
        }
        base_main += gap * std::max<size_t>(0, items.size() - 1);
        double main = base_main;
        const double content_main = vertical ? content.h : content.w;
        const double content_cross = vertical ? content.w : content.h;

        // Main-axis flex.  Native Fill distributes the remaining main-axis
        // space equally (not proportionally); the space modes keep item sizes
        // and move only the gaps.
        double leading = 0.0, between = gap;
        if (main_flex == 1) {
            const double item_sum =
                base_main - gap * double(items.size() - 1);
            const double available =
                content_main - gap * double(items.size() - 1);
            if (available > item_sum) {
                const double add = (available - item_sum) / double(items.size());
                for (Item& item : items) {
                    if (vertical)
                        item.h += add;
                    else
                        item.w += add;
                }
            } else if (available < item_sum && item_sum > 0.0) {
                // Native Fill shrinks overflowing items proportionally to
                // the available item area, unlike the equal growth above.
                const double scale = std::max(0.0, available / item_sum);
                for (Item& item : items) {
                    if (vertical)
                        item.h *= scale;
                    else
                        item.w *= scale;
                }
            }
            main = content_main;
        } else if (main_flex >= 2) {
            // Space modes measure only the item sizes: native replaces
            // Padding with the distributed flex gap instead of adding to it.
            const double item_sum =
                base_main - gap * double(items.size() - 1);
            const double free = std::max(0.0, content_main - item_sum);
            if (content_main < base_main) {
                // With no room to distribute, native leaves the normal
                // Padding in place and centers the overflowing block for
                // Around/Evenly; Between stays pinned to the start.
                if (main_flex == 2 || main_flex == 4)
                    leading = (content_main - base_main) * 0.5;
            } else if (main_flex == 2) { // SpaceAround
                leading = free / (2.0 * items.size());
                between = free / double(items.size());
            } else if (main_flex == 3) { // SpaceBetween
                between = items.size() > 1
                              ? free / double(items.size() - 1)
                              : 0.0;
            } else { // SpaceEvenly
                leading = free / double(items.size() + 1);
                between = free / double(items.size() + 1);
            }
        }

        // Cross-axis flex.  Fill stretches every item to the content cross
        // size; SpaceAround/Evenly center all items on the largest item (the
        // native line box); SpaceBetween leaves the line start at zero.
        if (cross_flex == 1) {
            for (Item& item : items) {
                if (vertical)
                    item.w = content_cross;
                else
                    item.h = content_cross;
            }
            cross = content_cross;
        }
        double cross_offset = 0.0;
        if (cross_flex == 2 || cross_flex == 4)
            cross_offset = (content_cross - cross) * 0.5;
        set_vector_prop(layout, "AbsoluteContentSize",
                        vertical ? Vector2{cross_flex == 1 ? content_cross : cross,
                                           main_flex == 1 ? content_main : main}
                                 : Vector2{main_flex == 1 ? content_main : main,
                                           cross_flex == 1 ? content_cross : cross});
        double cursor = (vertical ? content.y : content.x) +
                        (main_flex == 0
                             ? align_offset(vertical ? v_align : h_align,
                                            content_main, main)
                             : leading);
        for (const Item& item : items) {
            ChildPlacement p;
            p.child = item.child;
            if (vertical) {
                const double x = cross_flex == 0
                                     ? content.x +
                                           align_offset(h_align, content.w, item.w)
                                     : content.x + cross_offset;
                p.rect = GuiRect{x, cursor, item.w, item.h};
                cursor += item.h + between;
            } else {
                const double y = cross_flex == 0
                                     ? content.y +
                                           align_offset(v_align, content.h, item.h)
                                     : content.y + cross_offset;
                p.rect = GuiRect{cursor, y, item.w, item.h};
                cursor += item.w + between;
            }
            out->push_back(p);
        }
        return true;
    }

    // Wrapped list: each line/column is packed independently, then the whole
    // block is aligned inside the content rect.  Native automatic line
    // alignment pins every item to the line start; it does not center a short
    // item in a taller line (measured in the 2.743 edge probe).
    struct Line {
        std::vector<const Item*> items;
        double main = 0.0;
        double cross = 0.0;
        double leading = 0.0;
        double between = 0.0;
    };
    std::vector<Line> lines;
    for (const Item& item : items) {
        const double item_main = vertical ? item.h : item.w;
        if (!lines.empty() && !lines.back().items.empty() &&
            lines.back().main + gap + item_main >
                (vertical ? content.h : content.w)) {
            lines.emplace_back();
        }
        if (lines.empty())
            lines.emplace_back();
        Line& line = lines.back();
        line.main += (line.items.empty() ? 0.0 : gap) + item_main;
        line.cross = std::max(line.cross, vertical ? item.w : item.h);
        line.items.push_back(&item);
    }

    const double content_main = vertical ? content.h : content.w;
    const double content_cross = vertical ? content.w : content.h;
    for (Line& line : lines) {
        line.between = gap;
        if (line.items.empty())
            continue;
        if (main_flex == 1) {
            const double available =
                content_main - gap * double(line.items.size() - 1);
            const double item_sum =
                line.main - gap * double(line.items.size() - 1);
            if (available > item_sum) {
                const double add =
                    (available - item_sum) / double(line.items.size());
                for (const Item* item : line.items) {
                    Item* mutable_item = const_cast<Item*>(item);
                    if (vertical)
                        mutable_item->h += add;
                    else
                        mutable_item->w += add;
                }
            } else if (available < item_sum && item_sum > 0.0) {
                const double scale = std::max(0.0, available / item_sum);
                for (const Item* item : line.items) {
                    Item* mutable_item = const_cast<Item*>(item);
                    if (vertical)
                        mutable_item->h *= scale;
                    else
                        mutable_item->w *= scale;
                }
            }
            line.main = content_main;
        } else if (main_flex >= 2) {
            const double item_sum =
                line.main - gap * double(line.items.size() - 1);
            const double free = std::max(0.0, content_main - item_sum);
            if (content_main < line.main) {
                if (main_flex == 2 || main_flex == 4)
                    line.leading = (content_main - line.main) * 0.5;
                line.between = gap;
            } else if (main_flex == 2) {
                line.leading = free / (2.0 * line.items.size());
                line.between = free / double(line.items.size());
            } else if (main_flex == 3) {
                line.between = line.items.size() > 1
                                   ? free / double(line.items.size() - 1)
                                   : 0.0;
            } else {
                line.leading = free / double(line.items.size() + 1);
                line.between = free / double(line.items.size() + 1);
            }
        }
    }

    double total_cross = 0.0;
    double max_main = 0.0;
    for (const Line& line : lines) {
        total_cross += line.cross;
        max_main = std::max(max_main, line.main);
    }
    if (lines.size() > 1)
        total_cross += gap * (lines.size() - 1);
    double cross_leading = 0.0;
    double cross_between = gap;
    if (cross_flex == 1) {
        const double free = content_cross - total_cross;
        if (free > 0.0) {
            const double add = free / double(lines.size());
            for (Line& line : lines)
                line.cross += add;
            total_cross = content_cross;
        }
    } else if (cross_flex >= 2) {
        const double line_sum =
            total_cross - (lines.size() > 1 ? gap * (lines.size() - 1) : 0.0);
        const double free = std::max(0.0, content_cross - line_sum);
        if (cross_flex == 2) {
            cross_leading = free / (2.0 * lines.size());
            cross_between = free / double(lines.size());
        } else if (cross_flex == 3) {
            cross_between = lines.size() > 1
                                ? free / double(lines.size() - 1)
                                : 0.0;
        } else {
            cross_leading = free / double(lines.size() + 1);
            cross_between = free / double(lines.size() + 1);
        }
    }
    set_vector_prop(layout, "AbsoluteContentSize",
                    vertical ? Vector2{cross_flex == 1 ? content_cross : total_cross,
                                       main_flex == 1 ? content_main : max_main}
                             : Vector2{main_flex == 1 ? content_main : max_main,
                                       cross_flex == 1 ? content_cross : total_cross});
    double line_cursor = (vertical ? content.x : content.y) +
                         (cross_flex == 0
                              ? align_offset(vertical ? h_align : v_align,
                                             vertical ? content.w : content.h,
                                             total_cross)
                              : cross_flex == 1 ? 0.0 : cross_leading);
    for (const Line& line : lines) {
        // Automatic inherits the layout's cross-axis alignment; the other
        // modes are line-relative.  Native's default Left/Top therefore looks
        // like Start until a script changes one of those alignments.
        int item_align = line_align;
        if (item_align == 0) {
            const int align = vertical ? h_align : v_align;
            item_align = align == 0 ? 2 : align == 2 ? 3 : 1;
        }
        double cursor = (vertical ? content.y : content.x) +
                        (main_flex == 0
                             ? align_offset(vertical ? v_align : h_align,
                                            content_main, line.main)
                             : main_flex == 1 ? 0.0 : line.leading);
        for (const Item* item : line.items) {
            ChildPlacement p;
            p.child = item->child;
            double cross_size = vertical ? item->w : item->h;
            double cross_off = 0.0;
            if (cross_flex == 1) {
                cross_size = line.cross;
            } else if (item_align == 2) { // Center
                cross_off = (line.cross - cross_size) * 0.5;
            } else if (item_align == 3) { // End
                cross_off = line.cross - cross_size;
            } else if (item_align == 4) { // Stretch
                cross_size = line.cross;
            }
            if (vertical) {
                p.rect = GuiRect{line_cursor + cross_off, cursor, cross_size,
                                 item->h};
                cursor += item->h + (main_flex >= 2 ? line.between : gap);
            } else {
                p.rect = GuiRect{cursor, line_cursor + cross_off, item->w,
                                 cross_size};
                cursor += item->w + (main_flex >= 2 ? line.between : gap);
            }
            out->push_back(p);
        }
        line_cursor += line.cross + (cross_flex >= 2 ? cross_between : gap);
    }
    return true;
}

bool layout_grid(const Instance* layout, const GuiRect& content,
                 std::vector<ChildPlacement>* out, double parent_scale) {
    if (!layout)
        return false;
    const auto children = layout_children(layout->parent(), layout);
    const auto udim2_prop = [&](const char* name, rbx::UDim2 fallback) {
        Variant v;
        if (!layout->get_prop(name, v))
            return fallback;
        if (const auto* u = std::get_if<rbx::UDim2>(&v))
            return *u;
        return fallback;
    };
    const rbx::UDim2 cell_u =
        udim2_prop("CellSize", rbx::UDim2{rbx::UDim{0.0, 100.0},
                                           rbx::UDim{0.0, 100.0}});
    const rbx::UDim2 pad_u =
        udim2_prop("CellPadding", rbx::UDim2{rbx::UDim{0.0, 5.0},
                                              rbx::UDim{0.0, 5.0}});
    const double cell_w = cell_u.x.scale * content.w +
                          cell_u.x.offset * parent_scale;
    const double cell_h = cell_u.y.scale * content.h +
                          cell_u.y.offset * parent_scale;
    const double pad_x = pad_u.x.scale * content.w +
                         pad_u.x.offset * parent_scale;
    const double pad_y = pad_u.y.scale * content.h +
                         pad_u.y.offset * parent_scale;
    const int fill = enum_value(layout, "FillDirection", 0);
    const int max_cells = integer(layout, "FillDirectionMaxCells", 0);
    const int h_align = enum_value(layout, "HorizontalAlignment", 1);
    const int v_align = enum_value(layout, "VerticalAlignment", 1);
    const int start_corner = enum_value(layout, "StartCorner", 0);
    const size_t count = children.size();
    if (count == 0) {
        set_vector_prop(layout, "AbsoluteContentSize", Vector2{0.0, 0.0});
        set_vector_prop(layout, "AbsoluteCellSize", Vector2{cell_w, cell_h});
        set_vector_prop(layout, "AbsoluteCellCount", Vector2{0.0, 0.0});
        return true;
    }
    int cols = 1, rows = 1;
    if (fill == 0) {
        // FillDirectionMaxCells is a cap, not a forced count: native still
        // fits only as many cells as the content can hold (a max of 10 in a
        // 400 px frame with 100 px cells still produces three columns).
        const int capacity = std::max(
            1, int(std::floor((content.w + pad_x) /
                              std::max(1.0, cell_w + pad_x))));
        cols = max_cells > 0 ? std::min(max_cells, capacity) : capacity;
        rows = int((count + size_t(cols) - 1) / size_t(cols));
    } else {
        const int capacity = std::max(
            1, int(std::floor((content.h + pad_y) /
                              std::max(1.0, cell_h + pad_y))));
        rows = max_cells > 0 ? std::min(max_cells, capacity) : capacity;
        cols = int((count + size_t(rows) - 1) / size_t(rows));
    }
    const double total_w = cols * cell_w + std::max(0, cols - 1) * pad_x;
    const double total_h = rows * cell_h + std::max(0, rows - 1) * pad_y;
    set_vector_prop(layout, "AbsoluteContentSize", Vector2{total_w, total_h});
    set_vector_prop(layout, "AbsoluteCellSize", Vector2{cell_w, cell_h});
    set_vector_prop(layout, "AbsoluteCellCount",
                    Vector2{double(cols), double(rows)});
    const double origin_x = content.x + align_offset(h_align, content.w, total_w);
    const double origin_y = content.y + align_offset(v_align, content.h, total_h);
    for (size_t i = 0; i < count; ++i) {
        int col = 0, row = 0;
        if (fill == 0) {
            col = int(i % size_t(cols));
            row = int(i / size_t(cols));
        } else {
            row = int(i % size_t(rows));
            col = int(i / size_t(rows));
        }
        const bool right = start_corner == 1 || start_corner == 3;
        const bool bottom = start_corner == 2 || start_corner == 3;
        if (right)
            col = cols - 1 - col;
        if (bottom)
            row = rows - 1 - row;
        ChildPlacement p;
        p.child = children[i];
        p.rect = GuiRect{origin_x + col * (cell_w + pad_x),
                         origin_y + row * (cell_h + pad_y), cell_w, cell_h};
        out->push_back(p);
    }
    return true;
}

bool layout_children_for(const Instance* parent, const GuiRect& content,
                         std::vector<ChildPlacement>* out, double parent_scale) {
    if (!parent)
        return false;
    if (const Instance* grid = find_child_of_class(parent, "UIGridLayout"))
        return layout_grid(grid, content, out, parent_scale);
    if (const Instance* list = find_child_of_class(parent, "UIListLayout"))
        return layout_list(list, content, out, parent_scale);
    return false;
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
    c.border_width = double(std::max(0, border_px)) * node.scale;
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

void apply_text_metrics(const Instance* inst, const Vector2& bounds,
                        bool fits) {
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
    Variant old_fit;
    if (!inst->get_prop("TextFits", old_fit) ||
        !std::holds_alternative<bool>(old_fit) ||
        std::get<bool>(old_fit) != fits) {
        const_cast<Instance*>(inst)->set_internal_prop(
            "TextFits", Variant(fits));
    }
}

void refresh_text_metrics(const Instance* inst, const std::string& text,
                          double size, int face, double w, double h,
                          bool scaled) {
    Vector2 bounds = measureGuiText(text, size, face);
    bool wrapped_overflow = false;
    if (scaled) {
        if (scaled_auto_parent_context(inst))
            bounds = scaled_auto_bounds(text, face);
        else
            // TextScaled reports the fitted text box; with AutomaticSize both
            // axes and no automatic parent the native box is the 1x1 floor.
            bounds = Vector2{std::min(bounds.x, std::max(0.0, w)),
                             std::min(bounds.y, std::max(0.0, h))};
    } else if (boolean(inst, "TextWrap", false)) {
        if (enum_value(inst, "AutomaticSize", 0) & 1) {
            bounds = measureGuiText(text, size, face);
        } else if (w > 0.0) {
            bounds = wrapped_text_bounds(text, size, face, w);
            // A fixed wrapped label reports only complete visible lines, as
            // native does; TextFits still describes the whole string.
            const double line_h =
                std::max(1.0, measureGuiText("A", size, face).y);
            const int total = std::max(1, int(std::lround(bounds.y / line_h)));
            const int visible =
                h > 0.0
                    ? std::clamp(int(std::floor(h / line_h)), 1, total)
                    : total;
            wrapped_overflow = visible < total;
            bounds.y = line_h * double(visible);
        }
    }
    const bool fits = scaled ||
                      (!wrapped_overflow && bounds.x <= w + 0.01 &&
                       bounds.y <= h + 0.01);
    apply_text_metrics(inst, bounds, fits);
}

void append_text(const GuiNode& node, const GuiViewState* state,
                 std::vector<GuiDrawCommand>* out) {
    const Instance* inst = node.object;
    const bool focused = state && state->focused == inst;
    std::string text = text_of(inst, "Text");
    if (boolean(inst, "RichText", false))
        text = strip_rich_text(text);
    const double size =
        std::max(1.0, num(inst, "TextSize", 14.0) * node.scale);
    const int font = enum_value(inst, "Font", 0);
    const bool scaled = boolean(inst, "TextScaled", false);
    const TextSizeLimits limits = text_size_limits(inst);
    double draw_size = size;
    bool draw_scaled = scaled;
    if (scaled || limits.present) {
        const int auto_mode = enum_value(inst, "AutomaticSize", 0);
        TextLayout layout;
        if (scaled && (auto_mode & 3) == 3 &&
            !scaled_auto_parent_context(inst)) {
            // Native's lone scaled AutoXY label collapses to its 1x1 floor;
            // only an automatic parent supplies a fallback content box.
            layout.font = std::max(1.0, std::min(node.rect.w, node.rect.h));
            layout.bounds = Vector2{1.0, 1.0};
            layout.fits = true;
        } else {
            layout = make_text_layout(
                inst, text, font, node.rect.w, node.rect.h, node.scale,
                (auto_mode & 1) != 0, (auto_mode & 2) != 0);
        }
        apply_text_metrics(inst, layout.bounds, layout.fits);
        draw_size = layout.font;
        draw_scaled = false;
    } else {
        refresh_text_metrics(inst, text, size, font, node.rect.w, node.rect.h,
                             scaled);
    }

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
    c.text_size = draw_size;
    c.text_color = text_color;
    c.text_alpha =
        placeholder ? 1.0 : 1.0 - transparency(inst, "TextTransparency");
    // Native TextXAlignment is Left=0, Right=1, Center=2; the draw command
    // stores Left=0, Center=1, Right=2 for NanoVG.
    const int native_h = enum_value(inst, "TextXAlignment", 2);
    c.h_align = native_h == 0 ? 0 : native_h == 2 ? 1 : 2;
    c.v_align = std::clamp(
        enum_value(inst, "TextYAlignment", inst->is_a("TextBox") ? 0 : 1),
        0, 2);
    c.wrap = boolean(inst, "TextWrap", false);
    c.scaled = draw_scaled;
    c.font = font;
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
    const ScrollMetrics m = scrollMetrics(inst, node.rect, node.scale);
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

void visit_node_at(const Instance* inst, const GuiRect& rect,
                   const GuiRect& clip, bool parent_visible,
                   const GuiViewState* state, int* order,
                   const GuiVisit& visit, double parent_scale) {
    if (!inst || !is_gui_object(inst))
        return;
    const bool vis = parent_visible && visible(inst);
    if (!vis)
        return;
    GuiNode node;
    node.object = inst;
    node.rect = rect;
    // A zero-sized parent still lays out its children (native reports their
    // absolute geometry), and AutomaticSize X/Y leaves the other axis at 0.
    node.clip = clip;
    node.scale = parent_scale * ui_scale(inst);
    node.content = padded_rect(inst, node.rect, node.scale);
    node.child_clip = clip;
    node.rotation = num(inst, "Rotation", 0.0) * kPi / 180.0;
    node.interactive = interactive(inst);
    node.scrolling = is_scrolling(inst);

    write_absolute(inst, node.rect, node.clip, node.rotation);

    const bool clips = node.scrolling || boolean(inst, "ClipsDescendants", false);
    if (clips)
        node.child_clip = intersect(clip, node.rect);

    if (node.scrolling) {
        const ScrollMetrics m = scrollMetrics(inst, node.rect, node.scale);
        rbx::Vector2 extra{};
        if (state && state->scroll_offsets) {
            auto it = state->scroll_offsets->find(inst);
            if (it != state->scroll_offsets->end())
                extra = it->second;
        }
        node.content.x = node.rect.x - m.pos_x * node.scale - extra.x;
        node.content.y = node.rect.y - m.pos_y * node.scale - extra.y;
        node.content.w = m.canvas_w;
        node.content.h = m.canvas_h;
    }

    node.order = (*order)++;
    visit(node);

    std::vector<ChildPlacement> placements;
    const bool laid_out =
        layout_children_for(inst, node.content, &placements, node.scale);
    const auto children = sorted_children(inst);
    for (const Instance* child : children) {
        GuiRect child_rect;
        if (laid_out) {
            const auto it = std::find_if(
                placements.begin(), placements.end(),
                [child](const ChildPlacement& p) { return p.child == child; });
            if (it != placements.end())
                child_rect = it->rect;
            else
                child_rect = absolute_rect(child, node.content, node.scale);
        } else {
            child_rect = absolute_rect(child, node.content, node.scale);
        }
        visit_node_at(child, child_rect, node.child_clip, vis, state, order,
                      visit, node.scale);
    }
}

void visit_node(const Instance* inst, const GuiRect& parent,
                const GuiRect& clip, bool parent_visible,
                const GuiViewState* state, int* order,
                const GuiVisit& visit) {
    if (!inst || !parent_visible || !visible(inst))
        return;
    visit_node_at(inst, absolute_rect(inst, parent, 1.0), clip, parent_visible,
                  state, order, visit, 1.0);
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
        const_cast<Instance*>(screen)->set_internal_prop(
            "AbsoluteSize", Variant(Vector2{double(width), double(height)}));
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

ScrollMetrics scrollMetrics(const Instance* frame, const GuiRect& rect,
                            double scale) {
    ScrollMetrics m;
    m.window_w = rect.w;
    m.window_h = rect.h;
    const UDim2 canvas = udim2(frame, "CanvasSize");
    m.canvas_w = canvas.x.scale * rect.w + canvas.x.offset * scale;
    m.canvas_h = canvas.y.scale * rect.h + canvas.y.offset * scale;
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
    // CanvasPosition is a script-facing pixel value in the frame's own
    // coordinate space; UIScale changes where the canvas is drawn, not the
    // property (native leaves CanvasPosition at 10 under Scale=2).
    m.pos_x = std::clamp(pos.x, 0.0, m.max_x);
    m.pos_y = std::clamp(pos.y, 0.0, m.max_y);

    m.thickness = std::max(0.0, num(frame, "ScrollBarThickness", 12.0) * scale);
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
    g_viewport_w = width;
    g_viewport_h = height;
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

void setGuiLayoutViewport(int width, int height) {
    if (width > 0)
        g_viewport_w = width;
    if (height > 0)
        g_viewport_h = height;
}

void ensureGuiLayout(const rbx::Instance* instance) {
    if (!instance || g_layout_in_progress)
        return;
    const rbx::Instance* root = instance;
    while (root->parent())
        root = root->parent();
    if (!root)
        return;
    g_layout_in_progress = true;
    collectGui(root, g_viewport_w, g_viewport_h);
    g_layout_in_progress = false;
}

void setGuiTextMeasure(GuiTextMeasure measure) {
    g_text_measure = std::move(measure);
}

rbx::Vector2 measureGuiText(const std::string& text, double size, int font) {
    if (g_text_measure)
        return g_text_measure(text, size, font);
    return font_metrics::measure(text, size, font);
}

void setGuiImageLoadedQuery(GuiImageLoadedQuery query) {
    g_image_loaded = std::move(query);
}

bool guiImageLoaded(const std::string& uri) {
    return g_image_loaded ? g_image_loaded(uri) : false;
}

} // namespace rbxclient
