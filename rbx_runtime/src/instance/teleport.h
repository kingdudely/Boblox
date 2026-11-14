// teleport.h — TeleportService's process-wide teleport state.
//
// The native client carries the teleport request and its `teleportData`
// through the session transition in-process: Teleport() starts a normal join
// with isTeleport=true, the old DataModel is torn down, and the destination
// DataModel receives the same data table. Keeping that state here (instead of
// in a Lua registry) makes it survive the environment replacement and lets the
// network host drive the actual transition without teaching the runtime about
// join tickets or sockets.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct lua_State;

namespace rbx {

struct Instance;

namespace teleport {

struct Request {
    std::int64_t place_id = 0;
    std::string instance_id; // job / private server instance, empty for a normal join
    std::string spawn_name;
    std::int64_t source_place_id = 0;
    std::int64_t universe_id = 0;
    bool has_data = false;
    nlohmann::json data = nullptr;
};

// Installed by the live client. Returns false + an error message when the
// transition cannot be started; true means the request was accepted and the
// client will tear down the current session and join the target.
using Handler = std::function<bool(const Request&, std::string* error)>;

void set_handler(Handler handler);
void clear_handler();

// TeleportService calls this after validating its arguments. A second request
// while `in_progress()` is true is the native IsTeleporting failure and must
// not reach the handler.
bool begin(const Request& request, std::string* error);
bool in_progress();

// Called by the live client once the destination DataModel exists, and on a
// failed transition. `complete_arrival` leaves the data available for the
// whole destination session, exactly like GetLocalPlayerTeleportData.
void complete_arrival(const nlohmann::json& data);
void fail(const std::string& error);
void cancel();
bool consume_cancelled();

const nlohmann::json* arriving_data();
bool arrived();

void set_current_place(std::int64_t place_id, std::int64_t universe_id);
std::int64_t current_place();
std::int64_t current_universe();

// Fires TeleportService.LocalPlayerArrivedFromTeleport on the destination
// DataModel exactly once. No-op when this session did not arrive by teleport.
void deliver_arrival(lua_State* L, Instance* root);

// Replaces the data model around the transition without touching the
// settings/history, which the native keeps for the process lifetime.
void reset_session_state();

// Test hook: clears handler, transition state, settings and history.
void reset_for_tests();

// -- Lua <-> teleport-data conversion ---------------------------------------
// Only values the native's teleport boundary carries are accepted; functions,
// threads and Instances are reported as unsupported instead of being silently
// dropped. `SetTeleportSetting` uses the same conversion.
bool lua_to_json(lua_State* L, int index, nlohmann::json* out,
                 std::string* error);
void push_json(lua_State* L, const nlohmann::json& value);

// Settings are keyed by string and keep their JSON shape across the session.
nlohmann::json* find_setting(const std::string& key);
void set_setting(const std::string& key, nlohmann::json value);
const std::vector<std::int64_t>& places_history();
const std::vector<std::int64_t>& universes_history();

} // namespace teleport
} // namespace rbx
