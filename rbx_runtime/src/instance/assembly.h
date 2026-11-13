// assembly.h — rigid-joint assembly graph and native root-part selection.
//
// The physics-facing query surface (GetConnectedParts, GetJoints,
// GetRootPart, AssemblyRootPart) all read this one model.  Keeping the graph
// here prevents the Lua bindings and the property getter from growing separate
// traversals with subtly different joint rules.
#pragma once

#include <vector>

namespace rbx {
class Instance;
}

namespace rbx::assembly {

// True for instance classes that participate in the rigid assembly graph.
bool is_joint_like(const Instance* inst);
// True for the joint classes the native solver merges rigidly.  The legacy
// surface joints Glue and ManualGlue are rigid (native GetRootPart and
// GetConnectedParts include them); Rotate and VelocityMotor are the non-rigid
// JointInstance classes and stay in GetJoints without joining an assembly.
bool is_rigid_joint(const Instance* inst);
bool joint_enabled(const Instance* inst);
Instance* joint_part(const Instance* joint, const char* prop);
// Native JointInstance.Active: true exactly when this rigid joint is one of
// the solver-selected edges of the assembly's active spanning forest.  The
// same selection pass produces AssemblyRootPart.  Non-rigid joints always
// report false, matching the native Rotate override and VelocityMotor's
// inactive internal joint.
bool joint_active(Instance* joint);

// Nearest Workspace/WorldModel ancestor, or null for an unparented/out-of-
// world part.  Native GetRootPart returns nil outside that scope.
Instance* workspace_scope(Instance* part);

// The native GetConnectedParts result: the receiver plus every part reachable
// through enabled rigid joints in the same active scope.  An out-of-world part
// yields only itself, matching the native warning-and-self result.
void connected_parts(Instance* part, bool recursive,
                     std::vector<Instance*>* out);

// Every joint/constraint attached to the part, including disabled joints and
// joints in an inactive model.  This is the native GetJoints surface.
void joints(Instance* part, std::vector<Instance*>* out);

// The assembly root chosen by the native rules:
//   1. an anchored part is itself;
//   2. one anchored member owns the assembly;
//   3. non-massless parts precede massless ones;
//   4. higher RootPriority wins;
//   5. larger largest-face area wins, with the native 20x Seat/VehicleSeat,
//      10x HumanoidRootPart and 5x Torso multipliers;
//   6. equal-rank anchors follow the newest active anchor link; a complete
//      unanchored tie uses one instance-creation order across replicated and
//      locally created parts (the wire id is not the sort key).
// Returns null when the part is not in a Workspace/WorldModel.
Instance* root_part(Instance* part);

} // namespace rbx::assembly
