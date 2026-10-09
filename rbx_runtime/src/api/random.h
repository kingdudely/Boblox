// rbxrandom.h — Roblox Random (PCG) reimplementation for the challenge runner.
// All symbols are prefixed rbx_ to keep this project cleanly separable from Luau.
#pragma once

struct lua_State;

// Registers the global `Random` table (with Random.new) into the given state.
void rbx_random_register(lua_State* L);
