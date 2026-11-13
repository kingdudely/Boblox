// gui_layout.h — pure ScreenGui layout, walk, hit-testing and scrolling geometry.
//
// This module is shared by the runtime input dispatcher and the OpenGL
// presentation layer.  It never touches a window, a font atlas or an asset:
// layout produces absolute-pixel commands, and a single walk supplies the same
// rects/clips/order to hit-testing so input cannot drift from what is drawn.
#pragma once

#include "instance/variant.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace rbx {
class Instance;
}

namespace rbxclient {

enum class GuiKind { Rect, Text, Image };

// Per-frame UI state owned by rbx::gui::Dispatcher.  Layout reads it only to
// paint hover/press feedback and the focused TextBox caret/selection.
struct GuiViewState {
    const rbx::Instance* hovered = nullptr;
    const rbx::Instance* pressed = nullptr;
    const rbx::Instance* focused = nullptr;
    bool caret_visible = true;
    // Optional elastic-scroll overshoot per ScrollingFrame.  CanvasPosition
    // itself stays clamped; this only shifts the rendered/hit content while
    // the spring returns to zero.
    const std::map<const rbx::Instance*, rbx::Vector2>* scroll_offsets = nullptr;
};

struct GuiRect {
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
};

struct GuiDrawCommand {
    GuiKind kind = GuiKind::Rect;
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;
    double rotation = 0.0;

    // Background/border (Rect; Text/Image commands carry only their own
    // foreground and are drawn on top of the parent's Rect command).
    rbx::Color3 background{0.639, 0.639, 0.639};
    double background_alpha = 1.0;
    rbx::Color3 border{0.1, 0.1, 0.1};
    double border_alpha = 1.0;
    double border_width = 0.0;
    double corner_radius = 0.0;

    // Text.
    std::string text;
    double text_size = 14.0;
    rbx::Color3 text_color{1.0, 1.0, 1.0};
    double text_alpha = 1.0;
    int h_align = 1; // 0 left, 1 center, 2 right
    int v_align = 1; // 0 top, 1 center, 2 bottom
    bool wrap = false;
    bool scaled = false;
    bool bold = false;
    bool placeholder = false;

    // TextBox interaction presentation.  `selection_start` is the anchor
    // (where the selection began); the caret is the other end.
    bool focused = false;
    int caret = 0;
    int selection_start = 0;
    bool caret_visible = true;

    // Hover/press feedback for GuiButtons (native AutoButtonColor).
    bool hovered = false;
    bool pressed = false;

    // Image.  The path is a resolved local file, never a network URL.
    std::string image_path;
    rbx::Color3 image_color{1.0, 1.0, 1.0};
    double image_alpha = 1.0;

    // Scissor rectangle in pixels; w/h <= 0 means no clip.
    double clip_x = 0.0, clip_y = 0.0, clip_w = 0.0, clip_h = 0.0;

    // Which object produced this command (diagnostics and hit tests).
    const rbx::Instance* source = nullptr;
};

// One visited GuiObject.  `content` is the coordinate space its children use;
// a ScrollingFrame offsets that space by CanvasPosition and `child_clip` is
// the visible viewport.  `order` is draw order (higher is on top).
struct GuiNode {
    const rbx::Instance* object = nullptr;
    GuiRect rect;
    GuiRect clip;       // ancestor clip applied to this object's own pixels
    GuiRect content;    // parent rect for this object's children
    GuiRect child_clip; // clip applied to this object's children
    double rotation = 0.0; // radians
    int order = 0;
    bool interactive = false;
    bool scrolling = false;
};

using GuiVisit = std::function<void(const GuiNode&)>;

// Walk every visible GuiObject in draw order.  ScreenGui inset, PlayerGui
// selection, ZIndex ordering, clipping and ScrollingFrame canvas offsets are
// applied exactly once here and shared by rendering and hit-testing.
void walkGui(const rbx::Instance* root, int width, int height,
             const GuiVisit& visit, const GuiViewState* state = nullptr);

// Topmost interactive object under a point, or nullptr for world input.
// Non-Active/non-interactive objects do not block objects beneath them.
const rbx::Instance* hitTestGui(const rbx::Instance* root, int width,
                                int height, double x, double y);

// Convert a point from screen space into an object's unrotated local space.
// True when the point is inside the object's rotated rectangle and its clip.
bool guiPointInside(const GuiNode& node, double x, double y);

struct ScrollMetrics {
    double window_w = 0.0, window_h = 0.0;
    double canvas_w = 0.0, canvas_h = 0.0;
    double max_x = 0.0, max_y = 0.0;
    double pos_x = 0.0, pos_y = 0.0;
    bool vertical = false;
    bool horizontal = false;
    double thickness = 0.0;
    GuiRect v_track, v_thumb, h_track, h_thumb;
};

// CanvasSize/CanvasPosition and scrollbar geometry for a laid-out frame.
// `pos_x/pos_y` are clamped to [0, max].  Shared by drawing, wheel input and
// scrollbar dragging.
ScrollMetrics scrollMetrics(const rbx::Instance* frame, const GuiRect& rect);

// Lay out one GuiObject subtree into draw commands (unit-test entry point).
void layoutGuiObject(const rbx::Instance* object, const GuiRect& parent,
                     const GuiRect& clip, std::vector<GuiDrawCommand>* out,
                     const GuiViewState* state = nullptr);

// Walk a live tree and produce draw commands.  `state` may be null (tests,
// headless snapshots): no hover/caret feedback is produced.
std::vector<GuiDrawCommand> collectGui(const rbx::Instance* root, int width,
                                       int height,
                                       const GuiViewState* state = nullptr);

// Text measurement used for TextBounds/TextFits and TextBox caret placement.
// The client installs its NanoVG-backed measurer; the fallback is a stable
// monospace approximation so headless scripts still get finite values.
using GuiTextMeasure =
    std::function<rbx::Vector2(const std::string&, double, bool)>;
void setGuiTextMeasure(GuiTextMeasure measure);
rbx::Vector2 measureGuiText(const std::string& text, double size, bool bold);

// ImageLabel/ImageButton.IsLoaded query.  The presentation layer owns the
// actual image cache; the runtime asks it through this hook so a headless
// build (no renderer installed) reports false rather than a fabricated true.
using GuiImageLoadedQuery = std::function<bool(const std::string&)>;
void setGuiImageLoadedQuery(GuiImageLoadedQuery query);
bool guiImageLoaded(const std::string& uri);

} // namespace rbxclient
