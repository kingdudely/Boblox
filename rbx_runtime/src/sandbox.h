// sandbox.h — installs the production-mirroring Roblox environment into a
// fresh lua_State (see sandbox.cpp). Internal to challenge_core; runner.cpp
// is the only consumer.
//
// "Sandbox" here means: standard Luau libs + every module from the API
// registry (api/registry.cpp). The rules it must preserve are recorded in
// runner.h and FINDINGS.md (byte-exact vs the production client).
#pragma once

struct lua_State;

namespace rbxch {

struct Options;

void setup_sandbox(lua_State* L, const Options& opts);

} // namespace rbxch
