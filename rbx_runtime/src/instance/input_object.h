// input_object.h — shared construction of the transient InputObject that
// UserInputService, GuiObject and TextBox events receive.
#pragma once

#include "instance/instance.h"

struct lua_State;

namespace rbx {
namespace input_objects {

// InputObject with UserInputType/State/KeyCode/Position/Delta set.  The caller
// holds the returned Ref across the signal fire so Lua handlers can retain it.
Ref<Instance> make(lua_State* L, int input_type, int input_state, int keycode,
                   double x, double y);
} // namespace input_objects
} // namespace rbx
