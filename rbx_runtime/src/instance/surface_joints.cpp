#include "instance/surface_joints.h"

#include "instance/assembly.h"
#include "instance/class_registry.h"
#include "instance/instance.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace rbx::surface_joints {
namespace {

// Native MakeJoints consumes the collision contact manifold.  The probe
// established that the auto-joint frame is anchored at the caller's surface
// centre, not at the centre of the overlap rectangle.  A face-adjacent pair
// creates the edge; interpenetrating boxes do not (native overlap case), so
// the rectangle test rejects a volume overlap deeper than the contact slop.
constexpr double kContactEps = 1e-6;
constexpr double kPenetrationEps = 1e-4;

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

Vec3 axis(const CFrame& cf, int i) {
    if (i == 0)
        return {cf.r00, cf.r10, cf.r20};
    if (i == 1)
        return {cf.r01, cf.r11, cf.r21};
    return {cf.r02, cf.r12, cf.r22};
}

Vec3 sub(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

double dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Separating-axis test for two oriented boxes.  The caller's surface is a
// zero-thickness box (a rectangle), so the same test proves that the face
// rectangle touches the other part's box at any caller rotation.  Native
// created the Rotate for Z90, X90 and Z180 callers in the rotation probe.
// `depth` is the smallest projection overlap; a positive value on two solid
// boxes is their penetration depth, used to reject the native overlap case.
bool box_overlap_depth(const CFrame& a, const Vec3& ha, const CFrame& b,
                       const Vec3& hb, double eps, double* depth) {
    Vec3 axes_a[3] = {axis(a, 0), axis(a, 1), axis(a, 2)};
    Vec3 axes_b[3] = {axis(b, 0), axis(b, 1), axis(b, 2)};
    const Vec3 d = sub({b.x, b.y, b.z}, {a.x, a.y, a.z});
    double t[3] = {dot(d, axes_a[0]), dot(d, axes_a[1]),
                   dot(d, axes_a[2])};
    double r[3][3];
    double ar[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r[i][j] = dot(axes_a[i], axes_b[j]);
            ar[i][j] = std::abs(r[i][j]);
        }
    }
    const double h[3] = {ha.x, ha.y, ha.z};
    const double g[3] = {hb.x, hb.y, hb.z};
    double min_overlap = std::numeric_limits<double>::infinity();
    const auto test = [&](double ra, double rb, double tv, bool measure) {
        const double overlap = ra + rb - std::abs(tv);
        if (overlap < -eps)
            return false;
        if (measure)
            min_overlap = std::min(min_overlap, overlap);
        return true;
    };

    for (int i = 0; i < 3; ++i) {
        const double rb = g[0] * ar[i][0] + g[1] * ar[i][1] +
                          g[2] * ar[i][2];
        if (!test(h[i], rb, t[i], true))
            return false;
    }
    for (int j = 0; j < 3; ++j) {
        const double ra = h[0] * ar[0][j] + h[1] * ar[1][j] +
                          h[2] * ar[2][j];
        const double tb = t[0] * r[0][j] + t[1] * r[1][j] +
                          t[2] * r[2][j];
        if (!test(ra, g[j], tb, true))
            return false;
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const int i1 = (i + 1) % 3;
            const int i2 = (i + 2) % 3;
            const int j1 = (j + 1) % 3;
            const int j2 = (j + 2) % 3;
            const double ra = h[i1] * ar[i2][j] + h[i2] * ar[i1][j];
            const double rb = g[j1] * ar[i][j2] + g[j2] * ar[i][j1];
            const double tv = t[i2] * r[i1][j] - t[i1] * r[i2][j];
            if (!test(ra, rb, tv, false))
                return false;
        }
    }
    *depth = min_overlap;
    return true;
}

bool cframe_and_size(const Instance* inst, CFrame* cf, Vector3* size) {
    Variant cf_value;
    if (!inst->get_prop("CFrame", cf_value))
        return false;
    const auto* frame = std::get_if<CFrame>(&cf_value);
    if (!frame)
        return false;
    Variant size_value;
    if (!inst->get_prop("Size", size_value))
        return false;
    const auto* extent = std::get_if<Vector3>(&size_value);
    if (!extent)
        return false;
    *cf = *frame;
    *size = *extent;
    return true;
}

void collect_parts(Instance* root, std::vector<Instance*>* out) {
    for (const auto& child : root->children()) {
        Instance* inst = child.get();
        if (inst->is_a("BasePart"))
            out->push_back(inst);
        collect_parts(inst, out);
    }
}

CFrame face_frame(const Vector3& size, bool top) {
    const double y = size.y * 0.5 * (top ? 1.0 : -1.0);
    CFrame out{};
    out.x = 0.0;
    out.y = y;
    out.z = 0.0;
    // Native Rotate C0/C1 for a top surface uses [-1,0,0; 0,0,1; 0,1,0];
    // the bottom surface uses [1,0,0; 0,0,-1; 0,1,0].  The matrices are
    // local to both ends and stay fixed under a yawed caller, exactly as the
    // native order probe prints them.
    out.r00 = top ? -1.0 : 1.0;
    out.r01 = 0.0;
    out.r02 = 0.0;
    out.r10 = 0.0;
    out.r11 = 0.0;
    out.r12 = top ? 1.0 : -1.0;
    out.r20 = 0.0;
    out.r21 = 1.0;
    out.r22 = 0.0;
    return out;
}

