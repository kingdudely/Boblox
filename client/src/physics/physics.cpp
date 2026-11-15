// physics.cpp — Jolt adapter: part shapes, static world, virtual character.
#include "physics/physics.h"

#include "instance/instance.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rbx {
namespace physics {

namespace {

// One Jolt world only needs two layers: server-owned geometry and locally
// simulated bodies. Static-static pairs are never tested.
namespace Layers {
constexpr JPH::ObjectLayer kStatic = 0;
constexpr JPH::ObjectLayer kMoving = 1;
constexpr JPH::ObjectLayer kCount = 2;
} // namespace Layers

namespace BpLayers {
const JPH::BroadPhaseLayer kStatic(0);
const JPH::BroadPhaseLayer kMoving(1);
constexpr JPH::uint kCount = 2;
} // namespace BpLayers

class BpLayerInterface final : public JPH::BroadPhaseLayerInterface {
  public:
    JPH::uint GetNumBroadPhaseLayers() const override { return BpLayers::kCount; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(
        JPH::ObjectLayer layer) const override {
        return layer == Layers::kStatic ? BpLayers::kStatic : BpLayers::kMoving;
    }
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == BpLayers::kStatic ? "STATIC" : "MOVING";
    }
};

class ObjectVsBpLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
  public:
    bool ShouldCollide(JPH::ObjectLayer layer,
                       JPH::BroadPhaseLayer bp) const override {
        // Static geometry is only queried by moving bodies/queries. A moving
        // body sees both layers.
        if (layer == Layers::kStatic)
            return bp == BpLayers::kMoving;
        return true;
    }
};

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
  public:
    bool ShouldCollide(JPH::ObjectLayer a,
                       JPH::ObjectLayer b) const override {
        return a == Layers::kMoving || b == Layers::kMoving;
    }
};

// Bodies that need pair-level collision exceptions share one filter:
//
//   * the two parts joined by a Motor never collide with each other, so a
//     hinge pivot can overlap without the narrow phase fighting it;
//   * a part with CanCollide == false never produces contact response with
//     any body, but still exists in the solver so joints can drive it.
//
// A body can be both a motor endpoint and non-collidable; keeping both rules
// in one group/filter avoids a second collision policy that could re-enable
// contact response.
class BodyCollisionFilter final : public JPH::GroupFilter {
  public:
    bool CanCollide(const JPH::CollisionGroup& a,
                    const JPH::CollisionGroup& b) const override {
        const uint32_t ai = a.GetSubGroupID();
        const uint32_t bi = b.GetSubGroupID();
        if (no_collide_.count(ai) || no_collide_.count(bi))
            return false;
        const Key pair_key = ai < bi ? Key{ai, bi} : Key{bi, ai};
        auto it = blocked_.find(pair_key);
        return it == blocked_.end() || it->second == 0;
    }

    void block(JPH::BodyID a, JPH::BodyID b) {
        ++blocked_[key(a, b)];
    }

    void unblock(JPH::BodyID a, JPH::BodyID b) {
        auto it = blocked_.find(key(a, b));
        if (it != blocked_.end() && --it->second <= 0)
            blocked_.erase(it);
    }

    void set_no_collide(JPH::BodyID id, bool no_collide) {
        const uint32_t index = id.GetIndexAndSequenceNumber();
        if (no_collide)
            no_collide_.insert(index);
        else
            no_collide_.erase(index);
    }

    bool is_no_collide(JPH::BodyID id) const {
        return no_collide_.count(id.GetIndexAndSequenceNumber()) != 0;
    }

    // Drop every exception owned by a body that is leaving the world.
    void forget(JPH::BodyID id) {
        const uint32_t index = id.GetIndexAndSequenceNumber();
        no_collide_.erase(index);
        for (auto it = blocked_.begin(); it != blocked_.end();) {
            if (it->first.first == index || it->first.second == index)
                it = blocked_.erase(it);
            else
                ++it;
        }
    }

    void clear() {
        blocked_.clear();
        no_collide_.clear();
    }

    static constexpr JPH::CollisionGroup::GroupID kGroup = 1;

  private:
    using Key = std::pair<uint32_t, uint32_t>;
    static Key key(JPH::BodyID a, JPH::BodyID b) {
        const uint32_t ai = a.GetIndexAndSequenceNumber();
        const uint32_t bi = b.GetIndexAndSequenceNumber();
        return ai < bi ? Key{ai, bi} : Key{bi, ai};
    }
    std::map<Key, int> blocked_;
    std::set<uint32_t> no_collide_;
};

void ensure_jolt_init() {
    static bool done = false;
    if (done)
        return;
    done = true;
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
}

JPH::Quat quat_from_cframe(const CFrame& f) {
    JPH::Mat44 m;
    m.SetColumn3(0, JPH::Vec3(float(f.r00), float(f.r10), float(f.r20)));
    m.SetColumn3(1, JPH::Vec3(float(f.r01), float(f.r11), float(f.r21)));
    m.SetColumn3(2, JPH::Vec3(float(f.r02), float(f.r12), float(f.r22)));
    m.SetColumn3(3, JPH::Vec3(0.0f, 0.0f, 0.0f));
    return m.GetQuaternion().Normalized();
}

CFrame cframe_from_quat(const JPH::RVec3& p, const JPH::Quat& q) {
    const JPH::Mat44 m = JPH::Mat44::sRotation(q);
    const JPH::Vec3 x = m.GetColumn3(0);
    const JPH::Vec3 y = m.GetColumn3(1);
    const JPH::Vec3 z = m.GetColumn3(2);
    CFrame f;
    f.x = p.GetX();
    f.y = p.GetY();
    f.z = p.GetZ();
    f.r00 = x.GetX();
    f.r01 = y.GetX();
    f.r02 = z.GetX();
    f.r10 = x.GetY();
    f.r11 = y.GetY();
    f.r12 = z.GetY();
    f.r20 = x.GetZ();
    f.r21 = y.GetZ();
    f.r22 = z.GetZ();
    return f;
}

bool get_size(const Instance* part, Vector3* out) {
    Variant v;
    if (!part->get_prop("Size", v))
        return false;
    if (const auto* s = std::get_if<Vector3>(&v)) {
        *out = *s;
        return true;
    }
    return false;
}

bool get_frame(const Instance* part, CFrame* out) {
    Variant v;
    if (!part->get_prop("CFrame", v))
        return false;
    if (const auto* f = std::get_if<CFrame>(&v)) {
        *out = *f;
        return true;
    }
    return false;
}

bool get_cframe_prop(const Instance* inst, const char* name, CFrame* out) {
    Variant value;
    if (!inst || !inst->get_prop(name, value))
        return false;
    const auto* frame = std::get_if<CFrame>(&value);
    if (!frame)
        return false;
    *out = *frame;
    return true;
}

bool get_bool(const Instance* part, const char* name, bool fallback) {
    Variant v;
    if (!part->get_prop(name, v))
        return fallback;
    if (const auto* b = std::get_if<bool>(&v))
        return *b;
    return fallback;
}

double get_double(const Instance* part, const char* name, double fallback) {
    Variant v;
    if (!part->get_prop(name, v))
        return fallback;
    if (const auto* d = std::get_if<double>(&v))
        return *d;
    if (const auto* i = std::get_if<int64_t>(&v))
        return double(*i);
    return fallback;
}

std::uint64_t network_owner(const Instance* part) {
    Variant v;
    if (!part->get_prop("NetworkOwnerV3", v))
        return 0;
    if (const auto* i = std::get_if<int64_t>(&v))
        return i && *i > 0 ? std::uint64_t(*i) : 0;
    if (const auto* d = std::get_if<double>(&v))
        return d && *d > 0.0 ? std::uint64_t(*d) : 0;
    return 0;
}

bool part_in_character(const Instance* part) {
    const Instance* node = part;
    for (int depth = 0; node && depth < 12; ++depth, node = node->parent()) {
        if (node->is_a("Model") &&
            node->find_first_child_of_class("Humanoid"))
            return true;
        if (node->is_a("DataModel") || node->is_a("Workspace"))
            break;
    }
    return false;
}

