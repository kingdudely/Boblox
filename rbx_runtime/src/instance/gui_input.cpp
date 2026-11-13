// gui_input.cpp — see gui_input.h.
#include "instance/gui_input.h"

#include "instance/class_registry.h"
#include "instance/input_object.h"
#include "instance/instance.h"
#include "instance/signal.h"

#include "lua.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace rbx {
namespace gui {

namespace {

using rbxclient::GuiNode;
using rbxclient::GuiRect;
using rbxclient::GuiViewState;
using rbxclient::ScrollMetrics;

constexpr double kCaretBlink = 0.5;
constexpr double kTextPad = 4.0;

bool has_prop(const Instance* inst, const char* name) {
    return inst && inst->has_prop_override(name);
}

bool get_prop(const Instance* inst, const char* name, Variant* out) {
    return inst && inst->get_prop(name, *out);
}

bool bool_prop(const Instance* inst, const char* name, bool fallback) {
    if (!has_prop(inst, name))
        return fallback;
    Variant v;
    if (!get_prop(inst, name, &v))
        return fallback;
    if (const auto* b = std::get_if<bool>(&v))
        return *b;
    return fallback;
}

int int_prop(const Instance* inst, const char* name, int fallback) {
    Variant v;
    if (!get_prop(inst, name, &v))
        return fallback;
    if (const auto* i = std::get_if<int64_t>(&v))
        return int(*i);
    if (const auto* d = std::get_if<double>(&v))
        return int(*d);
    return fallback;
}

double num_prop(const Instance* inst, const char* name, double fallback,
                bool use_default = true) {
    if (use_default && !has_prop(inst, name))
        return fallback;
    Variant v;
    if (!get_prop(inst, name, &v))
        return fallback;
    if (const auto* d = std::get_if<double>(&v))
        return *d;
    if (const auto* i = std::get_if<int64_t>(&v))
        return double(*i);
    return fallback;
}

int enum_prop(const Instance* inst, const char* name, int fallback,
              bool use_default = true) {
    Variant v;
    if (use_default && !has_prop(inst, name))
        return fallback;
    if (!get_prop(inst, name, &v))
        return fallback;
    if (const auto* e = std::get_if<EnumItem>(&v))
        return e->value;
    return fallback;
}

std::string string_prop(const Instance* inst, const char* name) {
    Variant v;
    if (!get_prop(inst, name, &v))
        return {};
    if (const auto* s = std::get_if<std::string>(&v))
        return *s;
    return {};
}

Vector2 vector_prop(const Instance* inst, const char* name) {
    Variant v;
    if (!get_prop(inst, name, &v))
        return Vector2{};
    if (const auto* p = std::get_if<Vector2>(&v))
        return *p;
    return Vector2{};
}

bool same(const Vector2& a, const Vector2& b) {
    return std::abs(a.x - b.x) < 0.0001 && std::abs(a.y - b.y) < 0.0001;
}

void fire_lua(lua_State* L, Instance* inst, const char* event,
              std::vector<Variant> args = {}) {
    if (!L || !inst || !find_event(inst->class_info(), event))
        return;
    inst->event_signal(event)->fire(L, args);
}

void notify_changed(lua_State* L, Instance* inst, const char* name) {
    if (!L || !inst)
        return;
    inst->changed_signal(name)->fire(L);
    if (find_event(inst->class_info(), "Changed"))
        inst->event_signal("Changed")->fire(L, {Variant(std::string(name))});
}

// C++ property writes bypass the Lua __newindex hook, so GUI state changes
// explicitly fire the same change signals scripts observe for a normal write.
bool set_value(lua_State* L, Instance* inst, const char* name, Variant value,
               bool internal = false) {
    if (!inst)
        return false;
    if (internal) {
        inst->set_internal_prop(name, value);
        notify_changed(L, inst, name);
        return true;
    }
    const bool ok = inst->set_prop(name, value, nullptr);
    if (ok)
        notify_changed(L, inst, name);
    return ok;
}

bool utf8_continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

std::vector<size_t> codepoint_offsets(const std::string& s) {
    std::vector<size_t> offsets;
    offsets.reserve(s.size() + 1);
    size_t i = 0;
    while (i < s.size()) {
        offsets.push_back(i);
        ++i;
        while (i < s.size() && utf8_continuation((unsigned char)s[i]))
            ++i;
    }
    offsets.push_back(s.size());
    return offsets;
}

int codepoint_count(const std::string& s) {
    return int(codepoint_offsets(s).size()) - 1;
}

int clamp_index(int index, int count) {
    return std::clamp(index, 0, std::max(0, count));
}

std::string text_slice(const std::string& s, int from, int to) {
    const auto offsets = codepoint_offsets(s);
    const int n = int(offsets.size()) - 1;
    from = clamp_index(from, n);
    to = clamp_index(to, n);
    if (to < from)
        std::swap(from, to);
    return s.substr(offsets[size_t(from)], offsets[size_t(to)] -
                                             offsets[size_t(from)]);
}

int line_start(const std::string& s, int caret) {
    const auto offsets = codepoint_offsets(s);
    const int n = int(offsets.size()) - 1;
    caret = clamp_index(caret, n);
    size_t byte = offsets[size_t(caret)];
    while (byte > 0 && s[byte - 1] != '\n') {
        --byte;
        while (byte > 0 && utf8_continuation((unsigned char)s[byte]))
            --byte;
    }
    // Recompute the codepoint index of the byte offset.
    return int(std::lower_bound(offsets.begin(), offsets.end(), byte) -
               offsets.begin());
}

int line_end(const std::string& s, int caret) {
    const auto offsets = codepoint_offsets(s);
    const int n = int(offsets.size()) - 1;
    caret = clamp_index(caret, n);
    size_t byte = offsets[size_t(caret)];
    while (byte < s.size() && s[byte] != '\n') {
        ++byte;
        while (byte < s.size() && utf8_continuation((unsigned char)s[byte]))
            ++byte;
    }
    return int(std::lower_bound(offsets.begin(), offsets.end(), byte) -
               offsets.begin());
}

int word_left(const std::string& s, int caret) {
    const auto offsets = codepoint_offsets(s);
    int i = clamp_index(caret, int(offsets.size()) - 1);
    while (i > 0) {
        const char c = s[offsets[size_t(i - 1)]];
        if (std::isalnum((unsigned char)c) || c == '_')
            break;
        --i;
    }
    while (i > 0) {
        const char c = s[offsets[size_t(i - 1)]];
        if (!(std::isalnum((unsigned char)c) || c == '_'))
            break;
        --i;
    }
    return i;
}

int word_right(const std::string& s, int caret) {
    const auto offsets = codepoint_offsets(s);
    const int n = int(offsets.size()) - 1;
    int i = clamp_index(caret, n);
    while (i < n) {
        const char c = s[offsets[size_t(i)]];
        if (std::isalnum((unsigned char)c) || c == '_')
            break;
        ++i;
    }
    while (i < n) {
        const char c = s[offsets[size_t(i)]];
        if (!(std::isalnum((unsigned char)c) || c == '_'))
            break;
        ++i;
    }
    return i;
}

bool bold_font(int font) {
    return font == 2 || font == 5 || font == 7 || font == 10 || font == 13 ||
           font == 16 || font == 18 || font == 20 || font == 21;
}

double text_size_of(const Instance* tb) {
    return std::max(1.0, num_prop(tb, "TextSize", 14.0));
}

bool text_bold(const Instance* tb) {
    return bold_font(enum_prop(tb, "Font", 0, false));
}

double text_width(const std::string& s, const Instance* tb) {
    return rbxclient::measureGuiText(s, text_size_of(tb), text_bold(tb)).x;
}

struct TextEntry {
    Ref<Instance> object;
    int caret = 0;
    int anchor = 0;
};

struct ScrollDrag {
    Ref<Instance> frame;
    bool vertical = false;
    double start_mouse = 0.0;
    double start_position = 0.0;
    double track = 0.0;
    double thumb = 0.0;
    double maximum = 0.0;
};

class State {
  public:
    Instance* root = nullptr;
    Ref<Instance> hovered;
    Ref<Instance> pressed;
    Ref<Instance> selected;
    Ref<Instance> focused;
    int pressed_button = 0;
    double last_x = -1.0;
    double last_y = -1.0;
    double last_width = 0.0;
    double last_height = 0.0;
    lua_State* lua = nullptr;
    double time = 0.0;
    double blink_accum = 0.0;
    bool blink_on = true;
    bool scroll_drag_active = false;
    ScrollDrag scroll_drag;
    std::map<const Instance*, TextEntry> texts;
    std::map<const Instance*, Vector2> elastic;
    GuiViewState view;

