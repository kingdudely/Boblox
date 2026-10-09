// runner.h — in-process execution of decoded 0x9B challenge programs.
//
// This is the library form of challenge_runner: the sandbox (Roblox API
// shims) plus program load/call. Kept in luau_runner/ because Roblox
// API shims live here (never in the client, never in the Luau checkout).
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

struct Options {
    uint32_t u1 = 0;
    uint32_t u2 = 0;
    std::string job = "00000000-0000-0000-0000-000000000000";
    std::string usersettings = "error";
    std::string us_string = "UserSettings";
    std::string os_exit = "missing";
    std::string studio = "false";
    std::string newproxy = "ok";
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