// The caller's face centre in the other part's local frame.  C0 stays at the
// caller's local face centre; C1 is the same world point expressed by Part1.
CFrame other_cframe(const CFrame& other_cf, const CFrame& local_face,
                    const CFrame& world_face) {
    const double dx = world_face.x - other_cf.x;
    const double dy = world_face.y - other_cf.y;
    const double dz = world_face.z - other_cf.z;
    CFrame out = local_face;
    out.x = other_cf.r00 * dx + other_cf.r10 * dy + other_cf.r20 * dz;
    out.y = other_cf.r01 * dx + other_cf.r11 * dy + other_cf.r21 * dz;
    out.z = other_cf.r02 * dx + other_cf.r12 * dy + other_cf.r22 * dz;
    return out;
}

int surface_value(const Instance* part, const char* prop) {
    Variant value;
    if (!part->get_prop(prop, value))
        return -1;
    const auto* item = std::get_if<EnumItem>(&value);
    return item ? item->value : -1;
}

bool face_contact(Instance* caller, Instance* other, bool top,
                  CFrame* c0, CFrame* c1) {
    CFrame caller_cf{}, other_cf{};
    Vector3 caller_size{}, other_size{};
    if (!cframe_and_size(caller, &caller_cf, &caller_size) ||
        !cframe_and_size(other, &other_cf, &other_size))
        return false;

    const CFrame face = face_frame(caller_size, top);
    CFrame world_face = face;
    world_face.x = caller_cf.x + caller_cf.r01 * face.y;
    world_face.y = caller_cf.y + caller_cf.r11 * face.y;
    world_face.z = caller_cf.z + caller_cf.r21 * face.y;
    world_face.r00 = caller_cf.r00;
    world_face.r01 = caller_cf.r01;
    world_face.r02 = caller_cf.r02;
    world_face.r10 = caller_cf.r10;
    world_face.r11 = caller_cf.r11;
    world_face.r12 = caller_cf.r12;
    world_face.r20 = caller_cf.r20;
    world_face.r21 = caller_cf.r21;
    world_face.r22 = caller_cf.r22;

    const Vec3 caller_half{caller_size.x * 0.5, caller_size.y * 0.5,
                           caller_size.z * 0.5};
    const Vec3 rectangle{caller_size.x * 0.5, 0.0, caller_size.z * 0.5};
    const Vec3 other_half{other_size.x * 0.5, other_size.y * 0.5,
                          other_size.z * 0.5};
    double penetration = 0.0;
    if (box_overlap_depth(caller_cf, caller_half, other_cf, other_half,
                          kContactEps, &penetration) &&
        penetration > kPenetrationEps)
        return false;
    if (!box_overlap_depth(world_face, rectangle, other_cf, other_half,
                           kContactEps, &penetration))
        return false;

    // Coplanar top faces that meet only along an edge project onto both
    // in-plane axes, but their overlap area is zero.  Native does not create
    // a surface joint for that edge contact; a real contact has positive
    // overlap on both face axes.
    const Vec3 in_plane[2] = {axis(world_face, 0), axis(world_face, 2)};
    const Vec3 other_axes[3] = {axis(other_cf, 0), axis(other_cf, 1),
                                axis(other_cf, 2)};
    const double in_half[2] = {rectangle.x, rectangle.z};
    const Vec3 centre_delta{other_cf.x - world_face.x,
                            other_cf.y - world_face.y,
                            other_cf.z - world_face.z};
    for (int i = 0; i < 2; ++i) {
        double other_radius = 0.0;
        for (int j = 0; j < 3; ++j) {
            const double g = j == 0 ? other_half.x
                             : j == 1 ? other_half.y
                                      : other_half.z;
            other_radius += g * std::abs(dot(in_plane[i], other_axes[j]));
        }
        if (in_half[i] + other_radius - std::abs(dot(centre_delta,
                                                      in_plane[i])) <=
            kContactEps)
            return false;
    }

    *c0 = face;
    *c1 = other_cframe(other_cf, face, world_face);
    return true;
}

bool has_joint(Instance* caller, Instance* other, const char* cls) {
    std::vector<Instance*> joints;
    assembly::joints(caller, &joints);
    for (Instance* joint : joints) {
        if (!joint || joint->destroyed() || joint->class_name() != cls)
            continue;
        Instance* p0 = assembly::joint_part(joint, "Part0");
        Instance* p1 = assembly::joint_part(joint, "Part1");
        if ((p0 == caller && p1 == other) ||
            (p0 == other && p1 == caller))
            return true;
    }
    return false;
}