double shape_volume(const Vector3& size, int kind) {
    const double x = std::max(0.01, size.x);
    const double y = std::max(0.01, size.y);
    const double z = std::max(0.01, size.z);
    switch (kind) {
    case 0: { // Ball: Jolt radius is the smallest half extent.
        const double r = std::min({x, y, z}) * 0.5;
        return 4.0 / 3.0 * M_PI * r * r * r;
    }
    case 2: { // Cylinder: smallest half extent is the radius.
        const double r = std::min(x, z) * 0.5;
        return M_PI * r * r * y;
    }
    case 3: // Wedge is half a box.
        return x * y * z * 0.5;
    case 4: // CornerWedge is the native five-vertex square pyramid.
        return x * y * z / 3.0;
    default:
        return x * y * z;
    }
}

bool frames_differ(const CFrame& a, const CFrame& b) {
    return std::abs(a.x - b.x) > 1e-5 || std::abs(a.y - b.y) > 1e-5 ||
           std::abs(a.z - b.z) > 1e-5 || std::abs(a.r00 - b.r00) > 1e-5 ||
           std::abs(a.r01 - b.r01) > 1e-5 || std::abs(a.r02 - b.r02) > 1e-5 ||
           std::abs(a.r10 - b.r10) > 1e-5 || std::abs(a.r11 - b.r11) > 1e-5 ||
           std::abs(a.r12 - b.r12) > 1e-5 || std::abs(a.r20 - b.r20) > 1e-5 ||
           std::abs(a.r21 - b.r21) > 1e-5 || std::abs(a.r22 - b.r22) > 1e-5;
}

// CFrame composition and basis extraction.  CFrame stores its rows in
// r00..r22; a local axis is one of the three rotation columns.
Vector3 frame_axis(const CFrame& frame, int axis) {
    switch (axis) {
    case 0:
        return Vector3{frame.r00, frame.r10, frame.r20};
    case 1:
        return Vector3{frame.r01, frame.r11, frame.r21};
    default:
        return Vector3{frame.r02, frame.r12, frame.r22};
    }
}

CFrame frame_mul(const CFrame& a, const CFrame& b) {
    CFrame out;
    out.x = a.x + a.r00 * b.x + a.r01 * b.y + a.r02 * b.z;
    out.y = a.y + a.r10 * b.x + a.r11 * b.y + a.r12 * b.z;
    out.z = a.z + a.r20 * b.x + a.r21 * b.y + a.r22 * b.z;
    out.r00 = a.r00 * b.r00 + a.r01 * b.r10 + a.r02 * b.r20;
    out.r01 = a.r00 * b.r01 + a.r01 * b.r11 + a.r02 * b.r21;
    out.r02 = a.r00 * b.r02 + a.r01 * b.r12 + a.r02 * b.r22;
    out.r10 = a.r10 * b.r00 + a.r11 * b.r10 + a.r12 * b.r20;
    out.r11 = a.r10 * b.r01 + a.r11 * b.r11 + a.r12 * b.r21;
    out.r12 = a.r10 * b.r02 + a.r11 * b.r12 + a.r12 * b.r22;
    out.r20 = a.r20 * b.r00 + a.r21 * b.r10 + a.r22 * b.r20;
    out.r21 = a.r20 * b.r01 + a.r21 * b.r11 + a.r22 * b.r21;
    out.r22 = a.r20 * b.r02 + a.r21 * b.r12 + a.r22 * b.r22;
    return out;
}

Instance* get_instance_prop(const Instance* inst, const char* name) {
    Variant value;
    if (!inst || !inst->get_prop(name, value))
        return nullptr;
    const auto* child = std::get_if<Instance*>(&value);
    return child ? *child : nullptr;
}

int part_shape(const Instance* part) {
    Variant v;
    if (!part->get_prop("Shape", v))
        return 1;
    if (const auto* e = std::get_if<EnumItem>(&v))
        return e->value;
    return 1; // Enum.PartType.Block
}

JPH::Ref<JPH::Shape> make_shape(const Vector3& size, int kind) {
    const float hx = float(std::max(0.01, size.x) * 0.5);
    const float hy = float(std::max(0.01, size.y) * 0.5);
    const float hz = float(std::max(0.01, size.z) * 0.5);
    switch (kind) {
    case 0: // Ball
        return new JPH::SphereShape(std::min({hx, hy, hz}));
    case 2: // Cylinder
        return new JPH::CylinderShape(hy, std::min(hx, hz));
    case 3: { // Wedge: a triangular prism, slope from -Z up to +Z.
        const JPH::Vec3 points[] = {
            {-hx, -hy, -hz}, {hx, -hy, -hz}, {-hx, -hy, hz},
            {hx, -hy, hz},   {-hx, hy, hz},  {hx, hy, hz},
        };
        JPH::ConvexHullShapeSettings settings(points, 6);
        const JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (result.IsValid())
            return result.Get();
        break;
    }
    case 4: { // Native Mesh::makeCornerWedge: bottom rectangle + apex at
              // (+X, +Y, -Z). This is a square pyramid, not the Wedge hull.
        const JPH::Vec3 points[] = {
            {-hx, -hy, -hz}, {-hx, -hy, hz}, {hx, hy, -hz},
            {hx, -hy, -hz},  {hx, -hy, hz},
        };
        JPH::ConvexHullShapeSettings settings(points, 5);
        const JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (result.IsValid())
            return result.Get();
        break;
    }
    default: // Block
        break;
    }
    return new JPH::BoxShape(JPH::Vec3(hx, hy, hz));
}

class IgnoreBodiesFilter final : public JPH::BodyFilter {
  public:
    IgnoreBodiesFilter(
        const std::unordered_map<uint32_t, Instance*>& by_index,
        const std::vector<const Instance*>* ignore)
        : by_index_(by_index), ignore_(ignore) {}

    bool ShouldCollide(const JPH::BodyID& id) const override {
        if (!ignore_ || ignore_->empty())
            return true;
        auto it = by_index_.find(id.GetIndexAndSequenceNumber());
        if (it == by_index_.end())
            return true;
        for (const Instance* p : *ignore_)
            if (it->second == p)
                return false;
        return true;
    }

  private:
    const std::unordered_map<uint32_t, Instance*>& by_index_;
    const std::vector<const Instance*>* ignore_;
};

// CharacterVirtual queries the narrow phase with a BodyFilter, not with the
// body's collision group.  Skip parts whose CanCollide is false so the local
// avatar does not stand on geometry that native treats as a non-collider.
class NoCollideBodyFilter final : public JPH::BodyFilter {
  public:
    explicit NoCollideBodyFilter(const BodyCollisionFilter* filter)
        : filter_(filter) {}

    bool ShouldCollide(const JPH::BodyID& id) const override {
        return !filter_ || !filter_->is_no_collide(id);
    }

  private:
    const BodyCollisionFilter* filter_;
};

// Narrow-phase contact queries are separate from the solver's pair filter, so
// they need the same no-contact rule explicitly.  Ray/overlap queries keep
// using IgnoreBodiesFilter and still see non-collidable parts, matching
// CanQuery's independence from CanCollide.
class ContactQueryBodyFilter final : public JPH::BodyFilter {
  public:
    ContactQueryBodyFilter(
        const std::unordered_map<uint32_t, Instance*>& by_index,
        const std::vector<const Instance*>* ignore,
        const BodyCollisionFilter* collision)
        : ignore_(by_index, ignore), collision_(collision) {}

    bool ShouldCollide(const JPH::BodyID& id) const override {
        if (collision_ && collision_->is_no_collide(id))
            return false;
        return ignore_.ShouldCollide(id);
    }

  private:
    IgnoreBodiesFilter ignore_;
    const BodyCollisionFilter* collision_;
};

// Collect the local avatar's parts so world-building can skip them. The
// character's own capsule is not a Jolt body, but the replicated model is in
// Workspace and would otherwise become static geometry around it.
void collect_parts(const Instance* root, std::vector<Instance*>* out) {
    if (!root)
        return;
    for (const auto& child : root->children()) {
        if (child->is_a("BasePart"))
            out->push_back(child.get());
        collect_parts(child.get(), out);
    }
}

} // namespace

