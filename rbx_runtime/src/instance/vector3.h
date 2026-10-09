// vector3.h — Vector3 value type: userdata + metatable + Vector3 class.
//
// Not a Luau built-in (the OSS VM has no vector library), so this is the
// Engine-profile stand-in: components are READ-ONLY (Roblox semantics), with
// new/zero constructors and the basics used by instance properties.
#pragma once

#include "instance/variant.h"

struct lua_State;

namespace rbx {

// Install the "Vector3" metatable + the `Vector3` global class (idempotent).
void create_vector3_class(lua_State* L);
// Push a Vector3 userdata (metatable "Vector3").
void push_vector3(lua_State* L, const Vector3& v);
// Check the value at idx; raises a Lua error on mismatch.
Vector3 check_vector3(lua_State* L, int idx);

} // namespace rbx
