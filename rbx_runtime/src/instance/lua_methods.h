// lua_methods.h — per-state Lua-implemented Instance methods (see .cpp for why).
#pragma once

struct lua_State;

namespace rbx {

// Compile the methods chunk and anchor its table in the registry
// ("rbx.LuaMethods") for inst_index. Engine profile only (called from
// register_engine — never on the Challenge/frozen path).
void install_lua_methods(lua_State* L);

} // namespace rbx
