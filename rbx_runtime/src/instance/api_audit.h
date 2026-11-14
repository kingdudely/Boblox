// api_audit.h — opt-in inventory of the Roblox API surface a script run used
// but this runtime does not implement.
//
// Set RBX_API_AUDIT=/path/to/audit.json before a script-mode run.  The engine
// writes one JSON object at teardown:
//
//   missing_globals  global reads that resolved to nil (task, TweenInfo, ...)
//   missing_members  Instance member reads that raised "not a valid member"
//                    (class.member: a missing method, event, property or child)
//   stub_calls       generated MethodInfo stubs that were actually called
//   fallback_calls   hand-written compatibility fallbacks (group/friend data)
//                    that did not have a real backend
//   event_connects   signals a script connected to or waited on
//   event_fires      signals that fired at least once
//
// Counts are per process.  This is deliberately behind an environment variable:
// the maps add an allocation to every miss, and ordinary runs should not pay
// for an inventory they never read.
#pragma once

namespace rbx {
namespace audit {

void note_global_missing(const char* name);
void note_member_missing(const char* cls, const char* member);
void note_stub_call(const char* cls, const char* member);
void note_fallback_call(const char* cls, const char* member);
void note_event_connect(const char* name);
void note_event_fire(const char* name);

// Write the JSON inventory to RBX_API_AUDIT, when set.  Safe to call more than
// once; the last call wins.
void dump();

} // namespace audit
} // namespace rbx
