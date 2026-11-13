// gui_input.h — the one owner of ScreenGui input routing.
//
// Layout/hit geometry comes from gui/gui_layout.h.  This component only owns
// interaction state (hover, press, focus, text selection, scroll drag) and
// turns platform input events into the Roblox GuiObject/TextBox signal
// surface.  The SDL viewer and the runtime's synthetic-input paths both feed
// the same dispatcher, so a headful session and unit tests exercise one path.
#pragma once

#include "gui/gui_layout.h"
#include "instance/host_input.h"

#include <map>
#include <string>

struct lua_State;

namespace rbx {
class Instance;

namespace gui {

enum class Cursor { Arrow, Hand, IBeam };

// Called by engine::attach / destroy.  Root is usually the DataModel.
void set_root(Instance* root);
void reset();

// Once per engine step before input events are dispatched.  `dt` drives caret
// blink and the elastic-scroll spring; `width/height` are the viewport.
void frame(lua_State* L, double dt, int width, int height);

// Route one queued platform event.  Returns true when the GUI consumed it.
bool dispatch(lua_State* L, const input::Event& event);

// Lua methods / UserInputService integration.
void install_methods();
bool capture_focus(lua_State* L, Instance* textbox);
void release_focus(lua_State* L, bool enter_pressed);
bool textbox_focused(const Instance* textbox);
Instance* focused_textbox();

const rbxclient::GuiViewState& view();
bool pointer_captured();
bool keyboard_captured();
Cursor cursor();

} // namespace gui
} // namespace rbx
