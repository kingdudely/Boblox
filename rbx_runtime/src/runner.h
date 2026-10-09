// runner.h — in-process execution of decoded 0x9B challenge programs.
//
// This is the public API of challenge_core: program load/call + the Options
// that describe the environment. The environment itself (Roblox API shims)
// lives in rbx_runtime/src/api/ — Roblox API shims live here (never in the
// client, never in the Luau checkout). See api/api.h for how to add one.
//
// Byte-exactness notes (see FINDINGS.md):
//   * LuauCallFeedback FFlag must be on (production bytecode uses CALLFB).
//   * Sandbox defaults mirror the production client:
//       UserSettings -> error, tostring(UserSettings()) = "UserSettings",
//       os.exit -> missing, IsStudio -> false, newproxy -> ok.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rbxch {

// Which sandbox surface the environment exposes (Options::profile):
//   Challenge — the frozen production challenge surface: exactly the
//               historical module set (api/registry.cpp, all tagged
//               kBoth), byte-exact against the native client. The 0x9B
//               solve path ALWAYS uses this; new APIs must not appear
//               here (the unit_profile test pins the list).
//   Engine    — the full scripting environment for real gameplay:
//               Challenge surface plus Engine-only modules (DataModel
//               tree, Instance, workspace, …) as they get implemented.
//               Never used by the solve path.
enum class Profile : uint8_t { Challenge = 0, Engine = 1 };

struct Options {
    uint32_t u1 = 0;
    uint32_t u2 = 0;
    std::string job = "00000000-0000-0000-0000-000000000000";
    std::string usersettings = "error";
    std::string us_string = "UserSettings";
    std::string os_exit = "missing";
    std::string studio = "false";
    std::string newproxy = "ok";
    Profile profile = Profile::Challenge; // see enum Profile above
};

enum class Status {
    Ok,
    LoadFailed,
    RuntimeError,
};

struct Result {
    Status status = Status::Ok;
    uint32_t answer = 0;   // valid when status == Ok
    double raw = 0.0;      // the raw returned double
    std::string error;     // load/runtime error text
};

// Execute a STANDARD-Luau bytecode challenge program with the given
// (u1, u2, job) arguments under the production-mirroring sandbox.
Result run(const void* code, size_t size, const Options& opts);

} // namespace rbxch
