// gui_renderer.cpp — see gui_renderer.h.
#include "view/gui_renderer.h"

#include "common/asset_path.h"

#define GL_GLEXT_PROTOTYPES 1
#include <GL/glcorearb.h>

#define NANOVG_GL3_IMPLEMENTATION
#include "nanovg.h"
#include "nanovg_gl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace rbxclient {

namespace {

bool load_file(const std::string& path, std::vector<unsigned char>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    out->assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
    return !out->empty();
}

// The native tree ships SourceSansPro; fall back to any DejaVu present on
// the host so a missing asset never disables the whole GUI.
std::string font_path(bool bold) {
    const char* names[] = {
        bold ? "fonts/SourceSansPro-Bold.ttf"
             : "fonts/SourceSansPro-Regular.ttf",
        bold ? "fonts/SourceSansPro-Semibold.ttf"
             : "fonts/SourceSansPro-Light.ttf",
    };
    for (const char* name : names) {
        const std::string p = rbxasset_file(std::string("rbxasset://") + name);
        if (!p.empty())
            return p;
    }
    const char* system_paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    for (const char* p : system_paths)
        if (std::ifstream(p).good())
            return p;
    return {};
}

NVGcolor nvg_color(const rbx::Color3& c, double alpha) {
    return nvgRGBAf(float(std::clamp(c.r, 0.0, 1.0)),
                    float(std::clamp(c.g, 0.0, 1.0)),
                    float(std::clamp(c.b, 0.0, 1.0)),
                    float(std::clamp(alpha, 0.0, 1.0)));
}

void apply_transform(NVGcontext* vg, const GuiDrawCommand& c) {
    if (std::abs(c.rotation) > 1e-6) {
        const float cx = float(c.x + c.w * 0.5);
        const float cy = float(c.y + c.h * 0.5);
        nvgTranslate(vg, cx, cy);
        nvgRotate(vg, float(c.rotation));
        nvgTranslate(vg, -cx, -cy);
    }
}

void apply_clip(NVGcontext* vg, const GuiDrawCommand& c) {
    if (c.clip_w > 0.0 && c.clip_h > 0.0)
        nvgIntersectScissor(vg, float(c.clip_x), float(c.clip_y),
                            float(c.clip_w), float(c.clip_h));
}

} // namespace

GuiRenderer::GuiRenderer() = default;

GuiRenderer::~GuiRenderer() { shutdown(); }

bool GuiRenderer::init(std::string* err) {
    if (vg_)
        return true;
    vg_ = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!vg_) {
        if (err)
            *err = "NanoVG GL3 init failed";
        return false;
    }

    const std::string regular = font_path(false);
    const std::string bold = font_path(true);
    if (!regular.empty() && load_file(regular, &regular_data_))
        font_regular_ = nvgCreateFontMem(vg_, "sans", regular_data_.data(),
                                         int(regular_data_.size()), 0);
    if (!bold.empty() && load_file(bold, &bold_data_))
        font_bold_ = nvgCreateFontMem(vg_, "sans-bold", bold_data_.data(),
                                      int(bold_data_.size()), 0);
    if (font_regular_ < 0 && !regular.empty())
        font_regular_ = nvgCreateFont(vg_, "sans", regular.c_str());
    if (font_bold_ < 0 && !bold.empty())
        font_bold_ = nvgCreateFont(vg_, "sans-bold", bold.c_str());
    if (font_bold_ < 0)
        font_bold_ = font_regular_;
    setGuiTextMeasure([this](const std::string& text, double size, bool bold) {
        return measure(text, size, bold);
    });
    return true;
}

rbx::Vector2 GuiRenderer::measure(const std::string& text, double size,
                                  bool bold) {
    if (text.empty())
        return rbx::Vector2{0.0, std::max(1.0, size) * 1.2};
    if (!vg_ || font_regular_ < 0) {
        const double em = std::max(1.0, size) * (bold ? 0.58 : 0.56);
        double width = 0.0, line = 0.0, max_width = 0.0;
        for (char ch : text) {
            if (ch == '\n') {
                max_width = std::max(max_width, width);
                width = 0.0;
                ++line;
            } else {
                width += em;
            }
        }
        max_width = std::max(max_width, width);
        return rbx::Vector2{max_width,
                            std::max(1.0, line + 1.0) * std::max(1.0, size) *
                                1.2};
    }
    nvgSave(vg_);
    const int font = bold && font_bold_ >= 0 ? font_bold_ : font_regular_;
    nvgFontFaceId(vg_, font);
    nvgFontSize(vg_, float(std::max(1.0, size)));
    float bounds[4];
    nvgTextBounds(vg_, 0.0f, 0.0f, text.c_str(), nullptr, bounds);
    nvgRestore(vg_);
    return rbx::Vector2{std::max(0.0, double(bounds[2] - bounds[0])),
                        std::max(1.0, double(bounds[3] - bounds[1]))};
}