    void sync_view() {
        view.hovered = hovered.get();
        view.pressed = pressed.get();
        view.focused = focused.get();
        view.caret_visible = focused && blink_on;
        view.scroll_offsets = &elastic;
    }

    TextEntry& text_entry(Instance* inst) {
        auto it = texts.find(inst);
        if (it == texts.end()) {
            TextEntry e;
            e.object = Ref<Instance>(inst);
            e.caret = int_prop(inst, "CursorPosition", 0);
            e.anchor = int_prop(inst, "SelectionStart", e.caret);
            it = texts.emplace(inst, std::move(e)).first;
        }
        return it->second;
    }

    int text_count(const TextEntry& e) const {
        return e.object ? codepoint_count(string_prop(e.object.get(), "Text"))
                        : 0;
    }

    void normalize(TextEntry& e) {
        if (!e.object)
            return;
        const int n = text_count(e);
        e.caret = clamp_index(e.caret, n);
        e.anchor = clamp_index(e.anchor, n);
    }

    void write_caret(TextEntry& e) {
        set_value(lua, const_cast<Instance*>(e.object.get()),
                  "CursorPosition", Variant(int64_t(e.caret)));
        set_value(lua, const_cast<Instance*>(e.object.get()),
                  "SelectionStart", Variant(int64_t(e.anchor)));
    }

