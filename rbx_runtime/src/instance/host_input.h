// host_input.h — the embedder's real pointer/keyboard state.
//
// The Engine runtime cannot open a window or read SDL itself (it is also used
// headless and by the challenge path). The presentation layer publishes what
// the OS actually reports here; instance methods read that state and
// engine::step dispatches the queued transitions to Mouse/UserInputService.
//
// Headless runs leave the state invalid: Mouse exists, but its coordinates are
// 0 and no input signals fire. It must never invent a center position or a
// pressed key.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct lua_State;

namespace rbx {
namespace input {

enum class EventType {
    MouseMove,
    MouseButtonDown,
    MouseButtonUp,
    MouseWheel,
    KeyDown,
    KeyUp,
    TextInput,
    TouchTap,
};

struct Event {
    EventType type = EventType::MouseMove;
    double x = 0.0;
    double y = 0.0;
    int button = 0;  // 1, 2 or 3 (SDL button numbering)
    int keycode = 0; // Enum.KeyCode value / SDL keycode
    bool repeat = false;
    // Modifier state at the instant the platform produced this event.  A
    // frame can contain ctrl-down/A/ctrl-up; querying the live key set during
    // dispatch would see the final state and lose the chord.
    bool ctrl = false;
    bool shift = false;
    double wheel_x = 0.0; // horizontal wheel delta
    double wheel_y = 0.0; // vertical wheel delta
    std::string text;     // UTF-8 TextInput payload
};

struct MouseState {
    double x = 0.0;
    double y = 0.0;
    bool button1 = false;
    bool button2 = false;
    bool button3 = false;
    int view_width = 0;
    int view_height = 0;
    bool valid = false; // false until a presentation layer publishes a frame
};

// Publish the current pointer state. This enqueues Move and button transitions
// only; unchanged frames are free.
void publish_mouse(double x, double y, bool button1, bool button2,
                   bool button3, int view_width, int view_height);

// Publish one key transition. Repeat key-down events are not queued twice.
void publish_key(int keycode, bool down, bool repeat = false);
// UTF-8 text produced by the platform (SDL_EVENT_TEXT_INPUT).
void publish_text(const std::string& utf8);
// Wheel transition at the current pointer position.
void publish_wheel(double x, double y, double delta_x, double delta_y);
// A completed touch tap (finger down/up at the same point).
void publish_touch_tap(double x, double y);

// Release every held key (window focus lost / viewer shutdown).
void clear_keys();

// Automation mode: the live SDL backend stops publishing its own pointer,
// wheel, key and text samples so a deterministic driver owns the queue.
void set_test_mode(bool enabled);
bool test_mode();

MouseState mouse();
bool key_down(int keycode);

// Roblox Enum.ModifierKey values. The host publishes left/right KeyCodes as
// separate keys; a modifier is down when either physical side is held.
enum class Modifier { Shift = 0, Ctrl = 1, Alt = 2, Meta = 3 };
bool modifier_key_down(Modifier modifier);

// GUI arbitration: a consumed pointer must not reach the camera, and a
// focused TextBox owns keyboard text/movement keys.
void set_pointer_captured(bool captured);
bool pointer_captured();
void set_keyboard_captured(bool captured);
bool keyboard_captured();
void set_wheel_consumed(bool consumed);
bool wheel_consumed();

// Platform clipboard bridge (installed by the SDL viewer).  The runtime must
// not link SDL; an uninstalled bridge reads/writes an empty clipboard.
void set_clipboard_bridge(std::function<std::string()> get,
                          std::function<void(const std::string&)> set);
std::string clipboard_text();
void set_clipboard_text(const std::string& text);
std::vector<int> keys_down();
int last_input_type(); // Enum.UserInputType value (Keyboard = 8, etc.)

// Drain transitions since the last dispatch.
std::vector<Event> take_events();

// Fire Mouse and UserInputService signals for queued transitions. Called once
// from engine::step; no-op when the queue is empty.
void dispatch(lua_State* L);

// Clear all host state (used when a viewer opens/closes).
void reset();

} // namespace input
} // namespace rbx
