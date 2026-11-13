// surface_joints.h — native surface auto-joint creation.
//
// The native client's BasePart:MakeJoints evaluates every contacting face of
// the part against every other part in the Workspace.  All six faces take
// part, not only Top/Bottom.  Dynamic faces (Hinge/Motor/SteppingMotor) win
// over rigid faces; when both sides have a dynamic face the caller's face
// wins.  Otherwise Weld beats Glue beats a Snap pairing, and rigid joints are
// caller-owned.  Face frames, C0/C1, names and Active flags follow the live
// native probes in tests/live/native_surface_*_probe.lua.
//
// BreakJoints removes the rigid joints reachable from the part and leaves
// Rotate/RotateV/RotateP untouched; that asymmetry is native (auto Weld/Glue
// disappear, auto Rotate survives).  Model:MakeJoints runs the BasePart pass
// for every part in its subtree; Model:BreakJoints removes every legacy joint
// and WeldConstraint in the subtree.
#pragma once

namespace rbx {
class Instance;
}

namespace rbx::surface_joints {

// Run the native BasePart:MakeJoints contact-pair pass for `part`.  Idempotent:
// an existing joint of the winning class between the same pair is reused,
// matching native after either part called MakeJoints.
void make_joints(Instance* part);

// Native BasePart:BreakJoints.  Destroys rigid joints reachable from `part`
// through the assembly, leaves non-rigid joints (Rotate/VelocityMotor) in
// place.
void break_joints(Instance* part);

// Native Model:MakeJoints runs the BasePart surface pass for every BasePart
// in the subtree, in live child order; nested models are visited first.
void make_joints_tree(Instance* root);

// Native Model:BreakJoints destroys every legacy JointInstance and
// WeldConstraint in the subtree, including Rotate (unlike BasePart).
void break_joints_tree(Instance* root);

} // namespace rbx::surface_joints
