// session_config.h — one JSON object that describes a client session.
//
// A fleet worker should not have to compose a 400-character command line, and
// an embedder should not have to know which flags exist. The same fields are
// accepted from a `--config` file, from BOBLOX_* environment variables and
// from the command line, in that order of precedence:
//
//   defaults -> config file -> BOBLOX_* env -> command line
//
// Unknown keys are an error rather than a silent no-op: a typo in a fleet
// manifest must fail at startup, not produce a run that quietly ignored the
// option. Values are validated here too, so a config and a CLI invocation
// reject the same combinations.
#pragma once

#include <fstream>
#include <iterator>
#include <string>

#include <nlohmann/json.hpp>

#include "common/features.h"

namespace rbx {
namespace features {

struct SessionConfig {
    std::string profile = "full";
    std::string mode;      // empty = the profile's base
    std::string features;  // empty = the mode/profile's optional set
    std::string world;     // empty = the mode/profile's world
    int pump_ms = 2;       // RakNet loop period / input cadence floor
    long long seconds = 0; // 0 = run until interrupted
    std::string input = "native";
    std::string exec;
    std::string join_json;
    long long place_id = 0;
    std::string cookie_file;
    std::string job_id;
    std::string require_job;
    std::string save_join;
    // Retrying a world join is opt-in: a slow but healthy join can take a
    // while to link its Character, and a retry tears the whole session down.
    // -1 keeps the built-in default (0).
    int spawn_retry = -1;
    // Seconds a world session may go without a replicated Character before
    // the opt-in retry becomes eligible. Never fires after a successful link.
    int spawn_retry_after = 30;
};

namespace detail {

inline bool configString(const nlohmann::json& j, const char* key,
                         std::string* out, std::string* err) {
    if (!j.contains(key)) return true;
    const auto& v = j[key];
    if (!v.is_string()) {
        if (err) *err = std::string("config: '") + key + "' must be a string";
        return false;
    }
    *out = v.get<std::string>();
    return true;
}

inline bool configInt(const nlohmann::json& j, const char* key, long long* out,
                      std::string* err) {
    if (!j.contains(key)) return true;
    const auto& v = j[key];
    if (!v.is_number_integer()) {
        if (err) *err = std::string("config: '") + key + "' must be an integer";
        return false;
    }
    *out = v.get<long long>();
    return true;
}

inline bool knownKey(const std::string& key) {
    static const char* kKeys[] = {
        "profile",   "mode",       "features",  "world",
        "pump_ms",   "seconds",    "input",     "exec",
        "join_json", "place_id",   "cookie_file", "job_id",
        "require_job", "save_join", "spawn_retry", "spawn_retry_after",
    };
    for (const char* k : kKeys)
        if (key == k) return true;
    return false;
}

} // namespace detail

inline bool parseSessionConfig(const std::string& text, SessionConfig* out,
                               std::string* err) {
    if (!out) return false;
    using nlohmann::json;
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        if (err) *err = "config: not valid JSON";
        return false;
    }
    if (!j.is_object()) {
        if (err) *err = "config: top level must be a JSON object";
        return false;
    }
    SessionConfig c;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!detail::knownKey(it.key())) {
            if (err) *err = "config: unknown key '" + it.key() + "'";
            return false;
        }
    }
    if (!detail::configString(j, "profile", &c.profile, err) ||
        !detail::configString(j, "mode", &c.mode, err) ||
        !detail::configString(j, "world", &c.world, err) ||
        !detail::configString(j, "input", &c.input, err) ||
        !detail::configString(j, "exec", &c.exec, err) ||
        !detail::configString(j, "join_json", &c.join_json, err) ||
        !detail::configString(j, "cookie_file", &c.cookie_file, err) ||
        !detail::configString(j, "job_id", &c.job_id, err) ||
        !detail::configString(j, "require_job", &c.require_job, err) ||
        !detail::configString(j, "save_join", &c.save_join, err))
        return false;

    if (j.contains("features")) {
        const auto& f = j["features"];
        if (f.is_string()) {
            c.features = f.get<std::string>();
        } else if (f.is_array()) {
            c.features.clear();
            for (const auto& item : f) {
                if (!item.is_string()) {
                    if (err)
                        *err = "config: 'features' must be a string or an "
                               "array of strings";
                    return false;
                }
                if (!c.features.empty()) c.features += ",";
                c.features += item.get<std::string>();
            }
        } else {
            if (err)
                *err = "config: 'features' must be a string or an array of "
                       "strings";
            return false;
        }
    }

    long long pump_ms = c.pump_ms;
    long long seconds = c.seconds;
    long long place_id = c.place_id;
    long long spawn_retry = c.spawn_retry;
    long long spawn_retry_after = c.spawn_retry_after;
    if (!detail::configInt(j, "pump_ms", &pump_ms, err) ||
        !detail::configInt(j, "seconds", &seconds, err) ||
        !detail::configInt(j, "place_id", &place_id, err) ||
        !detail::configInt(j, "spawn_retry", &spawn_retry, err) ||
        !detail::configInt(j, "spawn_retry_after", &spawn_retry_after, err))
        return false;
    if (pump_ms < 1) {
        if (err) *err = "config: 'pump_ms' must be >= 1";
        return false;
    }
    if (place_id < 0) {
        if (err) *err = "config: 'place_id' must be >= 0";
        return false;
    }
    if (spawn_retry < -1) {
        if (err) *err = "config: 'spawn_retry' must be >= 0";
        return false;
    }
    if (spawn_retry_after < 1) {
        if (err) *err = "config: 'spawn_retry_after' must be >= 1 second";
        return false;
    }
    c.pump_ms = int(pump_ms);
    c.seconds = seconds;
    c.place_id = place_id;
    c.spawn_retry = int(spawn_retry);
    c.spawn_retry_after = int(spawn_retry_after);
    *out = c;
    return true;
}

