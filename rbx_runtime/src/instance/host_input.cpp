// host_input.cpp — see host_input.h.
#include "instance/host_input.h"

#include <algorithm>
#include <mutex>
#include <set>

namespace rbx {
namespace input {
namespace {

std::mutex g_mu;
MouseState g_mouse;
std::set<int> g_keys;
std::vector<Event> g_events;
int g_last_input = 8; // Enum.UserInputType.Keyboard
bool g_pointer_captured = false;
bool g_keyboard_captured = false;
bool g_wheel_consumed = false;
bool g_test_mode = false;
std::function<std::string()> g_clipboard_get;
std::function<void(const std::string&)> g_clipboard_set;

void queue(Event e) {
    g_events.push_back(std::move(e));
}

} // namespace

void publish_mouse(double x, double y, bool b1, bool b2, bool b3,
                   int view_width, int view_height) {
    std::lock_guard<std::mutex> lock(g_mu);
    const bool moved = !g_mouse.valid || x != g_mouse.x || y != g_mouse.y;
    if (moved) {
        Event e;
        e.type = EventType::MouseMove;
        e.x = x;
        e.y = y;
        queue(std::move(e));
    }
    const bool old[3] = {g_mouse.button1, g_mouse.button2, g_mouse.button3};
    const bool now[3] = {b1, b2, b3};
    for (int i = 0; i < 3; ++i) {
        if (old[i] == now[i])
            continue;
        Event e;
        e.type = now[i] ? EventType::MouseButtonDown
                        : EventType::MouseButtonUp;
        e.x = x;
        e.y = y;
        e.button = i + 1;
        queue(std::move(e));
        g_last_input = i; // MouseButton1/2/3
    }
    if (moved)
        g_last_input = 4; // MouseMovement
    g_mouse.x = x;
    g_mouse.y = y;
    g_mouse.button1 = b1;
    g_mouse.button2 = b2;
    g_mouse.button3 = b3;
    g_mouse.view_width = std::max(0, view_width);
    g_mouse.view_height = std::max(0, view_height);
    g_mouse.valid = true;
}

void publish_key(int keycode, bool down, bool repeat) {
    std::lock_guard<std::mutex> lock(g_mu);
    const bool was = g_keys.count(keycode) != 0;
    if (down == was)
        return;
    const bool ctrl = g_keys.count(305) || g_keys.count(306) ||
                      (down && (keycode == 305 || keycode == 306));
    const bool shift = g_keys.count(303) || g_keys.count(304) ||
                       (down && (keycode == 303 || keycode == 304));
    if (down) {
        g_keys.insert(keycode);
    } else {
        g_keys.erase(keycode);
    }
    Event e;
    e.type = down ? EventType::KeyDown : EventType::KeyUp;
    e.keycode = keycode;
    e.repeat = repeat;
    e.ctrl = ctrl;
    e.shift = shift;
    queue(std::move(e));
    g_last_input = 8; // Keyboard
}

void publish_text(const std::string& utf8) {
    if (utf8.empty())
        return;
    std::lock_guard<std::mutex> lock(g_mu);
    Event e;
    e.type = EventType::TextInput;
    e.text = utf8;
    e.ctrl = g_keys.count(305) || g_keys.count(306);
    e.shift = g_keys.count(303) || g_keys.count(304);
    queue(std::move(e));
    g_last_input = 8;
}

void publish_wheel(double x, double y, double delta_x, double delta_y) {
    std::lock_guard<std::mutex> lock(g_mu);
    Event e;
    e.type = EventType::MouseWheel;
    e.x = x;
    e.y = y;
    e.wheel_x = delta_x;
    e.wheel_y = delta_y;
    queue(std::move(e));
    g_last_input = 4; // MouseMovement
}

void publish_touch_tap(double x, double y) {
    std::lock_guard<std::mutex> lock(g_mu);
    Event e;
    e.type = EventType::TouchTap;
    e.x = x;
    e.y = y;
    queue(std::move(e));
    g_last_input = 7; // Enum.UserInputType.Touch
}

void set_test_mode(bool enabled) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_test_mode = enabled;
}

bool test_mode() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_test_mode;
}

void clear_keys() {
    std::lock_guard<std::mutex> lock(g_mu);
    for (int key : g_keys) {
        Event e;
        e.type = EventType::KeyUp;
        e.keycode = key;
        queue(std::move(e));
    }
    g_keys.clear();
}

MouseState mouse() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_mouse;
}

bool key_down(int keycode) {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_keys.count(keycode) != 0;
}

std::vector<int> keys_down() {
    std::lock_guard<std::mutex> lock(g_mu);
    return std::vector<int>(g_keys.begin(), g_keys.end());
}

void set_pointer_captured(bool captured) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_pointer_captured = captured;
}

bool pointer_captured() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_pointer_captured;
}

void set_keyboard_captured(bool captured) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_keyboard_captured = captured;
}

bool keyboard_captured() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_keyboard_captured;
}

void set_wheel_consumed(bool consumed) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_wheel_consumed = consumed;
}

bool wheel_consumed() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_wheel_consumed;
}

void set_clipboard_bridge(std::function<std::string()> get,
                          std::function<void(const std::string&)> set) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_clipboard_get = std::move(get);
    g_clipboard_set = std::move(set);
}

std::string clipboard_text() {
    std::function<std::string()> get;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        get = g_clipboard_get;
    }
    return get ? get() : std::string();
}

void set_clipboard_text(const std::string& text) {
    std::function<void(const std::string&)> set;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        set = g_clipboard_set;
    }
    if (set)
        set(text);
}

int last_input_type() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_last_input;
}

std::vector<Event> take_events() {
    std::lock_guard<std::mutex> lock(g_mu);
    std::vector<Event> out;
    out.swap(g_events);
    return out;
}

void reset() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_mouse = MouseState{};
    g_keys.clear();
    g_events.clear();
    g_pointer_captured = false;
    g_keyboard_captured = false;
    g_wheel_consumed = false;
    g_last_input = 8;
}

} // namespace input
} // namespace rbx
