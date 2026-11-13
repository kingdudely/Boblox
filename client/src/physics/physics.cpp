// physics.cpp — Jolt adapter: part shapes, static world, virtual character.
#include "physics/physics.h"

#include "instance/instance.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
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

    // Keyed by the live instance pointer.  The map holds a strong Ref, so a
    // body can never outlive the part replication is about to replace.
    std::unordered_map<Instance*, BodyRecord> bodies;
    std::unordered_map<uint32_t, Instance*> by_index;
    std::vector<PartUpdate> updates;
    std::unordered_map<Instance*, size_t> update_slot;
    double accumulator = 0.0;
    std::uint64_t steps = 0;

    Impl() {
        // A busy streaming world (NDS) replicates thousands of parts; the
        // previous 4096-body cap silently dropped colliders once avatars and
        // unanchored scenery were all represented.
        system.Init(32768, 0, 32768, 32768, bp, ovbp, olp);
        bi = &system.GetBodyInterface();
    }

    void remove_body(std::unordered_map<Instance*, BodyRecord>::iterator it) {
        if (it == bodies.end())
            return;
        if (!it->second.id.IsInvalid()) {
            by_index.erase(it->second.id.GetIndexAndSequenceNumber());
            bi->RemoveBody(it->second.id);
            bi->DestroyBody(it->second.id);
        }
        bodies.erase(it);
    }

    void remove_all() {
        for (auto& kv : bodies) {
            if (kv.second.id.IsInvalid())
                continue;
            bi->RemoveBody(kv.second.id);
            bi->DestroyBody(kv.second.id);
        }
        bodies.clear();
        by_index.clear();
        updates.clear();
        update_slot.clear();
        accumulator = 0.0;
        steps = 0;
    }

    Instance* part_of(const JPH::BodyID& id) const {
        auto it = by_index.find(id.GetIndexAndSequenceNumber());
        return it == by_index.end() ? nullptr : it->second;
    }
};

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
        if (!can_collide) {
            auto old = impl_->bodies.find(part);
            if (old != impl_->bodies.end())
                impl_->remove_body(old);
            continue;
        }

        const bool anchored = get_bool(part, "Anchored", false);
        const bool avatar = part_in_character(part);
        const std::uint64_t owner = network_owner(part);
        BodyMode mode = BodyMode::Kinematic;
        if (anchored)
            mode = BodyMode::Static;
        else if (simulate_all_unanchored_ && !avatar)
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
}

void World::step(double dt) {
    if (!(dt > 0.0) || impl_->bodies.empty())
        return;
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
        impl_->system.Update(float(kPhysicsStep), 1, &impl_->temp,
                             &impl_->jobs);
        impl_->accumulator -= kPhysicsStep;
        ++impl_->steps;
        advanced = true;
    }
    if (!advanced)
        return;

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
    c->ExtendedUpdate(
        float(dt), JPH::Vec3(0.0f, -float(state_.gravity), 0.0f),
        impl_->update_settings, JPH::BroadPhaseLayerFilter{},
        JPH::ObjectLayerFilter{}, JPH::BodyFilter{}, JPH::ShapeFilter{},
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

} // namespace physics
} // namespace rbx
