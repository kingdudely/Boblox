// lua_methods.cpp — per-state Lua-implemented Instance methods.
//
// Why Lua, not C: some behaviors are poll-and-yield loops (WaitForChild),
// and C cannot loop across yields — Luau's lua_yield only propagates outward
// (there are no continuations; a C function never resumes mid-body). In Lua,
// loops and task.wait compose naturally, the scheduler stays untouched, and
// timeouts accumulate deterministically off task.wait's return values
// (virtual clock — exact under engine::step).
//
// The chunk returns the methods table; install_lua_methods compiles it once
// per environment (luau_compile — Luau.Compiler is linked) and anchors it in
// the registry ("rbx.LuaMethods"). inst_index consults it (shadowing C
// stubs), so these behave exactly like registry methods. Installed from
// register_engine ONLY — the Challenge profile never sees it (frozen).
#include "instance/lua_methods.h"

#include "lua.h"
#include "lualib.h"
#include "luacode.h" // luau_compile (Luau.Compiler)

#include <cstdlib>
#include <cstring>

namespace rbx {

namespace {

// Poll slices match the scheduler's granularity for change signals.
const char* kMethodsChunk = R"(
local methods = {}

-- WaitForChild(name[, timeout]): block (yielding) until a child appears.
-- Main-thread callers fail the same way task.wait would (Luau restriction).
function methods.WaitForChild(self, name, timeout)
    local child = self:FindFirstChild(name)
    if child ~= nil then
        return child
    end
    local waited = 0.0
    while true do
        if timeout ~= nil and waited >= timeout then
            return nil
        end
        waited = waited + task.wait(0.03)
        child = self:FindFirstChild(name)
        if child ~= nil then
            return child
        end
    end
end

return methods
)";

} // namespace

void install_lua_methods(lua_State* L) {
    lua_CompileOptions co;
    std::memset(&co, 0, sizeof(co)); // same as Luau's REPL defaults
    size_t size = 0;
    char* bc = luau_compile(kMethodsChunk, std::strlen(kMethodsChunk), &co, &size);
    if (!bc)
        luaL_error(L, "lua_methods: chunk failed to compile");
    const int loaded = luau_load(L, "=luamethods", bc, size, 0);
    std::free(bc); // luau_compile allocates with malloc
    if (loaded != 0) {
        luaL_error(L, "lua_methods: chunk failed to load");
        return;
    }
    if (lua_pcall(L, 0, 1, 0) != 0) {
        luaL_error(L, "lua_methods: chunk failed to run");
        return;
    }
    // [methods table]: anchor per state for inst_index.
    lua_setfield(L, LUA_REGISTRYINDEX, "rbx.LuaMethods");
}

} // namespace rbx