// Jolt only reports rigid-body contacts through this listener.  The runtime
// character capsule uses CharacterVirtual's active-contact list separately.
struct ContactRouter final : JPH::ContactListener,
                             JPH::BodyActivationListener {
    World::Impl* impl = nullptr;
    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2,
                        const JPH::ContactManifold& manifold,
                        JPH::ContactSettings&) override;
    void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2,
                            const JPH::ContactManifold& manifold,
                            JPH::ContactSettings&) override;
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override;
    void OnBodyActivated(const JPH::BodyID& id, JPH::uint64) override;
    void OnBodyDeactivated(const JPH::BodyID& id, JPH::uint64) override;
};

struct World::Impl {
    BpLayerInterface bp;
    ObjectVsBpLayerFilter ovbp;
    ObjectLayerPairFilter olp;
    // Contact constraints for a fully streamed RCC world are allocated from
    // this arena every step; 8 MiB aborted inside
    // ContactConstraintManager::PrepareConstraintBuffer on NDS.  128 MiB covers
    // the 32k-body cap (Jolt's own samples use 10 MiB for 4k bodies).
    JPH::TempAllocatorImpl temp{128 * 1024 * 1024};
    JPH::JobSystemSingleThreaded jobs{4096};
    JPH::PhysicsSystem system;
    JPH::BodyInterface* bi = nullptr;
    JPH::Ref<BodyCollisionFilter> collision_filter =
        new BodyCollisionFilter();

    struct BodyRecord {
        Ref<Instance> ref;
        JPH::BodyID id;
        BodyMode mode = BodyMode::None;
        bool avatar = false;
        bool anchored = false;
        bool can_collide = true;
        int shape = 1;
        std::uint64_t owner = 0;
        Vector3 size;
        CFrame frame;
    };

    // A legacy Motor is a hinge with a velocity target.  Motor6D inherits
    // Motor, so the same record and solver path covers both classes.  Keeping
    // the hinge here (rather than creating a second physics world) means the
    // existing body integration, contact reporting and CFrame publishing all
    // observe the driven assembly.
    struct MotorRecord {
        Ref<Instance> joint;
        Ref<Instance> part0;
        Ref<Instance> part1;
        JPH::Ref<JPH::HingeConstraint> constraint;
        JPH::BodyID body0;
        JPH::BodyID body1;
        CFrame c0;
        CFrame c1;
        // Jolt reports a wrapped hinge angle.  Keep a continuous angle so a
        // multi-revolution DesiredAngle has a stable target while the solver
        // continues to use the wrapped value internally.
        double unwrapped_angle = 0.0;
        double last_wrapped_angle = 0.0;
    };

    // Keyed by the live instance pointer.  The map holds a strong Ref, so a
    // body can never outlive the part replication is about to replace.
    std::unordered_map<Instance*, BodyRecord> bodies;
    std::unordered_map<uint32_t, Instance*> by_index;
    std::unordered_map<Instance*, MotorRecord> motors;
    std::vector<PartUpdate> updates;
    std::unordered_map<Instance*, size_t> update_slot;
    double accumulator = 0.0;
    std::uint64_t steps = 0;

    // Contact bookkeeping is keyed by the sorted Jolt body indices, not by
    // Instance pointers: a removed body can still be named by an OnContactRemoved
    // callback, and the part may already have been replaced by replication.
    using ContactKey = std::pair<uint32_t, uint32_t>;
    ContactRouter contact_router;
    std::map<ContactKey, int> contacts_active;
    std::map<ContactKey, int> contacts_dormant;
    std::set<ContactKey> contacts_known;
    std::vector<contact::Pair> contact_begun;
    std::vector<contact::Pair> contact_ended;
    std::set<uint32_t> sleeping_bodies;

    Impl() {
        // A busy streaming world (NDS) replicates thousands of parts; the
        // previous 4096-body cap silently dropped colliders once avatars and
        // unanchored scenery were all represented.
        system.Init(32768, 0, 32768, 32768, bp, ovbp, olp);
        bi = &system.GetBodyInterface();
        contact_router.impl = this;
        system.SetContactListener(&contact_router);
        system.SetBodyActivationListener(&contact_router);
    }

    void remove_body(std::unordered_map<Instance*, BodyRecord>::iterator it) {
        if (it == bodies.end())
            return;
        if (!it->second.id.IsInvalid()) {
            const uint32_t index = it->second.id.GetIndexAndSequenceNumber();
            remove_motors_for_body(index);
            // Emit TouchEnded while the mapping still names both parts; Jolt's
            // removal callback may run after the body is gone.
            end_body_contacts(index, true);
            collision_filter->forget(it->second.id);
            bi->RemoveBody(it->second.id);
            bi->DestroyBody(it->second.id);
            by_index.erase(index);
        }
        bodies.erase(it);
    }

    void release_motor(MotorRecord& motor) {
        if (motor.constraint) {
            system.RemoveConstraint(motor.constraint);
            motor.constraint = nullptr;
        }
        if (!motor.body0.IsInvalid() && !motor.body1.IsInvalid())
            collision_filter->unblock(motor.body0, motor.body1);
        motor.body0 = JPH::BodyID();
        motor.body1 = JPH::BodyID();
    }

    void apply_collision_filter(const JPH::BodyID& id) {
        bi->SetCollisionGroup(
            id, JPH::CollisionGroup(collision_filter.GetPtr(),
                                    BodyCollisionFilter::kGroup,
                                    id.GetIndexAndSequenceNumber()));
    }

    void track_motor_angle(MotorRecord& motor) {
        if (!motor.constraint)
            return;
        // Jolt's hinge angle is Part1 relative to Part0, independent of which
        // endpoint is the anchored static body.
        const double wrapped = motor.constraint->GetCurrentAngle();
        motor.unwrapped_angle += std::remainder(
            wrapped - motor.last_wrapped_angle, 2.0 * M_PI);
        motor.last_wrapped_angle = wrapped;
    }

    void remove_motors_for_body(uint32_t index) {
        for (auto it = motors.begin(); it != motors.end();) {
            if (it->second.body0.GetIndexAndSequenceNumber() == index ||
                it->second.body1.GetIndexAndSequenceNumber() == index) {
                release_motor(it->second);
                it = motors.erase(it);
            } else {
                ++it;
            }
        }
    }

    void remove_motor(Instance* joint) {
        auto it = motors.find(joint);
        if (it == motors.end())
            return;
        release_motor(it->second);
        motors.erase(it);
    }

    void remove_all_motors() {
        for (auto& kv : motors)
            release_motor(kv.second);
        motors.clear();
    }

    void remove_all() {
        remove_all_motors();
        for (auto& kv : bodies) {
            if (kv.second.id.IsInvalid())
                continue;
            bi->RemoveBody(kv.second.id);
            bi->DestroyBody(kv.second.id);
        }
        bodies.clear();
        by_index.clear();
        collision_filter->clear();
        clear_contacts();
        updates.clear();
        update_slot.clear();
        accumulator = 0.0;
        steps = 0;
    }

    Instance* part_of(const JPH::BodyID& id) const {
        auto it = by_index.find(id.GetIndexAndSequenceNumber());
        return it == by_index.end() ? nullptr : it->second;
    }

    static ContactKey key_for(uint32_t a, uint32_t b) {
        return a < b ? ContactKey{a, b} : ContactKey{b, a};
    }

    static ContactKey key_for(const JPH::BodyID& a, const JPH::BodyID& b) {
        return key_for(a.GetIndexAndSequenceNumber(),
                       b.GetIndexAndSequenceNumber());
    }

    bool pair_parts(const ContactKey& key, Instance** a, Instance** b) const {
        auto ai = by_index.find(key.first);
        auto bi = by_index.find(key.second);
        if (ai == by_index.end() || bi == by_index.end())
            return false;
        *a = ai->second;
        *b = bi->second;
        return *a && *b;
    }

