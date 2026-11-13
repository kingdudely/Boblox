// features.h — the local feature set shared by the client entry points.
//
// This is deliberately about *local* cost, not the wire protocol. Transport,
// join and (unless it is switched off) replication parsing are always on:
//
//   world   apply the replicated world at all (join-only vs a DataModel)
//   luau    run replicated LocalScript/ModuleScript chunks
//   render  open the SDL/GL viewer and draw the world
//   gui     install PlayerGui/CurrentCamera for scripts
//
// `world` is the biggest lever a fleet has. A join-only session keeps the
// RakNet/join/challenge handshake and the liveness streams but never builds a
// DataModel, never parses record batches and never allocates the Engine, which
// is where nearly all of the per-session memory goes. Luau and the GUI tree
// need a world; asking for either without one is rejected instead of silently
// paying for a VM that has nothing to run.
//
// physics is a mode, not a toggle. `client` runs a Jolt CharacterVirtual and
// the 2016 camera locally (the default). `server` keeps the replicated world
// but no local solver; `off` records that the caller wants no local solver.
// Neither mode transfers ownership (SetNetworkOwner is a server-only API).
#pragma once

#include <cctype>
#include <string>

namespace rbx {
namespace features {

enum class PhysicsMode { Client, Server, Off };
enum class WorldMode { None, Full };

struct Set {
    WorldMode world = WorldMode::Full;
    bool luau = true;
    bool render = true;
    bool gui = true;
    bool audio = true;
    PhysicsMode physics = PhysicsMode::Client;
};

inline const char* onOff(bool on) { return on ? "on" : "off"; }
inline const char* worldName(WorldMode w) {
    return w == WorldMode::None ? "none" : "full";
}

inline std::string trim(const std::string& in) {
    size_t b = 0;
    while (b < in.size() && std::isspace(static_cast<unsigned char>(in[b]))) ++b;
    size_t e = in.size();
    while (e > b && std::isspace(static_cast<unsigned char>(in[e - 1]))) --e;
    return in.substr(b, e - b);
}

inline bool lowerIs(const std::string& s, const char* word) {
    size_t i = 0;
    for (; i < s.size() && word[i]; ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != word[i]) return false;
    return i == s.size() && word[i] == '\0';
}

// full: the normal client.
// minimal: replication-only base (headless, no Luau chunks, no PlayerGui).
// remote: alias for the same low-cost base, named for automation runs.
// join: handshake and liveness only; no world is built or parsed.
inline bool preset(Set* out, const std::string& profile, std::string* err) {
    if (!out) return false;
    Set s;
    if (lowerIs(profile, "full")) {
        s = Set{};
    } else if (lowerIs(profile, "minimal") || lowerIs(profile, "remote")) {
        s.world = WorldMode::Full;
        s.luau = false;
        s.render = false;
        s.gui = false;
        s.audio = false;
        s.physics = PhysicsMode::Client;
    } else if (lowerIs(profile, "join") || lowerIs(profile, "count")) {
        s.world = WorldMode::None;
        s.luau = false;
        s.render = false;
        s.gui = false;
        s.audio = false;
        s.physics = PhysicsMode::Client;
    } else {
        if (err)
            *err = "--profile must be full, minimal, remote or join, got " + profile;
        return false;
    }
    *out = s;
    return true;
}

// Coarse sessions, each one a complete base. A mode is what a fleet worker
// names itself: join/count, world, script or full. `--features` and the
// positive/negative switches refine the result afterwards.
inline bool applyMode(Set* out, const std::string& mode, std::string* err) {
    if (!out) return false;
    Set s;
    if (lowerIs(mode, "join") || lowerIs(mode, "count") || lowerIs(mode, "none")) {
        s.world = WorldMode::None;
        s.luau = false;
        s.render = false;
        s.gui = false;
        s.audio = false;
    } else if (lowerIs(mode, "world") || lowerIs(mode, "minimal") ||
               lowerIs(mode, "remote")) {
        s.world = WorldMode::Full;
        s.luau = false;
        s.render = false;
        s.gui = false;
        s.audio = false;
    } else if (lowerIs(mode, "script") || lowerIs(mode, "scripts")) {
        s.world = WorldMode::Full;
        s.luau = true;
        s.render = false;
        s.gui = false;
        s.audio = false;
    } else if (lowerIs(mode, "full")) {
        s = Set{};
    } else {
        if (err)
            *err = "--mode must be join, world, script or full, got " + mode;
        return false;
    }
    *out = s;
    return true;
}

inline bool applyWorld(Set* out, const std::string& spec, std::string* err) {
    if (!out) return false;
    if (lowerIs(spec, "none") || lowerIs(spec, "off") || lowerIs(spec, "no") ||
        lowerIs(spec, "count") || lowerIs(spec, "join")) {
        out->world = WorldMode::None;
        return true;
    }
    if (lowerIs(spec, "full") || lowerIs(spec, "on") || lowerIs(spec, "yes") ||
        lowerIs(spec, "world")) {
        out->world = WorldMode::Full;
        return true;
    }
    if (err) *err = "--world must be none or full, got " + spec;
    return false;
}

// An explicit list is exact for the optional features: unlisted optional
// features are off. This makes `--features=luau` mean "only Luau", not "Luau
// plus the profile's renderer". `all` enables every optional feature; `none`
// disables them. The world dimension is separate and is preserved; use
// `world=none` / `world=full` (or `--world`) to change it.
inline bool applyList(Set* out, const std::string& spec, std::string* err) {
    if (!out) return false;
    if (spec.empty()) {
        if (err) *err = "--features needs a comma-separated list";
        return false;
    }
    const WorldMode world = out->world;
    Set s;
    s.world = world;
    s.luau = s.render = s.gui = s.audio = false;
    s.physics = PhysicsMode::Client;

    size_t pos = 0;
    while (true) {
        const size_t comma = spec.find(',', pos);
        const std::string token =
            trim(spec.substr(pos, comma == std::string::npos
                                      ? std::string::npos
                                      : comma - pos));
        if (token.empty()) {
            if (err) *err = "--features contains an empty name";
            return false;
        }
        if (lowerIs(token, "all")) {
            s.luau = s.render = s.gui = s.audio = true;
        } else if (lowerIs(token, "none")) {
            s.luau = s.render = s.gui = s.audio = false;
        } else if (lowerIs(token, "luau") || lowerIs(token, "scripts")) {
            s.luau = true;
        } else if (lowerIs(token, "render")) {
            s.render = true;
        } else if (lowerIs(token, "gui")) {
            s.gui = true;
        } else if (lowerIs(token, "audio")) {
            s.audio = true;
        } else if (lowerIs(token, "world") || lowerIs(token, "world=full")) {
            s.world = WorldMode::Full;
        } else if (lowerIs(token, "world=none") || lowerIs(token, "count")) {
            s.world = WorldMode::None;
        } else if (lowerIs(token, "physics") || lowerIs(token, "physics=server")) {
            s.physics = PhysicsMode::Server;
        } else if (lowerIs(token, "physics=off")) {
            s.physics = PhysicsMode::Off;
        } else if (lowerIs(token, "physics=client")) {
            s.physics = PhysicsMode::Client;
        } else if (lowerIs(token, "input") || lowerIs(token, "controller") ||
                   lowerIs(token, "camera")) {
            // Local character/physics and the 2016 CameraScript are always
            // part of a world session; these names are accepted for
            // compatibility with older configs and mean "on".
        } else if (lowerIs(token, "mic")) {
            if (err)
                *err = "feature 'mic' is not implemented in this build";
            return false;
        } else {
            if (err) *err = "unknown feature '" + token + "'";
            return false;
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    *out = s;
    return true;
}

// A join-only session has nothing to render, script or put in a PlayerGui.
// Reject the combination instead of building a client that silently ignores
// the feature it was told to enable.
inline bool validate(const Set& s, std::string* err) {
    if (s.world == WorldMode::Full) return true;
    const char* what = s.luau ? "run Luau"
                     : s.gui ? "install the GUI tree"
                     : s.render ? "render the world"
                     : s.audio ? "play sounds"
                                : nullptr;
    if (!what) return true;
    if (err)
        *err = std::string("--world=none cannot ") + what +
               "; drop the feature or use --mode=world/script/full";
    return false;
}

inline std::string featureList() {
    return
        "world (default: full):\n"
        "  world=none      join/handshake + liveness only; no DataModel\n"
        "  world=full      apply the replicated world\n"
        "core (always on with a world): transport, join, replication, Engine\n"
        "optional (--features=luau,render,gui,audio):\n"
        "  luau    execute replicated Luau chunks (alias: scripts)\n"
        "  render  SDL/OpenGL world viewer\n"
        "  gui     PlayerGui tree + NanoVG ScreenGui rendering\n"
        "  audio   SDL3 WAV/OGG/MP3 playback (local rbxasset + HTTP)\n"
        "  camera  accepted alias; 2016 camera is part of a world session\n"
        "  input   accepted alias; host input is part of a world session\n"
        "physics (default: client):\n"
        "  physics=client  Jolt CharacterVirtual prediction + 2016 camera\n"
        "  physics=server  server-authoritative; no local solver\n"
        "  physics=off     no local solver (does not transfer ownership)\n"
        "unavailable:\n"
        "  mic (voice capture/transport)\n"
        "  face control (needs a face-tracking source)\n"
        "profiles: full, minimal, remote, join\n"
        "modes (--mode): join, world, script, full\n"
        "An explicit list is exact: --features=luau disables render and gui.\n";
}

inline std::string summary(const Set& s, const std::string& profile) {
    std::string out = "profile=" + profile + " world=" +
                      worldName(s.world) + " luau=" + onOff(s.luau) +
                      " render=" + onOff(s.render) + " gui=" + onOff(s.gui) +
                      " audio=" + onOff(s.audio) + " physics=";
    out += s.physics == PhysicsMode::Client
               ? "client (Jolt local prediction)"
           : s.physics == PhysicsMode::Server
               ? "server (no local solver)"
               : "off (no local solver; ownership is server-controlled)";
    return out;
}

} // namespace features
} // namespace rbx
