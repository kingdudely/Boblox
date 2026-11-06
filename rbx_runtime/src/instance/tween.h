// tween.h — TweenService:Create + Tween playback (see tween.cpp).
//
// TweenService:Create(instance, TweenInfo, goals) returns a TWEEN INSTANCE
// (class Tween, whose base TweenBase carries PlaybackState/Play/Pause/Cancel
// and the Completed event). The animation state is C++-side and advances on
// engine::step's frame delta, so headless runs animate identically to the
// rendered client.
#pragma once

struct lua_State;

namespace rbx {

// TweenService:Create / Tween:Play|Pause|Cancel (attached by methods.cpp).
int m_tween_service_create(lua_State* L);
int m_tween_play(lua_State* L);
int m_tween_pause(lua_State* L);
int m_tween_cancel(lua_State* L);

// Advance every playing tween by dt (called from engine::step).
void tween_step(lua_State* L, double dt);

// Drop the per-state tween table (teardown_alive, after pinning: the run
// list holds Refs to the target and the Tween instance).
void delete_tweens(lua_State* L);

} // namespace rbx