    void begin_pair(const ContactKey& key) {
        Instance* a = nullptr;
        Instance* b = nullptr;
        if (!pair_parts(key, &a, &b))
            return;
        if (contacts_known.insert(key).second) {
            contact::Pair pair;
            pair.a = Ref<Instance>(a);
            pair.b = Ref<Instance>(b);
            contact_begun.push_back(std::move(pair));
        }
    }

    void finish_pair(const ContactKey& key, bool emit) {
        Instance* a = nullptr;
        Instance* b = nullptr;
        if (emit && pair_parts(key, &a, &b)) {
            contact::Pair pair;
            pair.a = Ref<Instance>(a);
            pair.b = Ref<Instance>(b);
            contact_ended.push_back(std::move(pair));
        }
        contacts_active.erase(key);
        contacts_dormant.erase(key);
        contacts_known.erase(key);
    }

    void contact_added(uint32_t a, uint32_t b) {
        const ContactKey key = key_for(a, b);
        const int dormant = contacts_dormant.count(key)
                                ? contacts_dormant[key]
                                : 0;
        if (dormant) {
            contacts_dormant.erase(key);
            contacts_active[key] = dormant;
        }
        begin_pair(key);
        ++contacts_active[key];
    }

    void contact_persisted(uint32_t a, uint32_t b) {
        const ContactKey key = key_for(a, b);
        if (!contacts_active.count(key))
            contact_added(a, b);
    }

    void contact_removed(uint32_t a, uint32_t b) {
        const ContactKey key = key_for(a, b);
        auto it = contacts_active.find(key);
        if (it == contacts_active.end())
            return;
        if (--it->second > 0)
            return;
        contacts_active.erase(it);
        if (sleeping_bodies.count(key.first) ||
            sleeping_bodies.count(key.second)) {
            contacts_dormant[key] = 1;
        } else {
            finish_pair(key, true);
        }
    }

    void end_body_contacts(uint32_t index, bool emit) {
        std::vector<ContactKey> doomed;
        for (const auto& kv : contacts_active)
            if (kv.first.first == index || kv.first.second == index)
                doomed.push_back(kv.first);
        for (const auto& kv : contacts_dormant)
            if (kv.first.first == index || kv.first.second == index)
                doomed.push_back(kv.first);
        for (const ContactKey& key : doomed)
            finish_pair(key, emit);
    }

    void body_slept(uint32_t index) { sleeping_bodies.insert(index); }

    void body_woke(uint32_t index) {
        sleeping_bodies.erase(index);
        for (auto& kv : contacts_dormant)
            if (kv.first.first == index || kv.first.second == index)
                kv.second = 0;
    }

    void reconcile_dormant() {
        std::vector<ContactKey> expired;
        for (auto& kv : contacts_dormant) {
            if (sleeping_bodies.count(kv.first.first) ||
                sleeping_bodies.count(kv.first.second))
                continue;
            if (++kv.second > 2)
                expired.push_back(kv.first);
        }
        for (const ContactKey& key : expired)
            finish_pair(key, true);
    }

    void clear_contacts() {
        contacts_active.clear();
        contacts_dormant.clear();
        contacts_known.clear();
        contact_begun.clear();
        contact_ended.clear();
        sleeping_bodies.clear();
    }
};

void ContactRouter::OnContactAdded(const JPH::Body& body1,
                                   const JPH::Body& body2,
                                   const JPH::ContactManifold&,
                                   JPH::ContactSettings&) {
    impl->contact_added(body1.GetID().GetIndexAndSequenceNumber(),
                        body2.GetID().GetIndexAndSequenceNumber());
}

void ContactRouter::OnContactPersisted(const JPH::Body& body1,
                                       const JPH::Body& body2,
                                       const JPH::ContactManifold&,
                                       JPH::ContactSettings&) {
    impl->contact_persisted(body1.GetID().GetIndexAndSequenceNumber(),
                            body2.GetID().GetIndexAndSequenceNumber());
}

void ContactRouter::OnContactRemoved(const JPH::SubShapeIDPair& pair) {
    impl->contact_removed(pair.GetBody1ID().GetIndexAndSequenceNumber(),
                          pair.GetBody2ID().GetIndexAndSequenceNumber());
}

void ContactRouter::OnBodyActivated(const JPH::BodyID& id, JPH::uint64) {
    impl->body_woke(id.GetIndexAndSequenceNumber());
}

void ContactRouter::OnBodyDeactivated(const JPH::BodyID& id, JPH::uint64) {
    impl->body_slept(id.GetIndexAndSequenceNumber());
}

World::World() {
    // Must run before Impl's allocator-backed members are constructed: Jolt's
    // allocation hooks are null until RegisterDefaultAllocator().
    ensure_jolt_init();
    impl_ = std::make_unique<Impl>();
    impl_->system.SetGravity(JPH::Vec3(0.0f, -float(gravity_), 0.0f));
}
World::~World() = default;

void World::set_gravity(double value) {
    gravity_ = std::max(0.0, value);
    if (impl_)
        impl_->system.SetGravity(JPH::Vec3(0.0f, -float(gravity_), 0.0f));
}

void World::set_authority(bool client_authority,
                          std::uint64_t local_owner_id) {
    client_authority_ = client_authority;
    local_owner_id_ = local_owner_id;
}

void World::set_simulate_all_unanchored(bool on) {
    simulate_all_unanchored_ = on;
}

void World::clear() { impl_->remove_all(); }

void World::rebuild(Instance* workspace, bool simulate_unanchored,
                    const Instance* ignore_root) {
    impl_->remove_all();
    simulate_all_unanchored_ = simulate_unanchored;
    sync(workspace, ignore_root);
}