inline bool loadSessionConfig(const std::string& path, SessionConfig* out,
                              std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) *err = "config: cannot open " + path;
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    std::string local;
    if (!parseSessionConfig(text, out, &local)) {
        if (err) *err = path + ": " + local;
        return false;
    }
    return true;
}

// profile -> mode -> explicit feature list -> world override. The explicit
// list wins for luau/render/gui; the world stays a separate dimension.
inline bool applySessionConfig(const SessionConfig& c, Set* out,
                               std::string* err) {
    if (!preset(out, c.profile, err)) return false;
    if (!c.mode.empty() && !applyMode(out, c.mode, err)) return false;
    if (!c.features.empty() && !applyList(out, c.features, err)) return false;
    if (!c.world.empty()) {
        if (!applyWorld(out, c.world, err)) return false;
        // An explicit world=none is the count-only base, not "drop the world
        // and keep a renderer/Luau VM that has nothing to draw or run". A
        // later CLI --render still re-enables it and fails validation, so the
        // conflict is visible rather than silently ignored.
        if (out->world == WorldMode::None)
            out->luau = out->render = out->gui = out->audio = false;
    }
    return validate(*out, err);
}

inline std::string sessionConfigJson(const SessionConfig& c,
                                     const Set* resolved = nullptr) {
    nlohmann::json j;
    j["profile"] = c.profile;
    j["mode"] = c.mode;
    j["features"] = c.features;
    j["world"] = c.world;
    j["pump_ms"] = c.pump_ms;
    j["seconds"] = c.seconds;
    j["input"] = c.input;
    j["exec"] = c.exec;
    j["join_json"] = c.join_json;
    j["place_id"] = c.place_id;
    j["cookie_file"] = c.cookie_file;
    j["job_id"] = c.job_id;
    j["require_job"] = c.require_job;
    j["save_join"] = c.save_join;
    j["spawn_retry"] = c.spawn_retry;
    j["spawn_retry_after"] = c.spawn_retry_after;
    if (resolved) {
        j["resolved"] = {
            {"world", worldName(resolved->world)},
            {"luau", resolved->luau},
            {"render", resolved->render},
            {"gui", resolved->gui},
            {"audio", resolved->audio},
            {"physics", resolved->physics == PhysicsMode::Client
                             ? "client"
                         : resolved->physics == PhysicsMode::Server ? "server"
                                                                    : "off"},
        };
    }
    return j.dump(2);
}

} // namespace features
} // namespace rbx