    void set_text(TextEntry& e, const std::string& text, int caret,
                  int anchor) {
        if (!e.object)
            return;
        set_value(lua, const_cast<Instance*>(e.object.get()), "Text",
                  Variant(text));
        e.caret = caret;
        e.anchor = anchor;
        normalize(e);
        write_caret(e);
    }

    void replace_selection(TextEntry& e, const std::string& insertion) {
        if (!e.object)
            return;
        const std::string current =
            string_prop(e.object.get(), "Text");
        const int from = std::min(e.caret, e.anchor);
        const int to = std::max(e.caret, e.anchor);
        const auto offsets = codepoint_offsets(current);
        const std::string next =
            current.substr(0, offsets[size_t(from)]) + insertion +
            current.substr(offsets[size_t(to)]);
        const int caret = from + codepoint_count(insertion);
        set_text(e, next, caret, caret);
    }

    Instance* hit(double x, double y, int width, int height) const {
        return const_cast<Instance*>(
            rbxclient::hitTestGui(root, width, height, x, y));
    }

    Instance* scroll_ancestor(Instance* inst) const {
        for (Instance* p = inst; p; p = p->parent())
            if (p->is_a("ScrollingFrame"))
                return p;
        return nullptr;
    }

    bool node_for(const Instance* wanted, GuiNode* out, int width,
                  int height) const {
        bool found = false;
        rbxclient::walkGui(root, width, height, [&](const GuiNode& node) {
            if (!found && node.object == wanted) {
                *out = node;
                found = true;
            }
        });
        return found;
    }

    void set_selection(lua_State* L, Instance* next) {
        if (selected.get() == next)
            return;
        Ref<Instance> old = selected;
        selected = next ? Ref<Instance>(next) : Ref<Instance>();
        if (old)
            fire_lua(L, old.get(), "SelectionLost");
        if (selected)
            fire_lua(L, selected.get(), "SelectionGained");
    }

    void update_hover(lua_State* L, Instance* hit, double x, double y) {
        if (hovered.get() == hit)
            return;
        Ref<Instance> old = hovered;
        hovered = hit ? Ref<Instance>(hit) : Ref<Instance>();
        if (old)
            fire_lua(L, old.get(), "MouseLeave", {Variant(x), Variant(y)});
        if (hovered)
            fire_lua(L, hovered.get(), "MouseEnter", {Variant(x), Variant(y)});
    }

    void leave_focus(lua_State* L, bool enter) {
        if (!focused)
            return;
        Ref<Instance> old = focused;
        focused = Ref<Instance>();
        keyboard_capture(false);
        if (enter)
            fire_lua(L, old.get(), "ReturnPressed");
        fire_lua(L, old.get(), "FocusLost", {Variant(enter)});
        set_selection(L, nullptr);
        blink_on = true;
        sync_view();
    }

    void focus(lua_State* L, Instance* textbox, bool clicked, double click_x) {
        if (!textbox || !textbox->is_a("TextBox"))
            return;
        if (focused.get() == textbox) {
            // A second click inside an already focused TextBox only moves
            // the caret; it must not duplicate Focused.
            TextEntry& e = text_entry(textbox);
            if (clicked)
                e.caret = caret_at(*this, e, click_x);
            else
                e.caret = text_count(e);
            e.anchor = e.caret;
            normalize(e);
            write_caret(e);
            blink_on = true;
            blink_accum = 0.0;
            sync_view();
            return;
        }
        leave_focus(L, false);
        focused = Ref<Instance>(textbox);
        TextEntry& e = text_entry(textbox);
        if (clicked)
            e.caret = caret_at(*this, e, click_x);
        else
            e.caret = clicked ? 0 : text_count(e);
        if (bool_prop(textbox, "ClearTextOnFocus", false)) {
            const std::string current = string_prop(textbox, "Text");
            if (!current.empty())
                set_text(e, std::string(), 0, 0);
        }
        if (!clicked)
            e.caret = text_count(e);
        e.anchor = e.caret;
        normalize(e);
        write_caret(e);
        keyboard_capture(true);
        fire_lua(L, textbox, "Focused");
        set_selection(L, textbox);
        blink_on = true;
        blink_accum = 0.0;
        sync_view();
    }