void World::sync(Instance* workspace, const Instance* ignore_root) {
    if (!workspace) {
        clear();
        return;
    }

    std::unordered_set<Instance*> seen;
    std::vector<Instance*> stack;
    for (const auto& child : workspace->children())
        stack.push_back(child.get());
    while (!stack.empty()) {
        Instance* inst = stack.back();
        stack.pop_back();
        if (!inst || inst->destroyed())
            continue;
        if (ignore_root && inst == ignore_root)
            continue;
        if (!inst->is_a("BasePart")) {
            for (const auto& child : inst->children())
                stack.push_back(child.get());
            continue;
        }

        Instance* part = inst;
        seen.insert(part);
        const bool can_collide = get_bool(part, "CanCollide", true);
        const bool anchored = get_bool(part, "Anchored", false);
        const bool avatar = part_in_character(part);
        const std::uint64_t owner = network_owner(part);
        BodyMode mode = BodyMode::Kinematic;
        if (anchored)
            mode = BodyMode::Static;
        else if (simulate_all_unanchored_ && !avatar)
            mode = BodyMode::Dynamic;
        else if (!avatar && owner == 0 && part->network_lo() == 0 &&
                 part->network_id() == 0)
            // A script-created local part has no server replication identity;
            // the native client simulates it locally even in a server-owner
            // world (the 1818 contact probe drops one onto an anchored part).
            mode = BodyMode::Dynamic;
        else if (client_authority_ && !avatar && local_owner_id_ != 0 &&
                 owner == local_owner_id_)
            mode = BodyMode::Dynamic;

        CFrame frame;
        Vector3 size{4.0, 1.0, 2.0};
        if (!get_frame(part, &frame) || !get_size(part, &size)) {
            auto old = impl_->bodies.find(part);
            if (old != impl_->bodies.end())
                impl_->remove_body(old);
            continue;
        }
        const int shape_kind = part_shape(part);

        auto it = impl_->bodies.find(part);
        if (it != impl_->bodies.end()) {
            Impl::BodyRecord& rec = it->second;
            const bool rebuilt = rec.mode != mode || rec.size.x != size.x ||
                                 rec.size.y != size.y ||
                                 rec.size.z != size.z ||
                                 rec.shape != shape_kind ||
                                 rec.anchored != anchored ||
                                 rec.can_collide != can_collide ||
                                 rec.owner != owner || rec.avatar != avatar;
            if (rebuilt) {
                impl_->remove_body(it);
            } else {
                if (mode == BodyMode::Dynamic) {
                    // A script (or a local tool) can teleport an owned body
                    // between frames; keep Jolt in sync without discarding
                    // its velocity.  Server writes are filtered out upstream
                    // by the local-CFrame override.
                    if (frames_differ(frame, rec.frame)) {
                        impl_->bi->SetPositionAndRotation(
                            rec.id, JPH::RVec3(frame.x, frame.y, frame.z),
                            quat_from_cframe(frame), JPH::EActivation::Activate);
                        rec.frame = frame;
                    }
                } else if (frames_differ(frame, rec.frame)) {
                    impl_->bi->SetPositionAndRotation(
                        rec.id, JPH::RVec3(frame.x, frame.y, frame.z),
                        quat_from_cframe(frame), JPH::EActivation::DontActivate);
                    rec.frame = frame;
                }
                continue;
            }
        }

        if (mode == BodyMode::Dynamic && !part->has_local_cframe())
            part->set_local_cframe(frame);
        if (mode != BodyMode::Dynamic && part->has_local_cframe()) {
            part->clear_local_cframe();
            get_frame(part, &frame);
        }

        JPH::Ref<JPH::Shape> shape = make_shape(size, shape_kind);
        JPH::BodyCreationSettings settings(
            shape, JPH::RVec3(frame.x, frame.y, frame.z),
            quat_from_cframe(frame),
            mode == BodyMode::Static ? JPH::EMotionType::Static
            : mode == BodyMode::Kinematic ? JPH::EMotionType::Kinematic
                                          : JPH::EMotionType::Dynamic,
            mode == BodyMode::Static ? Layers::kStatic : Layers::kMoving);
        settings.mFriction = float(std::clamp(
            get_double(part, "Friction", 0.3), 0.0, 2.0));
        settings.mRestitution = float(std::clamp(
            get_double(part, "Elasticity", 0.5), 0.0, 1.0));
        settings.mAllowSleeping = mode == BodyMode::Dynamic;
        settings.mLinearDamping = 0.0f;
        settings.mAngularDamping = 0.05f;
        if (mode == BodyMode::Dynamic) {
            // Roblox density is 0.7 mass units per cubic stud.  Let Jolt
            // calculate the inertia from the shape and scale it to that mass.
            settings.mOverrideMassProperties =
                JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass =
                float(std::max(0.01, shape_volume(size, shape_kind) * 0.7));
        }
        const JPH::BodyID id = impl_->bi->CreateAndAddBody(
            settings, mode == BodyMode::Static
                          ? JPH::EActivation::DontActivate
                      : mode == BodyMode::Kinematic
                          ? JPH::EActivation::DontActivate
                          : JPH::EActivation::Activate);
        if (id.IsInvalid())
            continue;
        if (!can_collide) {
            impl_->collision_filter->set_no_collide(id, true);
            impl_->apply_collision_filter(id);
        }
        Impl::BodyRecord rec;
        rec.ref = part;
        rec.id = id;
        rec.mode = mode;
        rec.avatar = avatar;
        rec.anchored = anchored;
        rec.can_collide = can_collide;
        rec.shape = shape_kind;
        rec.owner = owner;
        rec.size = size;
        rec.frame = frame;
        impl_->bodies.emplace(part, std::move(rec));
        impl_->by_index[id.GetIndexAndSequenceNumber()] = part;
    }

    for (auto it = impl_->bodies.begin(); it != impl_->bodies.end();) {
        if (seen.find(it->first) == seen.end() || it->first->destroyed()) {
            auto doomed = it++;
            impl_->remove_body(doomed);
        } else {
            ++it;
        }
    }

    sync_motors(workspace);
}

void World::sync_motors(Instance* workspace) {
    if (!workspace) {
        impl_->remove_all_motors();
        return;
    }

    std::unordered_set<Instance*> seen;
    std::vector<Instance*> stack;
    for (const auto& child : workspace->children())
        stack.push_back(child.get());
    while (!stack.empty()) {
        Instance* inst = stack.back();
        stack.pop_back();
        if (!inst || inst->destroyed())
            continue;
        if (inst->is_a("Motor")) {
            seen.insert(inst);
            // JointInstance.Enabled defaults to true in the engine registry.
            if (!get_bool(inst, "Enabled", true)) {
                impl_->remove_motor(inst);
                continue;
            }

            Instance* part0 = get_instance_prop(inst, "Part0");
            Instance* part1 = get_instance_prop(inst, "Part1");
            auto b0 = part0 ? impl_->bodies.find(part0) : impl_->bodies.end();
            auto b1 = part1 ? impl_->bodies.find(part1) : impl_->bodies.end();
            if (!part0 || !part1 || part0 == part1 || b0 == impl_->bodies.end() ||
                b1 == impl_->bodies.end()) {
                impl_->remove_motor(inst);
                continue;
            }

            CFrame c0;
            CFrame c1;
            if (!get_cframe_prop(inst, "C0", &c0) ||
                !get_cframe_prop(inst, "C1", &c1)) {
                impl_->remove_motor(inst);
                continue;
            }

            auto found = impl_->motors.find(inst);
            if (found != impl_->motors.end()) {
                Impl::MotorRecord& old = found->second;
                const bool same = old.constraint &&
                    old.part0.get() == part0 && old.part1.get() == part1 &&
                    old.body0 == b0->second.id && old.body1 == b1->second.id &&
                    !frames_differ(old.c0, c0) && !frames_differ(old.c1, c1);
                if (same)
                    continue;
                impl_->release_motor(old);
                impl_->motors.erase(found);
            }

            CFrame part_frame0;
            CFrame part_frame1;
            if (!get_frame(part0, &part_frame0) ||
                !get_frame(part1, &part_frame1))
                continue;
            const CFrame frame0 = frame_mul(part_frame0, c0);
            const CFrame frame1 = frame_mul(part_frame1, c1);
            JPH::HingeConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = JPH::RVec3(frame0.x, frame0.y, frame0.z);
            settings.mPoint2 = JPH::RVec3(frame1.x, frame1.y, frame1.z);
            const Vector3 hinge0 = frame_axis(frame0, 0);
            const Vector3 normal0 = frame_axis(frame0, 1);
            const Vector3 hinge1 = frame_axis(frame1, 0);
            const Vector3 normal1 = frame_axis(frame1, 1);
            settings.mHingeAxis1 = JPH::Vec3(hinge0.x, hinge0.y, hinge0.z);
            settings.mNormalAxis1 = JPH::Vec3(normal0.x, normal0.y, normal0.z);
            settings.mHingeAxis2 = JPH::Vec3(hinge1.x, hinge1.y, hinge1.z);
            settings.mNormalAxis2 = JPH::Vec3(normal1.x, normal1.y, normal1.z);
            settings.mLimitsMin = -3.14159265f;
            settings.mLimitsMax = 3.14159265f;
            auto* hinge = static_cast<JPH::HingeConstraint*>(
                impl_->bi->CreateConstraint(&settings, b0->second.id,
                                            b1->second.id));
            if (!hinge)
                continue;
            impl_->system.AddConstraint(hinge);
            impl_->collision_filter->block(b0->second.id, b1->second.id);
            impl_->apply_collision_filter(b0->second.id);
            impl_->apply_collision_filter(b1->second.id);
            Impl::MotorRecord record;
            record.joint = inst;
            record.part0 = part0;
            record.part1 = part1;
            record.constraint = hinge;
            record.body0 = b0->second.id;
            record.body1 = b1->second.id;
            record.c0 = c0;
            record.c1 = c1;
            record.unwrapped_angle = hinge->GetCurrentAngle();
            record.last_wrapped_angle = record.unwrapped_angle;
            const double initial_angle = record.unwrapped_angle;
            impl_->motors.emplace(inst, std::move(record));
            impl_->bi->ActivateConstraint(hinge);
            inst->set_internal_prop(
                "CurrentAngle", Variant(initial_angle));
            continue;
        }
        for (const auto& child : inst->children())
            stack.push_back(child.get());
    }

    for (auto it = impl_->motors.begin(); it != impl_->motors.end();) {
        if (seen.find(it->first) == seen.end() || it->first->destroyed()) {
            impl_->release_motor(it->second);
            it = impl_->motors.erase(it);
        } else {
            ++it;
        }
    }
}

