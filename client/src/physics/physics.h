// physics.h — Jolt-backed client physics for the Engine profile.
//
// Roblox distributes rigid-body simulation by network ownership.  The client
// always simulates its own character, and it also simulates unanchored bodies
// the server has granted to it.  Everything else is server-owned geometry
// whose pose arrives through replication.  Workspace.AuthorityMode selects the
// policy: Server keeps every replicated assembly kinematic, Automatic lets the
// per-part NetworkOwnerV3 grants drive local simulation (Crossroads vs Natural
// Disaster Survival).
//
// The client has three jobs here:
//
//   * collide the local character against the replicated world so input
//     produces a pose (and a 0x83/0x05 upload) before the server answers,
//   * simulate locally-owned unanchored parts and report their poses back,
//   * answer Workspace ray/overlap queries for the camera and scripts.
//
// The solver is Jolt Physics (MIT).  We own only the policy: which instances
// become bodies, how a Roblox part's Size/CFrame/Shape maps to a Jolt shape,
// and how the character's Humanoid state is derived from Jolt's ground state.
// Keeping that policy here (instead of inside LiveRuntime) makes it testable
// without a socket or a window.
//
// Engine-profile only.  Nothing here is registered on the Luau challenge
// surface; the challenge runner never links a World.
#pragma once

#include "instance/contact_host.h"
#include "instance/variant.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rbx {

class Instance;

namespace physics {

// Result of a ray query. `instance` is the BasePart that was hit.
struct RayHit {
    bool hit = false;
    double distance = 0.0;
    Vector3 position;
    Vector3 normal;
    Instance* instance = nullptr;
};

// Mutable collision-world configuration.  One World owns one Jolt PhysicsSystem
// and is intentionally single-threaded (a client only needs one simulation).
class CharacterController;

// The native solver runs its 60 Hz frame as four 240 Hz physics steps.  Input
// and rendering stay frame-paced; the rigid-body world does not.
constexpr double kPhysicsStep = 1.0 / 240.0;

enum class BodyMode { None, Static, Kinematic, Dynamic };

struct BodyState {
    BodyMode mode = BodyMode::None;
    bool sleeping = false;
    CFrame cframe;
    Vector3 linear_velocity;
    Vector3 angular_velocity;
};

// A locally simulated body whose pose changed since the previous drain.  The
// network layer turns these into owner CFrame uploads; the physics module does
// not know replication ids or property ids.
struct PartUpdate {
    Instance* part = nullptr;
    CFrame cframe;
    Vector3 linear_velocity;
    Vector3 angular_velocity;
};

struct WorldStats {
    std::size_t static_bodies = 0;
    std::size_t kinematic_bodies = 0;
    std::size_t dynamic_bodies = 0;
    std::size_t steps = 0;
};

class World {
  public:
    World();
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    void set_gravity(double studs_per_second_squared);
    double gravity() const { return gravity_; }

    void clear();

    // Incrementally reconcile the Jolt world with the replicated tree.  New
    // parts become bodies, removed parts lose theirs, and changed Size/Shape/
    // Anchored/CanCollide/CFrame properties are applied without resetting the
    // rest of the world.  `ignore_root` and its descendants (the local avatar)
    // never become bodies.  Safe to call every frame; the per-part state cache
    // keeps unchanged parts cheap.
    void sync(Instance* workspace, const Instance* ignore_root = nullptr);

    // Network ownership policy.  `client_authority` is
    // Workspace.AuthorityMode == Automatic; `local_owner_id` is the numeric
    // NetworkOwnerV3 value granted to this client.  Server-authority worlds
    // keep every unanchored body kinematic regardless of grants.
    void set_authority(bool client_authority, std::uint64_t local_owner_id);

    // Offline tests and embeds can opt into simulating every unanchored part.
    // The live path uses set_authority instead.
    void set_simulate_all_unanchored(bool on);

    // Legacy full rebuild used by offline tests and one-shot tools.  The live
    // client calls sync() so bodies and their velocities survive replication.
    void rebuild(Instance* workspace, bool simulate_unanchored = false,
                 const Instance* ignore_root = nullptr);

