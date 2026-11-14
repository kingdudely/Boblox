// teleport.cpp — see teleport.h. State is deliberately process-wide: a
// teleport replaces the Lua environment, so a registry/anchor in the old state
// would disappear exactly when the destination needs it.
#include "instance/teleport.h"

#include "instance/instance.h"
#include "instance/signal.h"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <utility>

namespace rbx::teleport {
namespace {

Handler g_handler;
bool g_in_progress = false;
bool g_cancelled = false;
std::optional<nlohmann::json> g_arrival;
bool g_arrived = false;
bool g_arrival_delivered = false;

std::int64_t g_current_place = 0;
std::int64_t g_current_universe = 0;
std::map<std::string, nlohmann::json> g_settings;
std::vector<std::int64_t> g_places_history;
std::vector<std::int64_t> g_universes_history;

constexpr int kMaxDepth = 32;
constexpr std::size_t kMaxEntries = 10000;

const char* lua_type_name(lua_State* L, int index) {
    switch (lua_type(L, index)) {
    case LUA_TNIL:
        return "nil";
    case LUA_TBOOLEAN:
        return "boolean";
    case LUA_TNUMBER:
        return "number";
    case LUA_TSTRING:
        return "string";
    case LUA_TFUNCTION:
        return "function";
    case LUA_TTHREAD:
        return "thread";
    case LUA_TTABLE:
        return "table";
    case LUA_TUSERDATA:
        return "userdata";
    default:
        return "value";
    }
}

bool lua_to_json_impl(lua_State* L, int index, nlohmann::json* out, int depth,
                      std::set<const void*>* seen, std::string* error) {
    index = lua_absindex(L, index);
    const int type = lua_type(L, index);
    switch (type) {
    case LUA_TNIL:
        *out = nullptr;
        return true;
    case LUA_TBOOLEAN:
        *out = lua_toboolean(L, index) != 0;
        return true;
    case LUA_TNUMBER: {
        const double value = lua_tonumber(L, index);
        if (!std::isfinite(value)) {
            if (error)
                *error = "TeleportData contains number which is unsupported";
            return false;
        }
        *out = value;
        return true;
    }
    case LUA_TSTRING: {
        std::size_t len = 0;
        const char* value = lua_tolstring(L, index, &len);
        *out = std::string(value, len);
        return true;
    }
    case LUA_TTABLE: {
        if (depth >= kMaxDepth) {
            if (error)
                *error = "TeleportData exceeds maximum nesting depth or size. "
                         "This will be blocked in a future update.";
            return false;
        }
        const void* identity = lua_topointer(L, index);
        if (identity && !seen->insert(identity).second) {
            if (error)
                *error =
                    "TeleportData contains table which is unsupported";
            return false;
        }

        std::size_t count = 0;
        bool all_array = true;
        bool all_object = true;
        std::size_t max_index = 0;
        lua_pushnil(L);
        while (lua_next(L, index) != 0) {
            if (++count > kMaxEntries) {
                lua_pop(L, 2);
                if (identity)
                    seen->erase(identity);
                if (error)
                    *error = "TeleportData exceeds maximum nesting depth or "
                             "size. This will be blocked in a future update.";
                return false;
            }
            if (lua_type(L, -2) == LUA_TNUMBER) {
                all_object = false;
                const double key = lua_tonumber(L, -2);
                const auto integer = static_cast<std::size_t>(key);
                if (!std::isfinite(key) || key < 1.0 ||
                    static_cast<double>(integer) != key) {
                    all_array = false;
                } else {
                    max_index = std::max(max_index, integer);
                }
            } else if (lua_type(L, -2) == LUA_TSTRING) {
                all_array = false;
            } else {
                all_array = false;
                all_object = false;
            }
            lua_pop(L, 1);
        }
        if (all_array && max_index != count)
            all_array = false;

        bool ok = true;
        if (all_array) {
            nlohmann::json array = nlohmann::json::array();
            for (std::size_t i = 1; i <= count; ++i) {
                lua_rawgeti(L, index, static_cast<int>(i));
                nlohmann::json value;
                ok = lua_to_json_impl(L, -1, &value, depth + 1, seen, error);
                lua_pop(L, 1);
                if (!ok)
                    break;
                array.push_back(std::move(value));
            }
            *out = std::move(array);
        } else if (all_object) {
            nlohmann::json object = nlohmann::json::object();
            lua_pushnil(L);
            while (lua_next(L, index) != 0) {
                std::size_t key_len = 0;
                const char* key = lua_tolstring(L, -2, &key_len);
                if (!key) {
                    lua_pop(L, 2);
                    ok = false;
                    if (error)
                        *error = "TeleportData contains table which is "
                                 "unsupported";
                    break;
                }
                nlohmann::json value;
                ok = lua_to_json_impl(L, -1, &value, depth + 1, seen, error);
                if (ok)
                    object[std::string(key, key_len)] = std::move(value);
                lua_pop(L, 1);
                if (!ok) {
                    lua_pop(L, 1);
                    break;
                }
            }
            if (ok)
                *out = std::move(object);
        } else {
            if (error)
                *error = "TeleportData contains table which is unsupported";
            ok = false;
        }
        if (identity)
            seen->erase(identity);
        return ok;
    }
    default:
        if (error) {
            *error = std::string("TeleportData contains ") +
                     lua_type_name(L, index) + " which is unsupported";
        }
        return false;
    }
}

Instance* find_teleport_service(Instance* root) {
    if (!root)
        return nullptr;
    for (const auto& child : root->children()) {
        if (child->is_a("TeleportService"))
            return child.get();
    }
    return nullptr;
}

} // namespace

void set_handler(Handler handler) {
    g_handler = std::move(handler);
}

void clear_handler() {
    g_handler = nullptr;
}

bool begin(const Request& request, std::string* error) {
    if (g_in_progress) {
        if (error) {
            *error = "The previous teleport is in processing "
                     "(IsTeleporting)";
        }
        return false;
    }
    if (!g_handler) {
        if (error)
            *error = "Teleport is not available in this environment";
        return false;
    }
    if (!g_handler(request, error))
        return false;
    g_in_progress = true;
    g_cancelled = false;
    if (g_current_place > 0) {
        g_places_history.push_back(g_current_place);
        g_universes_history.push_back(g_current_universe);
    }
    // Starting a new teleport invalidates the previous arrival data.
    g_arrival.reset();
    g_arrived = false;
    g_arrival_delivered = false;
    return true;
}

bool in_progress() {
    return g_in_progress;
}

void complete_arrival(const nlohmann::json& data) {
    g_in_progress = false;
    g_cancelled = false;
    g_arrival = data;
    g_arrived = true;
    g_arrival_delivered = false;
}

void fail(const std::string&) {
    g_in_progress = false;
    g_cancelled = false;
}

void cancel() {
    g_cancelled = true;
    g_in_progress = false;
}

bool consume_cancelled() {
    const bool cancelled = g_cancelled;
    g_cancelled = false;
    return cancelled;
}

const nlohmann::json* arriving_data() {
    return g_arrival ? &*g_arrival : nullptr;
}

bool arrived() {
    return g_arrived;
}

void set_current_place(std::int64_t place_id, std::int64_t universe_id) {
    g_current_place = place_id;
    g_current_universe = universe_id;
}

std::int64_t current_place() {
    return g_current_place;
}

std::int64_t current_universe() {
    return g_current_universe;
}

void deliver_arrival(lua_State* L, Instance* root) {
    if (!L || !root || !g_arrived || g_arrival_delivered)
        return;
    Instance* service = find_teleport_service(root);
    if (!service)
        return;
    Signal* signal = service->event_signal("LocalPlayerArrivedFromTeleport");
    lua_pushnil(L); // loadingGui: this build exposes no arriving GUI
    push_json(L, g_arrival ? *g_arrival : nlohmann::json(nullptr));
    signal->fire_lua(L, 2);
    lua_pop(L, 2);
    g_arrival_delivered = true;
}

void reset_session_state() {
    g_in_progress = false;
    g_cancelled = false;
    g_arrival.reset();
    g_arrived = false;
    g_arrival_delivered = false;
}

void reset_for_tests() {
    g_handler = nullptr;
    g_in_progress = false;
    g_cancelled = false;
    g_arrival.reset();
    g_arrived = false;
    g_arrival_delivered = false;
    g_current_place = 0;
    g_current_universe = 0;
    g_settings.clear();
    g_places_history.clear();
    g_universes_history.clear();
}

bool lua_to_json(lua_State* L, int index, nlohmann::json* out,
                 std::string* error) {
    std::set<const void*> seen;
    return lua_to_json_impl(L, index, out, 0, &seen, error);
}

void push_json(lua_State* L, const nlohmann::json& value) {
    if (value.is_null()) {
        lua_pushnil(L);
    } else if (value.is_boolean()) {
        lua_pushboolean(L, value.get<bool>() ? 1 : 0);
    } else if (value.is_number()) {
        lua_pushnumber(L, value.get<double>());
    } else if (value.is_string()) {
        const std::string text = value.get<std::string>();
        lua_pushlstring(L, text.data(), text.size());
    } else if (value.is_array()) {
        lua_createtable(L, static_cast<int>(value.size()), 0);
        int i = 1;
        for (const auto& element : value) {
            push_json(L, element);
            lua_rawseti(L, -2, i++);
        }
    } else if (value.is_object()) {
        lua_createtable(L, 0, static_cast<int>(value.size()));
        for (auto it = value.begin(); it != value.end(); ++it) {
            lua_pushlstring(L, it.key().data(), it.key().size());
            push_json(L, it.value());
            lua_rawset(L, -3);
        }
    } else {
        lua_pushnil(L);
    }
}

nlohmann::json* find_setting(const std::string& key) {
    auto it = g_settings.find(key);
    return it == g_settings.end() ? nullptr : &it->second;
}

void set_setting(const std::string& key, nlohmann::json value) {
    g_settings[key] = std::move(value);
}

const std::vector<std::int64_t>& places_history() {
    return g_places_history;
}

const std::vector<std::int64_t>& universes_history() {
    return g_universes_history;
}

} // namespace rbx::teleport