    static int caret_at(const State& state, TextEntry& e, double click_x) {
        GuiNode node;
        if (!state.node_for(e.object.get(), &node, int(state.last_width),
                            int(state.last_height)))
            return state.text_count(e);
        const std::string text = string_prop(e.object.get(), "Text");
        const double size = text_size_of(e.object.get());
        const double width = text_width(text, e.object.get());
        const int align = enum_prop(e.object.get(), "TextXAlignment", 0);
        double start = node.rect.x + kTextPad;
        if (align == 1)
            start = node.rect.x + (node.rect.w - width) * 0.5;
        else if (align == 2)
            start = node.rect.x + node.rect.w - width - kTextPad;
        const double local = click_x - start;
        const auto offsets = codepoint_offsets(text);
        int best = 0;
        double best_dist = 1e30;
        for (int i = 0; i < int(offsets.size()); ++i) {
            const double w =
                rbxclient::measureGuiText(text.substr(0, offsets[size_t(i)]),
                                          size, false)
                    .x;
            const double d = std::abs(w - local);
            if (d < best_dist) {
                best_dist = d;
                best = i;
            }
        }
        return best;
    }

    static void keyboard_capture(bool on) { input::set_keyboard_captured(on); }

    bool handle_key(lua_State* L, const input::Event& ev) {
        if (!focused || !ev.keycode)
            return false;
        TextEntry& e = text_entry(focused.get());
        normalize(e);
        const bool ctrl = ev.ctrl || input::key_down(305) ||
                          input::key_down(306);
        const bool shift = ev.shift || input::key_down(303) ||
                           input::key_down(304);
        const int key = ev.keycode;
        const std::string current = string_prop(focused.get(), "Text");
        const int n = codepoint_count(current);
        const bool editable = bool_prop(focused.get(), "TextEditable", true);

        auto move = [&](int to, bool select) {
            e.caret = clamp_index(to, n);
            if (!select)
                e.anchor = e.caret;
            write_caret(e);
            blink_on = true;
            blink_accum = 0.0;
            sync_view();
        };

        if (ctrl && key == 65) { // A
            e.anchor = 0;
            e.caret = n;
            write_caret(e);
            return true;
        }
        if (ctrl && key == 67) { // C
            if (e.caret != e.anchor)
                input::set_clipboard_text(
                    text_slice(current, e.anchor, e.caret));
            return true;
        }
        if (ctrl && key == 88) { // X
            if (editable && e.caret != e.anchor) {
                input::set_clipboard_text(
                    text_slice(current, e.anchor, e.caret));
                replace_selection(e, std::string());
            }
            return true;
        }
        if (ctrl && key == 86) { // V
            if (editable) {
                const std::string clip = input::clipboard_text();
                if (!clip.empty())
                    replace_selection(e, clip);
            }
            return true;
        }
        if (key == 8) { // backspace
            if (!editable)
                return true;
            if (e.caret != e.anchor) {
                replace_selection(e, std::string());
            } else if (e.caret > 0) {
                e.anchor = e.caret - 1;
                replace_selection(e, std::string());
            }
            return true;
        }
        if (key == 127) { // delete
            if (!editable)
                return true;
            if (e.caret != e.anchor) {
                replace_selection(e, std::string());
            } else if (e.caret < n) {
                e.anchor = e.caret;
                e.caret = e.caret + 1;
                replace_selection(e, std::string());
            }
            return true;
        }
        if (key == 276 || key == 275) { // left / right
            const int dir = key == 276 ? -1 : 1;
            if (ctrl) {
                move(dir < 0 ? word_left(current, e.caret)
                             : word_right(current, e.caret),
                     shift);
            } else {
                move(e.caret + dir, shift);
            }
            return true;
        }
        if (key == 278) { // home
            move(line_start(current, e.caret), shift);
            return true;
        }
        if (key == 279) { // end
            move(line_end(current, e.caret), shift);
            return true;
        }
        if (key == 273 || key == 274) { // up / down (MultiLine line motion)
            if (bool_prop(focused.get(), "MultiLine", false)) {
                const int start = line_start(current, e.caret);
                const int end = line_end(current, e.caret);
                if (key == 273) {
                    const int prev_end =
                        start > 0 ? line_end(current, start - 1) : 0;
                    const int prev_start =
                        prev_end > 0 ? line_start(current, prev_end - 1) : 0;
                    move(prev_start + std::min(e.caret - start,
                                               prev_end - prev_start),
                         shift);
                } else {
                    const int next_start =
                        end < n ? line_end(current, end + 1) + 1 : n;
                    const int next_end = line_end(current, next_start);
                    move(next_start + std::min(e.caret - start,
                                               next_end - next_start),
                         shift);
                }
            }
            return true;
        }
        if (key == 13) { // enter
            if (!editable)
                return true;
            if (bool_prop(focused.get(), "MultiLine", false))
                replace_selection(e, "\n");
            else {
                leave_focus(L, true);
            }
            return true;
        }
        if (key == 27) { // escape
            leave_focus(L, false);
            return true;
        }
        if (key == 9) { // tab: keep it inside the TextBox rather than moving
            if (editable)
                replace_selection(e, "\t");
            return true;
        }
        // Printable shortcuts are inserted by the platform TextInput event,
        // not here, so keyboard layouts/IME remapping stay correct.
        return true;
    }

