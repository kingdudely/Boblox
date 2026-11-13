// surface_joints.h — native BasePart surface auto-joint creation.
//
// The native client's BasePart:MakeJoints evaluates each contact pair.  A
// lower part's TopSurface=Hinge creates a Rotate owned by that part; if the
// lower top surface is non-special (Smooth/Weld/Glue), an upper part's
// BottomSurface=Hinge still creates the Rotate, owned by the Hinge part.  A
// lower Top Hinge wins over an upper Bottom Motor; a lower Top Motor or
// SteppingMotor wins over an upper Bottom Hinge and creates RotateV/RotateP.
// The other part's surface is therefore part of the edge, not just the
// caller's own Top/BottomSurface.  Side faces and their Hinge surfaces
// create no joint at all (native face probe).
//
// BreakJoints removes the rigid joints reachable from the part and leaves
// Rotate/RotateV/RotateP untouched; that asymmetry is native (auto Weld/Glue
// disappear, auto Rotate survives).
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

// Native Model:BreakJoints destroys every legacy JointInstance and
// WeldConstraint in the subtree, including Rotate (unlike BasePart).
void break_joints_tree(Instance* root);

} // namespace rbx::surface_joints