void World::step(double dt) {
    if (!(dt > 0.0) || impl_->bodies.empty())
        return;
    impl_->reconcile_dormant();
    // A script can teleport an owned body between reconciliation passes.  The
    // solver publishes its pose into the instance every frame, so without an
    // explicit check here that earlier script write would be overwritten
    // before the next (30 Hz) sync sees it.  Treat a differing instance CFrame
    // as the authoritative external write and move Jolt before integrating.
    for (auto& kv : impl_->bodies) {
        Impl::BodyRecord& rec = kv.second;
        if (rec.mode != BodyMode::Dynamic || !rec.ref ||
            rec.ref->destroyed())
            continue;
        CFrame wanted;
        if (!get_frame(rec.ref.get(), &wanted))
            continue;
        if (!frames_differ(wanted, rec.frame)) {
            continue;
        }
        impl_->bi->SetPositionAndRotation(
            rec.id, JPH::RVec3(wanted.x, wanted.y, wanted.z),
            quat_from_cframe(wanted), JPH::EActivation::Activate);
        rec.frame = wanted;
    }
    bool any_dynamic = false;
    for (const auto& kv : impl_->bodies) {
        if (kv.second.mode == BodyMode::Dynamic) {
            any_dynamic = true;
            break;
        }
    }
    // A server-authority scene (Crossroads) or a streamed map whose grants
    // have not arrived yet has no local rigid bodies.  Its colliders still
    // answer character/ray queries, but there is nothing to integrate.
    if (!any_dynamic)
        return;
    impl_->accumulator = std::min(impl_->accumulator + dt, 0.25);
    int budget = 16;
    bool advanced = false;
    while (impl_->accumulator + 1e-12 >= kPhysicsStep && budget-- > 0) {
        // A Motor is a hinge with a velocity target, not a spring.  Recompute
        // the target from the *live* hinge angle every substep so a large
        // DesiredAngle cannot make the solver jump and MaxVelocity remains a
        // hard angular-rate bound.  MaxVelocity is native radians per 1/60 s
        // frame, so convert it to radians per second for the 240 Hz solver.
        for (auto& kv : impl_->motors) {
            Impl::MotorRecord& motor = kv.second;
            if (!motor.constraint || !motor.joint ||
                motor.joint->destroyed())
                continue;
            impl_->track_motor_angle(motor);
            const double max_velocity =
                get_double(motor.joint.get(), "MaxVelocity", 0.0);
            if (!(max_velocity > 0.0) || !std::isfinite(max_velocity)) {
                motor.constraint->SetMotorState(JPH::EMotorState::Off);
                continue;
            }
            const double desired =
                get_double(motor.joint.get(), "DesiredAngle", 0.0);
            const double error = desired - motor.unwrapped_angle;
            if (!std::isfinite(error)) {
                motor.constraint->SetMotorState(JPH::EMotorState::Off);
                continue;
            }
            const double max_rate = max_velocity * 60.0;
            const double target_rate = std::clamp(
                error / kPhysicsStep, -max_rate, max_rate);
            if (std::abs(target_rate) > 1e-9) {
                impl_->bi->ActivateBody(motor.body0);
                impl_->bi->ActivateBody(motor.body1);
            }
            motor.constraint->SetTargetAngularVelocity(
                float(target_rate));
            motor.constraint->SetMotorState(JPH::EMotorState::Velocity);
        }
        impl_->system.Update(float(kPhysicsStep), 1, &impl_->temp,
                             &impl_->jobs);
        for (auto& kv : impl_->motors)
            impl_->track_motor_angle(kv.second);
        impl_->accumulator -= kPhysicsStep;
        ++impl_->steps;
        advanced = true;
    }
    if (!advanced)
        return;

    // CurrentAngle is a solver view, not an input: publish the hinge's live
    // angle after the substeps so scripts and Motor6D consumers see exactly
    // the angle the physics produced this frame.
    for (auto& kv : impl_->motors) {
        Impl::MotorRecord& motor = kv.second;
        if (motor.constraint && motor.joint && !motor.joint->destroyed())
            motor.joint->set_internal_prop(
                "CurrentAngle",
                Variant(motor.unwrapped_angle));
    }

    for (auto& kv : impl_->bodies) {
        Impl::BodyRecord& rec = kv.second;
        if (rec.mode != BodyMode::Dynamic || !rec.ref ||
            rec.ref->destroyed())
            continue;
        JPH::RVec3 p;
        JPH::Quat q;
        impl_->bi->GetPositionAndRotation(rec.id, p, q);
        const CFrame frame = cframe_from_quat(p, q);
        const JPH::Vec3 lv = impl_->bi->GetLinearVelocity(rec.id);
        const JPH::Vec3 av = impl_->bi->GetAngularVelocity(rec.id);
        rec.ref->set_internal_prop(
            "AssemblyLinearVelocity",
            Variant(Vector3{lv.GetX(), lv.GetY(), lv.GetZ()}));
        rec.ref->set_internal_prop(
            "AssemblyAngularVelocity",
            Variant(Vector3{av.GetX(), av.GetY(), av.GetZ()}));
        if (!frames_differ(frame, rec.frame)) {
            continue;
        }
        std::string err;
        rec.ref->set_prop("CFrame", Variant(frame), &err);
        rec.frame = frame;
        PartUpdate up;
        up.part = rec.ref.get();
        up.cframe = frame;
        up.linear_velocity = Vector3{lv.GetX(), lv.GetY(), lv.GetZ()};
        up.angular_velocity = Vector3{av.GetX(), av.GetY(), av.GetZ()};
        auto slot = impl_->update_slot.find(rec.ref.get());
        if (slot == impl_->update_slot.end()) {
            impl_->update_slot.emplace(rec.ref.get(), impl_->updates.size());
            impl_->updates.push_back(up);
        } else {
            impl_->updates[slot->second] = up;
        }
    }
}

bool World::raycast(const Vector3& origin, const Vector3& direction,
                    double max_distance, RayHit* out,
                    const std::vector<const Instance*>* ignore) const {
    if (out)
        *out = RayHit{};
    if (!(max_distance > 0.0))
        return false;
    const JPH::Vec3 dir(float(direction.x), float(direction.y),
                        float(direction.z));
    JPH::RRayCast ray(JPH::RVec3(origin.x, origin.y, origin.z),
                      dir * float(max_distance));
    JPH::RayCastResult hit;
    IgnoreBodiesFilter filter(impl_->by_index, ignore);
    if (!impl_->system.GetNarrowPhaseQuery().CastRay(ray, hit, {}, {}, filter))
        return false;
    if (!out)
        return true;
    out->hit = true;
    out->distance = double(hit.mFraction) * max_distance;
    out->position = vadd(origin, vmul(vnormalize(direction), out->distance));
    out->instance = impl_->part_of(hit.mBodyID);
    JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), hit.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(
            hit.mSubShapeID2,
            JPH::RVec3(out->position.x, out->position.y, out->position.z));
        out->normal = Vector3{n.GetX(), n.GetY(), n.GetZ()};
    }
    return true;
}

bool World::overlaps(const Vector3& center, const Vector3& half,
                     const std::vector<const Instance*>* ignore,
                     double* depth, Vector3* normal) const {
    if (depth)
        *depth = 0.0;
    if (normal)
        *normal = Vector3{};
    const JPH::Vec3 h(float(std::max(0.01, half.x)),
                      float(std::max(0.01, half.y)),
                      float(std::max(0.01, half.z)));
    JPH::Ref<JPH::Shape> shape = new JPH::BoxShape(h);
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    IgnoreBodiesFilter filter(impl_->by_index, ignore);
    const JPH::RMat44 transform =
        JPH::RMat44::sTranslation(JPH::RVec3(center.x, center.y, center.z));
    impl_->system.GetNarrowPhaseQuery().CollideShape(
        shape, JPH::Vec3::sReplicate(1.0f), transform, settings,
        JPH::RVec3::sZero(), collector, {}, {}, filter);
    if (collector.mHits.empty())
        return false;
    const JPH::CollideShapeResult& hit = collector.mHits.front();
    if (depth)
        *depth = hit.mPenetrationDepth;
    if (normal) {
        const JPH::Vec3 n = -hit.mPenetrationAxis.Normalized();
        *normal = Vector3{n.GetX(), n.GetY(), n.GetZ()};
    }
    return true;
}

