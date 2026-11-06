// exec.h — the lean executor surface installed into a live Engine state.
//
// Boblox owns its Luau VM, so nothing here injects: install() registers the
// UNC globals on the state the client already created, and run_source() feeds
// a user script through the same task scheduler the replicated scripts use.
//
// Where the surface cannot be backed by this client it says so instead of
// pretending: signals, raw VM internals and remote events have no equivalent
// in a client with no send path, so those entries are documented no-ops.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct lua_State;

namespace rbx {
class Instance;
namespace exec {

// The embedder's view of the live world: replication ids and the join
// schema's event/member indices. Without it the batch builders cannot address
// anything, so `boblox.fire`/`boblox.move` error clearly.
struct WorldView {
    void* ctx = nullptr;
    uint32_t (*ref_of)(void* ctx, rbx::Instance* inst) = nullptr;
    int (*event_index)(void* ctx, const char* cls, const char* ev) = nullptr;
    int (*member_index)(void* ctx, const char* cls, const char* member) = nullptr;
    rbx::Instance* (*local_humanoid)(void* ctx) = nullptr;
    // Called after a script-issued boblox.pos: the runtime keeps re-sending the
    // clocked upload at 2 Hz from that point until the RCC replicates our
    // character, so a spawn cannot be missed by stopping too early.
    void (*nudge_pos)(void* ctx, float x, float y, float z) = nullptr;
};
void set_world_view(const WorldView* view);
const WorldView* world_view();

// Register the executor globals on L. Call once, after engine::attach() so
// `game` exists and can be extended (HttpGet et al.). Idempotent.
void install(lua_State* L);

// Compile `src` and run it on a scheduler-owned thread (task.spawn), so
// task.wait/delay inside the script yield into the Engine scheduler instead
// of erroring across the C boundary. Returns false + *err on a compile or
// runtime error.
bool run_source(lua_State* L, const char* src, size_t size, std::string* err);

// Workspace root for the filesystem shims. Default $BOBLOX_WORKSPACE, else
// $HOME/.boblox/workspace. Created on first use.
const std::string& workspace_root();

// True once install() has run on this state.
bool installed(lua_State* L);

// Direction set by boblox.move(); consumed by the embedder each tick.
bool take_move(float out[3]);

// Absolute position requested by boblox.teleport(x,y,z); consumed per tick.
bool take_teleport(float out[3]);

// Scripts pushed from a socket. queue_source is thread-safe; pump_pending must
// be called on the Lua thread (the engine pump does it every tick).
void queue_source(std::string source);
int pump_pending(lua_State* L);

// Start the Unix-socket listener ($BOBLOX_SOCKET). Each connection is read to
// EOF and queued as one script; returns false when the path cannot bind.
bool start_socket(const std::string& path);

} // namespace exec
} // namespace rbx