    bool handle_text(lua_State* L, const input::Event& ev) {
        (void)L;
        if (!focused || !bool_prop(focused.get(), "TextEditable", true))
            return focused.get() != nullptr;
        if (ev.text.empty())
            return true;
        if (ev.ctrl || input::key_down(305) || input::key_down(306))
            return true; // ctrl chords are handled as keys
        TextEntry& e = text_entry(focused.get());
        replace_selection(e, ev.text);
        return true;
    }
};

State& state() {
    static State s;
    return s;
}

void sync_capture() {
    input::set_pointer_captured(state().hovered || state().pressed ||
                                state().scroll_drag_active);
    input::set_keyboard_captured(state().focused.get() != nullptr);
}

void clamp_scrolling(lua_State* L, int width, int height) {
    State& s = state();
    if (!s.root || width <= 0 || height <= 0)
        return;
    rbxclient::walkGui(s.root, width, height, [&](const GuiNode& node) {
        if (!node.scrolling)
            return;
        Instance* frame = const_cast<Instance*>(node.object);
        const ScrollMetrics m = rbxclient::scrollMetrics(frame, node.rect);
        auto set_vec = [&](const char* name, const Vector2& value) {
            if (!same(vector_prop(frame, name), value))
                set_value(nullptr, frame, name, Variant(value), true);
        };
        set_vec("AbsoluteCanvasSize", Vector2{m.canvas_w, m.canvas_h});
        set_vec("AbsoluteWindowSize", Vector2{m.window_w, m.window_h});
        set_vec("MaxCanvasPosition", Vector2{m.max_x, m.max_y});
        const Vector2 pos = vector_prop(frame, "CanvasPosition");
        if (!same(pos, Vector2{m.pos_x, m.pos_y})) {
            set_value(L, frame, "CanvasPosition",
                      Variant(Vector2{m.pos_x, m.pos_y}));
        }
    });
}

Instance* scroll_frame_at(State& s, double x, double y, GuiNode* node) {
    Instance* hit = s.hit(x, y, int(s.last_width), int(s.last_height));
    Instance* frame = s.scroll_ancestor(hit);
    if (!frame)
        return nullptr;
    if (!s.node_for(frame, node, int(s.last_width), int(s.last_height)))
        return nullptr;
    return frame;
}

bool point_in(const GuiRect& r, double x, double y) {
    return x >= r.x && y >= r.y && x <= r.x + r.w && y <= r.y + r.h;
}

void add_elastic(State& s, Instance* frame, double dx, double dy) {
    Vector2& extra = s.elastic[frame];
    extra.x = std::clamp(extra.x + dx, -48.0, 48.0);
    extra.y = std::clamp(extra.y + dy, -48.0, 48.0);
}

// Positive dy scrolls toward the end of the canvas.  CanvasPosition is kept
// inside its legal range; ElasticBehavior only moves the visual content.
void scroll_by(lua_State* L, State& s, Instance* frame, double dx, double dy) {
    GuiNode node;
    if (!s.node_for(frame, &node, int(s.last_width), int(s.last_height)))
        return;
    const ScrollMetrics m = rbxclient::scrollMetrics(frame, node.rect);
    const double rate = [&] {
        const double r = num_prop(frame, "ScrollRate", 40.0);
        return r > 0.0 ? r : 40.0;
    }();
    const double want_x = m.pos_x + dx * rate;
    const double want_y = m.pos_y + dy * rate;
    const double clamped_x = std::clamp(want_x, 0.0, m.max_x);
    const double clamped_y = std::clamp(want_y, 0.0, m.max_y);
    // Enum.ElasticBehavior: WhenScrollable=0, Always=1, Never=2.
    const int elastic_mode = enum_prop(frame, "ElasticBehavior", 0);
    const bool elastic = elastic_mode == 1 ||
                         (elastic_mode == 0 &&
                          (m.max_x > 0.0 || m.max_y > 0.0));
    if (elastic) {
        if (std::abs(want_y - clamped_y) > 0.001)
            add_elastic(s, frame, 0.0, want_y - clamped_y);
        if (std::abs(want_x - clamped_x) > 0.001)
            add_elastic(s, frame, want_x - clamped_x, 0.0);
    }
    const Vector2 pos = vector_prop(frame, "CanvasPosition");
    if (!same(pos, Vector2{clamped_x, clamped_y})) {
        set_value(L, frame, "CanvasPosition",
                  Variant(Vector2{clamped_x, clamped_y}));
    }
    input::set_wheel_consumed(true);
}

bool begin_scroll_drag(State& s, Instance* frame, const GuiNode& node,
                       double x, double y) {
    const ScrollMetrics m = rbxclient::scrollMetrics(frame, node.rect);
    const bool v = point_in(m.v_thumb, x, y);
    const bool h = point_in(m.h_thumb, x, y);
    if (!v && !h)
        return false;
    const Vector2 pos = vector_prop(frame, "CanvasPosition");
    s.scroll_drag_active = true;
    s.scroll_drag.frame = Ref<Instance>(frame);
    s.scroll_drag.vertical = v;
    s.scroll_drag.start_mouse = v ? y : x;
    s.scroll_drag.start_position = v ? pos.y : pos.x;
    s.scroll_drag.track = v ? std::max(1.0, m.window_h - m.thickness)
                            : std::max(1.0, m.window_w - m.thickness);
    s.scroll_drag.thumb = v ? m.v_thumb.h : m.h_thumb.w;
    s.scroll_drag.maximum = v ? m.max_y : m.max_x;
    s.pressed = Ref<Instance>(frame);
    s.pressed_button = 1;
    sync_capture();
    return true;
}

void drag_scroll(lua_State* L, State& s, double x, double y) {
    if (!s.scroll_drag_active || !s.scroll_drag.frame)
        return;
    Instance* frame = s.scroll_drag.frame.get();
    GuiNode node;
    if (!s.node_for(frame, &node, int(s.last_width), int(s.last_height)))
        return;
    const double mouse = s.scroll_drag.vertical ? y : x;
    const double travel = std::max(1.0, s.scroll_drag.track - s.scroll_drag.thumb);
    const double delta = (mouse - s.scroll_drag.start_mouse) / travel *
                         s.scroll_drag.maximum;
    double pos_x = vector_prop(frame, "CanvasPosition").x;
    double pos_y = vector_prop(frame, "CanvasPosition").y;
    if (s.scroll_drag.vertical)
        pos_y = s.scroll_drag.start_position + delta;
    else
        pos_x = s.scroll_drag.start_position + delta;
    const ScrollMetrics m = rbxclient::scrollMetrics(frame, node.rect);
    set_value(L, frame, "CanvasPosition",
              Variant(Vector2{std::clamp(pos_x, 0.0, m.max_x),
                              std::clamp(pos_y, 0.0, m.max_y)}));
}

} // namespace

// ---------------------------------------------------------------------------
// Public lifecycle.

void reset() {
    State& s = state();
    s.root = nullptr;
    s.hovered = Ref<Instance>();
    s.pressed = Ref<Instance>();
    s.selected = Ref<Instance>();
    s.focused = Ref<Instance>();
    s.pressed_button = 0;
    s.last_x = s.last_y = -1.0;
    s.last_width = s.last_height = 0.0;
    s.time = 0.0;
    s.blink_accum = 0.0;
    s.blink_on = true;
    s.scroll_drag_active = false;
    s.scroll_drag = ScrollDrag{};
    s.texts.clear();
    s.view = GuiViewState{};
    input::set_pointer_captured(false);
    input::set_keyboard_captured(false);
    input::set_wheel_consumed(false);
}

void set_root(Instance* root) {
    if (state().root == root)
        return;
    reset();
    state().root = root;
}

const rbxclient::GuiViewState& view() { return state().view; }

bool pointer_captured() {
    State& s = state();
    return s.hovered || s.pressed || s.scroll_drag_active;
}

bool keyboard_captured() { return state().focused.get() != nullptr; }

Cursor cursor() {
    State& s = state();
    if (s.scroll_drag_active)
        return Cursor::Hand;
    if (!s.hovered)
        return Cursor::Arrow;
    if (s.hovered->is_a("TextBox"))
        return Cursor::IBeam;
    if (s.hovered->is_a("GuiButton") || s.hovered->is_a("ScrollingFrame"))
        return Cursor::Hand;
    return Cursor::Arrow;
}

void frame(lua_State* L, double dt, int width, int height) {
    State& s = state();
    s.lua = L;
    s.time += std::max(0.0, dt);
    input::set_wheel_consumed(false);
    if (width > 0)
        s.last_width = double(width);
    if (height > 0)
        s.last_height = double(height);
    if (s.last_width <= 0.0 || s.last_height <= 0.0) {
        const input::MouseState m = input::mouse();
        if (m.view_width > 0)
            s.last_width = double(m.view_width);
        if (m.view_height > 0)
            s.last_height = double(m.view_height);
    }

    if (s.focused) {
        s.blink_accum += std::max(0.0, dt);
        if (s.blink_accum >= kCaretBlink) {
            s.blink_accum = std::fmod(s.blink_accum, kCaretBlink);
            s.blink_on = !s.blink_on;
        }
    } else {
        s.blink_on = true;
        s.blink_accum = 0.0;
    }

    // Elastic overshoot is presentation-only and always returns to zero.
    for (auto it = s.elastic.begin(); it != s.elastic.end();) {
        it->second.x *= std::exp(-dt * 14.0);
        it->second.y *= std::exp(-dt * 14.0);
        if (std::abs(it->second.x) < 0.05 && std::abs(it->second.y) < 0.05)
            it = s.elastic.erase(it);
        else
            ++it;
    }

    if (s.root && s.last_x >= 0.0) {
        Instance* hit = s.hit(s.last_x, s.last_y, int(s.last_width),
                              int(s.last_height));
        s.update_hover(L, hit, s.last_x, s.last_y);
    }
    clamp_scrolling(L, int(s.last_width), int(s.last_height));
    sync_capture();
    s.sync_view();
}

namespace {

void fire_input(lua_State* L, Instance* target, const char* event,
                int input_type, int input_state, int keycode, double x,
                double y) {
    Ref<Instance> input_object =
        input_objects::make(L, input_type, input_state, keycode, x, y);
    if (!input_object)
        return;
    fire_lua(L, target, event,
             {Variant(static_cast<Instance*>(input_object.get()))});
}

int button_type(int button) {
    if (button == 1)
        return 0; // Enum.UserInputType.MouseButton1
    if (button == 2)
        return 1;
    if (button == 3)
        return 2;
    return 0;
}

} // namespace

bool dispatch(lua_State* L, const input::Event& ev) {
    State& s = state();
    s.lua = L;
    if (!s.root)
        return false;
    const input::MouseState mouse = input::mouse();
    if (s.last_width <= 0.0 && mouse.view_width > 0)
        s.last_width = double(mouse.view_width);
    if (s.last_height <= 0.0 && mouse.view_height > 0)
        s.last_height = double(mouse.view_height);

    switch (ev.type) {
    case input::EventType::MouseMove: {
        s.last_x = ev.x;
        s.last_y = ev.y;
        Instance* hit = s.hit(ev.x, ev.y, int(s.last_width), int(s.last_height));
        s.update_hover(L, hit, ev.x, ev.y);
        if (hit) {
            fire_lua(L, hit, "MouseMoved", {Variant(ev.x), Variant(ev.y)});
            fire_input(L, hit, "InputChanged", 4, 1, 0, ev.x, ev.y);
        }
        if (s.scroll_drag_active)
            drag_scroll(L, s, ev.x, ev.y);
        sync_capture();
        s.sync_view();
        return hit != nullptr || s.scroll_drag_active;
    }
    case input::EventType::MouseButtonDown: {
        s.last_x = ev.x;
        s.last_y = ev.y;
        Instance* raw = s.hit(ev.x, ev.y, int(s.last_width), int(s.last_height));
        s.update_hover(L, raw, ev.x, ev.y);
        if (!raw) {
            s.pressed = Ref<Instance>();
            s.pressed_button = 0;
            s.leave_focus(L, false);
            sync_capture();
            s.sync_view();
            return false;
        }
        Ref<Instance> target = Ref<Instance>(raw);

        // A scrollbar thumb is an overlay: it wins over child GuiObjects at
        // the same pixel and begins a drag instead of a click.
        Instance* scroll_frame = s.scroll_ancestor(raw);
        if (scroll_frame && ev.button == 1) {
            GuiNode scroll_node;
            if (s.node_for(scroll_frame, &scroll_node, int(s.last_width),
                           int(s.last_height)) &&
                begin_scroll_drag(s, scroll_frame, scroll_node, ev.x, ev.y)) {
                s.sync_view();
                return true;
            }
        }

        s.pressed = target;
        s.pressed_button = ev.button;
        const int type = button_type(ev.button);
        fire_input(L, target.get(), "InputBegan", type, 0, 0, ev.x, ev.y);
        if (ev.button == 1)
            fire_lua(L, target.get(), "MouseButton1Down",
                     {Variant(ev.x), Variant(ev.y)});
        else if (ev.button == 2)
            fire_lua(L, target.get(), "MouseButton2Down",
                     {Variant(ev.x), Variant(ev.y)});
        else if (ev.button == 3)
            fire_lua(L, target.get(), "MouseButton3Down",
                     {Variant(ev.x), Variant(ev.y)});

        if (target->is_a("TextBox")) {
            s.focus(L, target.get(), true, ev.x);
        } else {
            s.leave_focus(L, false);
            if (bool_prop(target.get(), "Selectable", false))
                s.set_selection(L, target.get());
            else
                s.set_selection(L, nullptr);
        }
        sync_capture();
        s.sync_view();
        return true;
    }
    case input::EventType::MouseButtonUp: {
        s.last_x = ev.x;
        s.last_y = ev.y;
        if (!s.pressed || s.pressed_button != ev.button)
            return false;
        Ref<Instance> target = s.pressed;
        const bool was_drag = s.scroll_drag_active;
        s.scroll_drag_active = false;
        s.scroll_drag = ScrollDrag{};
        s.pressed = Ref<Instance>();
        s.pressed_button = 0;

        const int type = button_type(ev.button);
        fire_input(L, target.get(), "InputEnded", type, 2, 0, ev.x, ev.y);
        if (ev.button == 1)
            fire_lua(L, target.get(), "MouseButton1Up",
                     {Variant(ev.x), Variant(ev.y)});
        else if (ev.button == 2)
            fire_lua(L, target.get(), "MouseButton2Up",
                     {Variant(ev.x), Variant(ev.y)});
        else if (ev.button == 3)
            fire_lua(L, target.get(), "MouseButton3Up",
                     {Variant(ev.x), Variant(ev.y)});

        Instance* hit = s.hit(ev.x, ev.y, int(s.last_width), int(s.last_height));
        if (!was_drag && hit == target.get()) {
            if (target->is_a("GuiButton")) {
                if (ev.button == 1) {
                    fire_lua(L, target.get(), "MouseButton1Click");
                    fire_lua(L, target.get(), "Activated");
                } else if (ev.button == 2) {
                    fire_lua(L, target.get(), "MouseButton2Click");
                    fire_lua(L, target.get(), "SecondaryActivated");
                }
            }
        }
        s.update_hover(L, hit, ev.x, ev.y);
        sync_capture();
        s.sync_view();
        return true;
    }
    case input::EventType::MouseWheel: {
        s.last_x = ev.x;
        s.last_y = ev.y;
        GuiNode node;
        Instance* frame_obj =
            scroll_frame_at(s, ev.x, ev.y, &node);
        if (frame_obj) {
            scroll_by(L, s, frame_obj, ev.wheel_x, -ev.wheel_y);
            Instance* hit = const_cast<Instance*>(s.hovered.get());
            if (ev.wheel_y > 0.0)
                fire_lua(L, hit, "MouseWheelForward");
            else if (ev.wheel_y < 0.0)
                fire_lua(L, hit, "MouseWheelBackward");
            s.sync_view();
            return true;
        }
        return false;
    }
    case input::EventType::KeyDown:
        return s.handle_key(L, ev);
    case input::EventType::KeyUp:
        return s.focused.get() != nullptr;
    case input::EventType::TextInput:
        return s.handle_text(L, ev);
    case input::EventType::TouchTap: {
        s.last_x = ev.x;
        s.last_y = ev.y;
        Instance* raw = s.hit(ev.x, ev.y, int(s.last_width), int(s.last_height));
        s.update_hover(L, raw, ev.x, ev.y);
        if (!raw) {
            s.leave_focus(L, false);
            s.set_selection(L, nullptr);
            return false;
        }
        Ref<Instance> target = Ref<Instance>(raw);
        fire_input(L, target.get(), "InputBegan", 7, 0, 0, ev.x, ev.y);
        fire_input(L, target.get(), "TouchTap", 7, 0, 0, ev.x, ev.y);
        if (target->is_a("TextBox")) {
            s.focus(L, target.get(), true, ev.x);
        } else {
            s.leave_focus(L, false);
            if (bool_prop(target.get(), "Selectable", false))
                s.set_selection(L, target.get());
            if (target->is_a("GuiButton")) {
                fire_lua(L, target.get(), "MouseButton1Down",
                         {Variant(ev.x), Variant(ev.y)});
                fire_lua(L, target.get(), "MouseButton1Up",
                         {Variant(ev.x), Variant(ev.y)});
                fire_lua(L, target.get(), "MouseButton1Click");
                fire_lua(L, target.get(), "Activated");
            }
        }
        fire_input(L, target.get(), "InputEnded", 7, 2, 0, ev.x, ev.y);
        s.sync_view();
        return true;
    }
    }
    return false;
}

bool capture_focus(lua_State* L, Instance* textbox) {
    if (!textbox || !textbox->is_a("TextBox"))
        return false;
    State& s = state();
    s.lua = L;
    s.focus(L, textbox, false, 0.0);
    return s.focused.get() == textbox;
}

void release_focus(lua_State* L, bool enter_pressed) {
    state().lua = L;
    state().leave_focus(L, enter_pressed);
}

bool textbox_focused(const Instance* textbox) {
    return state().focused.get() == textbox;
}

Instance* focused_textbox() { return state().focused.get(); }

namespace {

int m_capture_focus(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    lua_pushboolean(L, capture_focus(L, inst) ? 1 : 0);
    return 1;
}

int m_release_focus(lua_State* L) {
    Instance::check(L, 1);
    release_focus(L, false);
    return 0;
}

int m_is_focused(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    lua_pushboolean(L, textbox_focused(inst) ? 1 : 0);
    return 1;
}

int m_get_focused_textbox(lua_State* L) {
    Instance::check(L, 1);
    Instance* tb = focused_textbox();
    if (!tb) {
        lua_pushnil(L);
        return 1;
    }
    push_variant(L, Variant(tb));
    return 1;
}

} // namespace

void install_methods() {
    // The dump lacks TextBox.ReturnPressed (the native emits it for physical
    // Enter) and ScrollingFrame.ScrollBarImageVisible.  Keep generated.cpp
    // parity-clean and add the interactive surface at registration time.
    ensure_event("TextBox", "ReturnPressed");
    attach_property("ScrollingFrame",
                    PropInfo{"ScrollBarImageVisible", PropType::Bool, true,
                             false});
    attach_method("TextBox", "CaptureFocus", m_capture_focus);
    attach_method("TextBox", "ReleaseFocus", m_release_focus);
    attach_method("TextBox", "IsFocused", m_is_focused);
    attach_method("UserInputService", "GetFocusedTextBox",
                  m_get_focused_textbox);
}

} // namespace gui
} // namespace rbx