    // Integrate simulated bodies.  The character is stepped separately by
    // CharacterController because Jolt's CharacterVirtual is not a rigid body.
    void step(double dt);

    // Ray against every collider. `ignore` parts are skipped (the local
    // character's own parts).
    bool raycast(const Vector3& origin, const Vector3& direction,
                 double max_distance, RayHit* out,
                 const std::vector<const Instance*>* ignore = nullptr) const;

    // True when an axis-aligned box centred at `center` overlaps any
    // collider.  `depth`/`normal` receive the smallest push that separates the
    // box (world-space normal pointing from the collider to the box).
    bool overlaps(const Vector3& center, const Vector3& half,
                  const std::vector<const Instance*>* ignore,
                  double* depth = nullptr, Vector3* normal = nullptr) const;

    std::size_t size() const;
    bool empty() const { return size() == 0; }
    WorldStats stats() const;

    // Current solver state for one part, or false when it has no body.
    bool body_state(const Instance* part, BodyState* out) const;

    // Latest contact state for a rigid body.  `touching_parts` deduplicates
    // contacts and never includes `part`; `grounded` is true for an upward
    // contact normal (the character root is handled by CharacterController).
    void touching_parts(const Instance* part,
                        std::vector<const Instance*>* out) const;
    bool grounded(const Instance* part) const;

    // Contact transitions produced by the last completed physics steps.
    // Strong refs keep a part alive until the runtime dispatches the event.
    std::vector<contact::Pair> take_contact_begun();
    std::vector<contact::Pair> take_contact_ended();

    // Move locally simulated poses since the last call.  The caller owns
    // mapping these instances to replication ids and encoding the upload.
    std::vector<PartUpdate> take_updates();

    // Jolt handles are opaque outside this translation unit.
    struct Impl;

  private:
    friend class CharacterController;
    // Narrow-phase shape query shared by GetTouchingParts and IsGrounded.
    bool contact_query(const Instance* part, std::vector<const Instance*>* out,
                       bool* grounded) const;
    std::unique_ptr<Impl> impl_;
    double gravity_ = 196.2;
    bool client_authority_ = false;
    bool simulate_all_unanchored_ = false;
    std::uint64_t local_owner_id_ = 0;
};

// Character controller state.  `position` is the HumanoidRootPart centre;
// `velocity` is studs/second.
struct CharacterState {
    bool attached = false;
    bool grounded = false;
    bool jumped = false;
    Vector3 position;
    Vector3 velocity;
    Vector3 move_direction;
    CFrame cframe;
    double walk_speed = 16.0;
    double jump_power = 50.0;
    double gravity = 196.2;
    double height = 5.0;
    double radius = 1.0;
    int state = 8; // Enum.HumanoidStateType.Running
};

// A Humanoid/Model driver backed by Jolt's CharacterVirtual.  It writes the
// local CFrame override on the root and places the model's other BaseParts at
// their attach-time offsets, so the avatar stays coherent without joints.
class CharacterController {
  public:
    CharacterController();
    ~CharacterController();
    CharacterController(const CharacterController&) = delete;
    CharacterController& operator=(const CharacterController&) = delete;

    // `character` is the local Player's Character Model.  The controller finds
    // the HumanoidRootPart (or the first BasePart) and every descendant part.
    bool attach(World* world, Instance* character);
    void detach();
    bool attached() const { return state_.attached; }

    // World-space direction, magnitude 0..1. Humanoid:Move uses this too.
    void set_move_input(const Vector3& direction);
    // Queue a jump; the next step consumes it when grounded.
    void request_jump(bool down);
    void set_walk_speed(double speed);
    void set_jump_power(double power);
    // Face the movement direction on the ground (native default).  Set false
    // for shift-lock / first-person where the avatar keeps the camera yaw.
    void set_face_movement(bool on) { face_movement_ = on; }
    void set_facing_yaw(double radians) { facing_yaw_ = radians; }

    // Advance the virtual character and write part CFrames.  Safe with a
    // null/destroyed character: it becomes a no-op and detaches.
    void step(double dt);

    // World parts whose collision geometry is currently touching the
    // character capsule.  The contact is a body contact, not a part-body
    // overlap; duplicate bodies are collapsed.
    void touching_parts(std::vector<Instance*>* out) const;