std::size_t World::size() const { return impl_->bodies.size(); }

WorldStats World::stats() const {
    WorldStats out;
    out.steps = impl_->steps;
    for (const auto& kv : impl_->bodies) {
        switch (kv.second.mode) {
        case BodyMode::Static:
            ++out.static_bodies;
            break;
        case BodyMode::Kinematic:
            ++out.kinematic_bodies;
            break;
        case BodyMode::Dynamic:
            ++out.dynamic_bodies;
            break;
        case BodyMode::None:
            break;
        }
    }
    return out;
}

bool World::body_state(const Instance* part, BodyState* out) const {
    if (out)
        *out = BodyState{};
    if (!part || !out)
        return false;
    auto it = impl_->bodies.find(const_cast<Instance*>(part));
    if (it == impl_->bodies.end())
        return false;
    const Impl::BodyRecord& rec = it->second;
    out->mode = rec.mode;
    out->sleeping = !impl_->bi->IsActive(rec.id);
    JPH::RVec3 p;
    JPH::Quat q;
    impl_->bi->GetPositionAndRotation(rec.id, p, q);
    out->cframe = cframe_from_quat(p, q);
    const JPH::Vec3 lv = impl_->bi->GetLinearVelocity(rec.id);
    const JPH::Vec3 av = impl_->bi->GetAngularVelocity(rec.id);
    out->linear_velocity = Vector3{lv.GetX(), lv.GetY(), lv.GetZ()};
    out->angular_velocity = Vector3{av.GetX(), av.GetY(), av.GetZ()};
    return true;
}

std::vector<PartUpdate> World::take_updates() {
    std::vector<PartUpdate> out;
    out.swap(impl_->updates);
    impl_->update_slot.clear();
    return out;
}

bool World::contact_query(const Instance* part,
                          std::vector<const Instance*>* out,
                          bool* grounded) const {
    if (grounded)
        *grounded = false;
    if (!part)
        return false;
    if (!get_bool(part, "CanCollide", true))
        return false;
    CFrame frame;
    Vector3 size;
    if (!get_frame(part, &frame) || !get_size(part, &size))
        return false;

    JPH::Ref<JPH::Shape> shape = make_shape(size, part_shape(part));
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const std::vector<const Instance*> ignore{part};
    ContactQueryBodyFilter filter(impl_->by_index, &ignore,
                                  impl_->collision_filter.GetPtr());
    const JPH::RMat44 transform = JPH::RMat44::sRotationTranslation(
        quat_from_cframe(frame),
        JPH::RVec3(frame.x, frame.y, frame.z));
    impl_->system.GetNarrowPhaseQuery().CollideShape(
        shape, JPH::Vec3::sReplicate(1.0f), transform, settings,
        JPH::RVec3::sZero(), collector, {}, {}, filter);

    std::set<Instance*> seen;
    bool found = false;
    for (const JPH::CollideShapeResult& hit : collector.mHits) {
        Instance* other = impl_->part_of(hit.mBodyID2);
        if (!other || other == part || other->destroyed())
            continue;
        found = true;
        if (out && seen.insert(other).second)
            out->push_back(other);
        if (grounded) {
            const JPH::Vec3 axis = hit.mPenetrationAxis.Normalized();
            if (std::abs(axis.GetY()) >= 0.7f)
                *grounded = true;
        }
    }
    return found;
}

void World::touching_parts(const Instance* part,
                           std::vector<const Instance*>* out) const {
    if (out)
        out->clear();
    contact_query(part, out, nullptr);
}

bool World::grounded(const Instance* part) const {
    bool grounded = false;
    contact_query(part, nullptr, &grounded);
    return grounded;
}

std::vector<contact::Pair> World::take_contact_begun() {
    std::vector<contact::Pair> out;
    out.swap(impl_->contact_begun);
    return out;
}

std::vector<contact::Pair> World::take_contact_ended() {
    std::vector<contact::Pair> out;
    out.swap(impl_->contact_ended);
    return out;
}

// ---------------------------------------------------------------------------
// CharacterController
// ---------------------------------------------------------------------------

struct CharacterController::Impl {
    World::Impl* world = nullptr;
    JPH::Ref<JPH::CharacterVirtual> character;
    JPH::TempAllocator* temp = nullptr;
    std::vector<CFrame> offsets;
    // Replicated avatar models are replaced/respawned under us. The controller
    // owns a strong reference so a pose write can never touch freed parts.
    Ref<Instance> character_ref;
    std::vector<Ref<Instance>> parts;
    Vector3 bounds_center;
    Vector3 root_offset; // root centre -> capsule centre
    JPH::CharacterVirtual::ExtendedUpdateSettings update_settings;

    void reset() {
        character = nullptr;
        world = nullptr;
        temp = nullptr;
        offsets.clear();
        character_ref.reset();
        parts.clear();
    }
};