void GuiRenderer::shutdown() {
    setGuiTextMeasure({});
    images_.clear();
    regular_data_.clear();
    bold_data_.clear();
    if (vg_) {
        nvgDeleteGL3(vg_);
        vg_ = nullptr;
    }
    font_regular_ = font_bold_ = -1;
}

int GuiRenderer::image(const std::string& path) {
    if (!vg_ || path.empty())
        return 0;
    const auto it = images_.find(path);
    if (it != images_.end())
        return it->second;
    std::string file = rbxasset_file(path);
    if (file.empty() && path.rfind("rbxassetid://", 0) != 0 &&
        path.rfind("rbxthumb://", 0) != 0)
        file = path; // a direct local path remains supported
    if (file.empty())
        return 0;
    const int id = nvgCreateImage(vg_, file.c_str(), 0);
    images_[path] = id;
    return id;
}

void GuiRenderer::draw(const std::vector<GuiDrawCommand>& commands, int width,
                       int height) {
    if (!vg_ || commands.empty() || width <= 0 || height <= 0)
        return;
    // The caller binds the render target (the 3D pass's multisample FBO);
    // NanoVG must draw into that same target so --shot captures the GUI too.
    nvgBeginFrame(vg_, float(width), float(height), 1.0f);
    nvgSave(vg_);
    nvgResetScissor(vg_);

    for (const GuiDrawCommand& c : commands) {
        if (c.w <= 0.0 || c.h <= 0.0)
            continue;
        nvgSave(vg_);
        apply_clip(vg_, c);
        apply_transform(vg_, c);
        switch (c.kind) {
        case GuiKind::Rect: {
            nvgBeginPath(vg_);
            if (c.corner_radius > 0.0)
                nvgRoundedRect(vg_, float(c.x), float(c.y), float(c.w),
                               float(c.h), float(c.corner_radius));
            else
                nvgRect(vg_, float(c.x), float(c.y), float(c.w), float(c.h));
            if (c.background_alpha > 0.001) {
                nvgFillColor(vg_, nvg_color(c.background, c.background_alpha));
                nvgFill(vg_);
            }
            if (c.border_width > 0.0) {
                nvgStrokeWidth(vg_, float(c.border_width));
                nvgStrokeColor(vg_, nvg_color(c.border, c.border_alpha));
                nvgStroke(vg_);
            }
            // Native AutoButtonColor feedback.  The press tint is darker so
            // a held button is distinguishable from hover alone.
            if (c.pressed) {
                nvgBeginPath(vg_);
                nvgRect(vg_, float(c.x), float(c.y), float(c.w), float(c.h));
                nvgFillColor(vg_, nvgRGBAf(0.0f, 0.0f, 0.0f, 0.16f));
                nvgFill(vg_);
            } else if (c.hovered) {
                nvgBeginPath(vg_);
                nvgRect(vg_, float(c.x), float(c.y), float(c.w), float(c.h));
                nvgFillColor(vg_, nvgRGBAf(1.0f, 1.0f, 1.0f, 0.12f));
                nvgFill(vg_);
            }
            break;
        }
        case GuiKind::Text: {
            const int font =
                c.bold && font_bold_ >= 0 ? font_bold_ : font_regular_;
            if (font < 0)
                break;
            nvgFontFaceId(vg_, font);
            float size = float(c.text_size);
            if (c.scaled) {
                // Fit the box: height first, then shrink to width.
                size = float(std::max(6.0, c.h * 0.78));
                float bounds[4];
                nvgFontSize(vg_, size);
                nvgTextBounds(vg_, 0, 0, c.text.c_str(), nullptr, bounds);
                const float tw = bounds[2] - bounds[0];
                if (tw > 0.0f && tw > c.w * 0.96f)
                    size *= float(c.w * 0.96 / tw);
            }
            nvgFontSize(vg_, size);
            int align = NVG_ALIGN_LEFT;
            if (c.h_align == 1)
                align = NVG_ALIGN_CENTER;
            else if (c.h_align == 2)
                align = NVG_ALIGN_RIGHT;
            if (c.v_align == 0)
                align |= NVG_ALIGN_TOP;
            else if (c.v_align == 2)
                align |= NVG_ALIGN_BOTTOM;
            else
                align |= NVG_ALIGN_MIDDLE;
            nvgTextAlign(vg_, align);
            float x = float(c.x);
            if (c.h_align == 1)
                x = float(c.x + c.w * 0.5);
            else if (c.h_align == 2)
                x = float(c.x + c.w);
            float y = float(c.y + c.h * 0.5);
            if (c.v_align == 0)
                y = float(c.y);
            else if (c.v_align == 2)
                y = float(c.y + c.h);
            if (c.focused && !c.placeholder) {
                // Selection and caret are painted from the same glyph metrics
                // as the text, in the same alignment space.
                const std::string& text = c.text;
                const auto offsets = [&] {
                    std::vector<size_t> v;
                    for (size_t i = 0; i < text.size();) {
                        v.push_back(i);
                        ++i;
                        while (i < text.size() &&
                               (static_cast<unsigned char>(text[i]) & 0xC0) ==
                                   0x80)
                            ++i;
                    }
                    v.push_back(text.size());
                    return v;
                }();
                const int ncp = int(offsets.size()) - 1;
                const int caret = std::clamp(c.caret, 0, ncp);
                const int anchor = std::clamp(c.selection_start, 0, ncp);
                const int from = std::min(caret, anchor);
                const int to = std::max(caret, anchor);
                auto prefix_width = [&](int cp) {
                    float b[4];
                    const std::string part = text.substr(0, offsets[size_t(cp)]);
                    nvgTextBounds(vg_, 0.0f, 0.0f, part.c_str(), nullptr, b);
                    return b[2] - b[0];
                };
                const float start_x =
                    c.h_align == 1   ? float(c.x) + float(c.w) * 0.5f -
                                           prefix_width(ncp) * 0.5f
                    : c.h_align == 2 ? float(c.x + c.w) - prefix_width(ncp)
                                     : float(c.x);
                const float top = float(c.y) + std::max(1.0f, float(size) * 0.08f);
                if (to > from) {
                    const float sx = start_x + prefix_width(from);
                    const float ex = start_x + prefix_width(to);
                    nvgBeginPath(vg_);
                    nvgRect(vg_, sx, top, ex - sx,
                            std::max(2.0f, float(size) * 1.15f));
                    nvgFillColor(vg_, nvgRGBAf(0.25f, 0.55f, 0.95f, 0.45f));
                    nvgFill(vg_);
                }
                if (c.caret_visible) {
                    const float cx = start_x + prefix_width(caret);
                    nvgBeginPath(vg_);
                    nvgRect(vg_, cx, top, std::max(1.0f, float(size) * 0.06f),
                            std::max(2.0f, float(size) * 1.15f));
                    nvgFillColor(vg_, nvg_color(c.text_color, 1.0));
                    nvgFill(vg_);
                }
                nvgBeginPath(vg_);
                nvgRect(vg_, float(c.x), float(c.y), float(c.w), float(c.h));
                nvgStrokeWidth(vg_, 1.5f);
                nvgStrokeColor(vg_, nvgRGBAf(0.35f, 0.68f, 1.0f, 0.95f));
                nvgStroke(vg_);
            }
            nvgFillColor(vg_, nvg_color(c.text_color, c.text_alpha));
            if (c.wrap)
                nvgTextBox(vg_, x, y, float(c.w), c.text.c_str(), nullptr);
            else
                nvgText(vg_, x, y, c.text.c_str(), nullptr);
            break;
        }
        case GuiKind::Image: {
            const int id = image(c.image_path);
            if (id <= 0)
                break;
            const NVGpaint paint = nvgImagePattern(
                vg_, float(c.x), float(c.y), float(c.w), float(c.h), 0.0f, id,
                float(std::clamp(c.image_alpha, 0.0, 1.0)));
            nvgBeginPath(vg_);
            nvgRect(vg_, float(c.x), float(c.y), float(c.w), float(c.h));
            nvgFillPaint(vg_, paint);
            nvgFill(vg_);
            break;
        }
        }
        nvgRestore(vg_);
    }
    nvgRestore(vg_);
    nvgEndFrame(vg_);
}

} // namespace rbxclient