    const CharacterState& state() const { return state_; }
    Instance* character() const { return character_; }
    Instance* root() const { return root_; }
    Instance* humanoid() const { return humanoid_; }
    World* world() const { return world_; }

    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;
    World* world_ = nullptr;
    Instance* character_ = nullptr;
    Instance* root_ = nullptr;
    Instance* humanoid_ = nullptr;
    CharacterState state_;
    Vector3 move_input_;
    bool want_jump_ = false;
    bool face_movement_ = true;
    double facing_yaw_ = 0.0;
    int landing_frames_ = 0;
};

// Small vector helpers shared with the camera controller and tests.
inline Vector3 vadd(const Vector3& a, const Vector3& b) {
    return Vector3{a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vector3 vsub(const Vector3& a, const Vector3& b) {
    return Vector3{a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vector3 vmul(const Vector3& a, double s) {
    return Vector3{a.x * s, a.y * s, a.z * s};
}
inline double vdot(const Vector3& a, const Vector3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vector3 vcross(const Vector3& a, const Vector3& b) {
    return Vector3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                   a.x * b.y - a.y * b.x};
}
inline double vlength(const Vector3& a) { return std::sqrt(vdot(a, a)); }
inline Vector3 vnormalize(const Vector3& a) {
    const double l = vlength(a);
    return l > 1e-9 ? vmul(a, 1.0 / l) : Vector3{};
}

// CFrame helpers used by the controller and camera (header-inline so tests
// can pin the math without linking another translation unit).
inline CFrame cframe_mul(const CFrame& a, const CFrame& b) {
    CFrame o;
    o.r00 = a.r00 * b.r00 + a.r01 * b.r10 + a.r02 * b.r20;
    o.r01 = a.r00 * b.r01 + a.r01 * b.r11 + a.r02 * b.r21;
    o.r02 = a.r00 * b.r02 + a.r01 * b.r12 + a.r02 * b.r22;
    o.r10 = a.r10 * b.r00 + a.r11 * b.r10 + a.r12 * b.r20;
    o.r11 = a.r10 * b.r01 + a.r11 * b.r11 + a.r12 * b.r21;
    o.r12 = a.r10 * b.r02 + a.r11 * b.r12 + a.r12 * b.r22;
    o.r20 = a.r20 * b.r00 + a.r21 * b.r10 + a.r22 * b.r20;
    o.r21 = a.r20 * b.r01 + a.r21 * b.r11 + a.r22 * b.r21;
    o.r22 = a.r20 * b.r02 + a.r21 * b.r12 + a.r22 * b.r22;
    o.x = a.r00 * b.x + a.r01 * b.y + a.r02 * b.z + a.x;
    o.y = a.r10 * b.x + a.r11 * b.y + a.r12 * b.z + a.y;
    o.z = a.r20 * b.x + a.r21 * b.y + a.r22 * b.z + a.z;
    return o;
}
inline CFrame cframe_inverse(const CFrame& c) {
    CFrame o;
    o.r00 = c.r00;
    o.r01 = c.r10;
    o.r02 = c.r20;
    o.r10 = c.r01;
    o.r11 = c.r11;
    o.r12 = c.r21;
    o.r20 = c.r02;
    o.r21 = c.r12;
    o.r22 = c.r22;
    o.x = -(o.r00 * c.x + o.r01 * c.y + o.r02 * c.z);
    o.y = -(o.r10 * c.x + o.r11 * c.y + o.r12 * c.z);
    o.z = -(o.r20 * c.x + o.r21 * c.y + o.r22 * c.z);
    return o;
}
inline CFrame cframe_yaw(const Vector3& position, double yaw) {
    const double c = std::cos(yaw);
    const double s = std::sin(yaw);
    CFrame f;
    f.x = position.x;
    f.y = position.y;
    f.z = position.z;
    f.r00 = c;
    f.r01 = 0.0;
    f.r02 = s;
    f.r10 = 0.0;
    f.r11 = 1.0;
    f.r12 = 0.0;
    f.r20 = -s;
    f.r21 = 0.0;
    f.r22 = c;
    return f;
}

} // namespace physics
} // namespace rbx