namespace {

Vector3 part_half_extent(const CFrame& f, const Vector3& size) {
    // Axis-aligned half extent of an oriented box: |R| * half-size.
    return Vector3{
        std::abs(f.r00) * size.x * 0.5 + std::abs(f.r01) * size.y * 0.5 +
            std::abs(f.r02) * size.z * 0.5,
        std::abs(f.r10) * size.x * 0.5 + std::abs(f.r11) * size.y * 0.5 +
            std::abs(f.r12) * size.z * 0.5,
        std::abs(f.r20) * size.x * 0.5 + std::abs(f.r21) * size.y * 0.5 +
            std::abs(f.r22) * size.z * 0.5};
}

bool bounds_of(const std::vector<Instance*>& parts, Vector3* center,
               Vector3* half) {
    bool any = false;
    Vector3 lo{1e30, 1e30, 1e30};
    Vector3 hi{-1e30, -1e30, -1e30};
    for (Instance* part : parts) {
        CFrame f;
        Vector3 s;
        if (!get_frame(part, &f) || !get_size(part, &s))
            continue;
        const Vector3 h = part_half_extent(f, s);
        lo.x = std::min(lo.x, f.x - h.x);
        lo.y = std::min(lo.y, f.y - h.y);
        lo.z = std::min(lo.z, f.z - h.z);
        hi.x = std::max(hi.x, f.x + h.x);
        hi.y = std::max(hi.y, f.y + h.y);
        hi.z = std::max(hi.z, f.z + h.z);
        any = true;
    }
    if (!any)
        return false;
    *center = Vector3{(lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5,
                      (lo.z + hi.z) * 0.5};
    *half = Vector3{(hi.x - lo.x) * 0.5, (hi.y - lo.y) * 0.5,
                    (hi.z - lo.z) * 0.5};
    return true;
}

} // namespace

CharacterController::CharacterController() : impl_(std::make_unique<Impl>()) {}
CharacterController::~CharacterController() = default;

void CharacterController::detach() {
    impl_->reset();
    character_ = nullptr;
    root_ = nullptr;
    humanoid_ = nullptr;
    world_ = nullptr;
    state_ = CharacterState{};
    move_input_ = Vector3{};
    want_jump_ = false;
    landing_frames_ = 0;
}

bool CharacterController::attach(World* world, Instance* character) {
    detach();
    if (!world || !character || character->destroyed())
        return false;
    world_ = world;
    character_ = character;
    impl_->character_ref = character;
    humanoid_ = character->find_first_child_of_class("Humanoid");
    root_ = character->find_first_child("HumanoidRootPart");
    if (!root_) {
        for (const auto& child : character->children())
            if (child->is_a("BasePart")) {
                root_ = child.get();
                break;
            }
    }
    if (!root_) {
        detach();
        return false;
    }

    std::vector<Instance*> parts;
    collect_parts(character, &parts);
    if (parts.empty())
        parts.push_back(root_);
    impl_->parts.clear();
    impl_->parts.reserve(parts.size());
    for (Instance* part : parts)
        impl_->parts.emplace_back(part);

    Vector3 center, half;
    if (!bounds_of(parts, &center, &half)) {
        CFrame root_frame;
        get_frame(root_, &root_frame);
        center = Vector3{root_frame.x, root_frame.y, root_frame.z};
        half = Vector3{1.0, 2.5, 1.0};
    }

    const double height = std::max(2.0, half.y * 2.0);
    // Characters are box-shaped in the avatar; use the wider horizontal
    // extent so a thin torso does not produce a needle-thin capsule.
    const double radius = std::max(0.3, std::max(half.x, half.z));
    const double capsule_radius = std::min(radius * 0.9, height * 0.45);
    const double half_cylinder =
        std::max(0.05, height * 0.5 - capsule_radius);

    CFrame root_frame;
    if (!get_frame(root_, &root_frame)) {
        root_frame.x = center.x;
        root_frame.y = center.y;
        root_frame.z = center.z;
    }

    JPH::CharacterVirtualSettings settings;
    settings.mShape = new JPH::CapsuleShape(float(half_cylinder),
                                            float(capsule_radius));
    settings.mMaxSlopeAngle = JPH::DegreesToRadians(50.0f);
    settings.mSupportingVolume =
        JPH::Plane(JPH::Vec3::sAxisY(), -float(capsule_radius));
    settings.mCharacterPadding = 0.02f;
    settings.mPredictiveContactDistance = 0.1f;
    settings.mMaxStrength = 1000.0f;
    settings.mMass = 70.0f;

    impl_->world = world->impl_.get();
    impl_->temp = &world->impl_->temp;
    impl_->character = new JPH::CharacterVirtual(
        &settings, JPH::RVec3(center.x, center.y, center.z),
        JPH::Quat::sIdentity(), &world->impl_->system);
    impl_->bounds_center = center;
    impl_->root_offset = vsub(Vector3{root_frame.x, root_frame.y, root_frame.z},
                              center);
    impl_->update_settings.mStickToFloorStepDown = JPH::Vec3(0, -0.5f, 0);
    impl_->update_settings.mWalkStairsStepUp = JPH::Vec3(0, 0.4f, 0);
    impl_->update_settings.mWalkStairsStepDownExtra = JPH::Vec3::sZero();

    impl_->offsets.clear();
    const CFrame inverse = cframe_inverse(root_frame);
    for (const Ref<Instance>& part : impl_->parts) {
        CFrame f;
        if (!get_frame(part.get(), &f))
            f = root_frame;
        impl_->offsets.push_back(cframe_mul(inverse, f));
    }

    state_ = CharacterState{};
    state_.attached = true;
    state_.position = Vector3{root_frame.x, root_frame.y, root_frame.z};
    state_.cframe = root_frame;
    state_.height = height;
    state_.radius = radius;
    state_.gravity = world->gravity();
    state_.grounded = false;
    state_.state = 5; // Freefall until the first ground test.
    facing_yaw_ = std::atan2(root_frame.r02, root_frame.r22);
    return true;
}

void CharacterController::set_move_input(const Vector3& direction) {
    const double len = vlength(direction);
    move_input_ = len > 1.0 ? vmul(direction, 1.0 / len) : direction;
}

void CharacterController::request_jump(bool down) {
    if (down)
        want_jump_ = true;
}

void CharacterController::set_walk_speed(double speed) {
    state_.walk_speed = std::max(0.0, speed);
}

void CharacterController::set_jump_power(double power) {
    state_.jump_power = std::max(0.0, power);
}

void CharacterController::step(double dt) {
    if (!state_.attached || !impl_->character || !character_)
        return;
    if (character_->destroyed() || !world_) {
        detach();
        return;
    }
    if (!(dt > 0.0))
        return;
    dt = std::min(dt, 0.1);
    state_.gravity = world_->gravity();

    JPH::CharacterVirtual* c = impl_->character.GetPtr();
    const bool grounded = c->GetGroundState() ==
                          JPH::CharacterBase::EGroundState::OnGround;
    // CharacterVirtual does not integrate gravity into its own velocity: the
    // caller owns the vertical speed (see Jolt's CharacterVirtualTest, which
    // adds gravity to the velocity before ExtendedUpdate).
    double vy = state_.velocity.y;
    if (grounded)
        vy = 0.0;
    if (grounded && want_jump_ && state_.jump_power > 0.0) {
        vy = state_.jump_power;
        state_.grounded = false;
        state_.state = 3; // Jumping
        landing_frames_ = 0;
    } else if (!grounded) {
        vy -= state_.gravity * dt;
    }
    want_jump_ = false;

    const JPH::Vec3 wanted(float(move_input_.x * state_.walk_speed), float(vy),
                           float(move_input_.z * state_.walk_speed));
    c->SetLinearVelocity(wanted);
    NoCollideBodyFilter body_filter(
        impl_->world->collision_filter.GetPtr());
    c->ExtendedUpdate(
        float(dt), JPH::Vec3(0.0f, -float(state_.gravity), 0.0f),
        impl_->update_settings, JPH::BroadPhaseLayerFilter{},
        JPH::ObjectLayerFilter{}, body_filter, JPH::ShapeFilter{},
        *impl_->temp);

    const JPH::RVec3 position = c->GetPosition();
    const JPH::Vec3 after = c->GetLinearVelocity();
    const bool now_grounded =
        c->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    if (!state_.grounded && now_grounded)
        landing_frames_ = 2;

    state_.position =
        Vector3{position.GetX() - impl_->root_offset.x,
                position.GetY() - impl_->root_offset.y,
                position.GetZ() - impl_->root_offset.z};
    state_.velocity = Vector3{after.GetX(), after.GetY(), after.GetZ()};
    state_.grounded = now_grounded;
    state_.move_direction = move_input_;

    if (face_movement_ && vlength(move_input_) > 0.05) {
        const double target = std::atan2(move_input_.x, move_input_.z);
        double delta = target - facing_yaw_;
        while (delta > M_PI)
            delta -= 2.0 * M_PI;
        while (delta < -M_PI)
            delta += 2.0 * M_PI;
        facing_yaw_ += delta * std::min(1.0, dt * 12.0);
    }

    state_.cframe = cframe_yaw(state_.position, facing_yaw_);
    for (size_t i = 0;
         i < impl_->parts.size() && i < impl_->offsets.size(); ++i) {
        Instance* part = impl_->parts[i].get();
        if (!part || part->destroyed())
            continue;
        const CFrame frame = cframe_mul(state_.cframe, impl_->offsets[i]);
        std::string err;
        part->set_prop("CFrame", Variant(frame), &err);
        if (part == root_)
            part->set_local_cframe(frame);
    }

    if (now_grounded) {
        if (landing_frames_ > 0) {
            state_.state = 7; // Landed
            --landing_frames_;
        } else {
            state_.state = 8; // Running
        }
    } else if (after.GetY() > 0.1) {
        state_.state = 3; // Jumping
    } else {
        state_.state = 5; // Freefall
    }
    state_.jumped = !now_grounded && after.GetY() > 0.1;
}

void CharacterController::touching_parts(std::vector<Instance*>* out) const {
    if (!out)
        return;
    out->clear();
    if (!impl_->character || !impl_->world)
        return;
    std::set<Instance*> seen;
    for (const JPH::CharacterContact& contact :
         impl_->character->GetActiveContacts()) {
        if (!contact.mHadCollision || contact.mIsSensorB ||
            contact.mDistance > 1e-3f)
            continue;
        Instance* part = impl_->world->part_of(contact.mBodyB);
        if (part && !part->destroyed() && seen.insert(part).second)
            out->push_back(part);
    }
}

} // namespace physics
} // namespace rbx
