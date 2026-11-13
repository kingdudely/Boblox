// avatar_watch.h — when is a world join's missing Character a failed join?
//
// The RCC's LoadCharacter decision is per-session, and a slow but healthy join
// can stream the world for longer than any fixed "no avatar yet" timer. A
// retry must therefore be opt-in, happen after a generous deadline, and never
// fire for a session that already linked a replicated Character once: a
// respawn or model swap briefly clears the link, and tearing that session down
// is worse than waiting for the next replicated body.
//
// Keeping this policy in a header (rather than in the run loop) makes the
// latch testable without a socket or a live server.
#pragma once

namespace rbx {
namespace client {

class AvatarWatch {
 public:
  // Feed every observation of the local Character link. `true` latches the
  // session as having had an avatar for the rest of its life.
  void observe(bool replicated) {
    replicated_now_ = replicated;
    if (replicated) ever_replicated_ = true;
  }

  bool ever_replicated() const { return ever_replicated_; }
  bool replicated_now() const { return replicated_now_; }

  // One diagnostic per session, only when the world is up, no avatar was ever
  // linked, and the deadline has passed.
  bool noteDue(bool has_world, double age_s, double deadline_s) {
    if (note_emitted_ || ever_replicated_ || !has_world) return false;
    if (age_s < deadline_s) return false;
    note_emitted_ = true;
    return true;
  }

  // A fresh session is allowed only for a join that has never linked an
  // avatar, after the deadline, with retries left and renewable credentials.
  bool retryDue(bool has_world, double age_s, double deadline_s, int retries,
                int limit, bool can_rejoin) const {
    if (ever_replicated_ || !has_world || !can_rejoin) return false;
    if (replicated_now_ || limit <= 0 || retries >= limit) return false;
    return age_s >= deadline_s;
  }

 private:
  bool ever_replicated_ = false;
  bool replicated_now_ = false;
  bool note_emitted_ = false;
};

} // namespace client
} // namespace rbx