void create_joint(const char* cls_name, Instance* owner, Instance* other,
                  const CFrame& c0, const CFrame& c1) {
    const ClassInfo* cls = find_class(cls_name);
    if (!cls)
        return;
    Ref<Instance> joint = instantiate(cls);
    if (!joint)
        return;
    joint->set_prop("Part0", Variant(owner), nullptr);
    joint->set_prop("Part1", Variant(other), nullptr);
    joint->set_prop("C0", Variant(c0), nullptr);
    joint->set_prop("C1", Variant(c1), nullptr);
    joint->set_prop("Enabled", Variant(true), nullptr);
    // Native parents the automatic joint to the part whose surface chose the
    // class: the caller for its Top/Bottom surface, or the other part when
    // its Bottom Hinge is the one that wins the contact.
    if (!joint->reparent(owner, nullptr))
        return;
}

const char* top_surface_joint(int surface) {
    switch (surface) {
    case 6: // Enum.SurfaceType.Hinge
        return "Rotate";
    case 7: // Enum.SurfaceType.Motor
        return "RotateV";
    case 8: // Enum.SurfaceType.SteppingMotor
        return "RotateP";
    default:
        return nullptr;
    }
}

} // namespace

void make_joints(Instance* part) {
    if (!part || !part->is_a("BasePart"))
        return;
    Instance* scope = assembly::workspace_scope(part);
    if (!scope)
        return;

    const int top_surface = surface_value(part, "TopSurface");
    const int bottom_surface = surface_value(part, "BottomSurface");
    const char* top_class = top_surface_joint(top_surface);
    const bool bottom_hinge = bottom_surface == 6;

    std::vector<Instance*> parts;
    collect_parts(scope, &parts);
    for (Instance* other : parts) {
        if (!other || other == part || other->destroyed())
            continue;

        // Native processes the contact pair, not just the receiver: a
        // Smooth caller touching a Bottom-Hinge part produces a Rotate owned
        // by the hinge part.  Top Hinge/Motor/SteppingMotor wins over a
        // Bottom Hinge; a non-special top surface (Smooth/Weld/Glue) lets the
        // other part's Bottom Hinge create the edge.
        const int other_top_surface = surface_value(other, "TopSurface");
        const int other_bottom_surface =
            surface_value(other, "BottomSurface");
        const char* other_top_class = top_surface_joint(other_top_surface);
        const bool other_bottom_hinge = other_bottom_surface == 6;

        CFrame c0{}, c1{};
        if (top_class &&
            face_contact(part, other, true, &c0, &c1) &&
            !has_joint(part, other, top_class)) {
            create_joint(top_class, part, other, c0, c1);
            continue;
        }
        if (other_top_class &&
            face_contact(other, part, true, &c0, &c1) &&
            !has_joint(other, part, other_top_class)) {
            create_joint(other_top_class, other, part, c0, c1);
            continue;
        }
        if (bottom_hinge &&
            face_contact(part, other, false, &c0, &c1) &&
            !has_joint(part, other, "Rotate")) {
            create_joint("Rotate", part, other, c0, c1);
            continue;
        }
        if (other_bottom_hinge &&
            face_contact(other, part, false, &c0, &c1) &&
            !has_joint(other, part, "Rotate")) {
            create_joint("Rotate", other, part, c0, c1);
        }
    }
}

void break_joints(Instance* part) {
    if (!part || !part->is_a("BasePart"))
        return;

    std::vector<Instance*> frontier{part};
    std::vector<Instance*> visited{part};
    std::vector<Instance*> doomed;
    while (!frontier.empty()) {
        Instance* current = frontier.back();
        frontier.pop_back();
        std::vector<Instance*> attached;
        assembly::joints(current, &attached);
        for (Instance* joint : attached) {
            if (!joint || joint->destroyed() || !assembly::is_rigid_joint(joint))
                continue;
            if (std::find(doomed.begin(), doomed.end(), joint) == doomed.end())
                doomed.push_back(joint);
            Instance* p0 = assembly::joint_part(joint, "Part0");
            Instance* p1 = assembly::joint_part(joint, "Part1");
            Instance* other = p0 == current ? p1 : (p1 == current ? p0 : nullptr);
            if (other && other->is_a("BasePart") &&
                std::find(visited.begin(), visited.end(), other) == visited.end()) {
                visited.push_back(other);
                frontier.push_back(other);
            }
        }
    }
    for (Instance* joint : doomed)
        joint->destroy();
}

void break_joints_tree(Instance* root) {
    if (!root)
        return;
    // Copy the child list: Destroy mutates the live vector.  Model:BreakJoints
    // removes every legacy joint in the subtree, including Rotate and
    // WeldConstraint; BasePart:BreakJoints keeps non-rigid joints.
    std::vector<Ref<Instance>> children = root->children();
    for (const Ref<Instance>& child : children) {
        Instance* inst = child.get();
        if (inst->is_a("JointInstance") || inst->is_a("WeldConstraint"))
            inst->destroy();
        else
            break_joints_tree(inst);
    }
}

} // namespace rbx::surface_joints
