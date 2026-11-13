#include "instance/surface_joints.h"

#include "instance/assembly.h"
#include "instance/class_registry.h"
#include "instance/instance.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace rbx::surface_joints {
namespace {

// Native MakeJoints consumes the collision contact manifold.  The auto-joint
// frame is the contacting face's constant frame (the probe established that
// for every part face); a face-adjacent pair creates one edge and
// interpenetrating boxes do not.  The tests below are deliberately
// face-generic: native joins side surfaces as well as Top/Bottom.
constexpr double kContactEps = 1e-6;
constexpr double kPenetrationEps = 1e-4;

// Enum.SurfaceType values (tools/api_dump.json): Smooth=0, Glue=1, Weld=2,
// Studs=3, Inlet=4, Universal=5, Hinge=6, Motor=7, SteppingMotor=8,
// SmoothNoOutlines=10.
constexpr int kSurfaceGlue = 1;
constexpr int kSurfaceWeld = 2;
constexpr int kSurfaceStuds = 3;
constexpr int kSurfaceInlet = 4;
constexpr int kSurfaceUniversal = 5;
constexpr int kSurfaceHinge = 6;
constexpr int kSurfaceMotor = 7;
constexpr int kSurfaceSteppingMotor = 8;

enum class Face : int { Top = 0, Bottom = 1, Right = 2, Left = 3, Front = 4, Back = 5 };
constexpr int kFaceCount = 6;

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

CFrame mul_cf(const CFrame& a, const CFrame& b) {
    CFrame out;
    out.r00 = a.r00 * b.r00 + a.r01 * b.r10 + a.r02 * b.r20;
    out.r01 = a.r00 * b.r01 + a.r01 * b.r11 + a.r02 * b.r21;
    out.r02 = a.r00 * b.r02 + a.r01 * b.r12 + a.r02 * b.r22;
    out.r10 = a.r10 * b.r00 + a.r11 * b.r10 + a.r12 * b.r20;
    out.r11 = a.r10 * b.r01 + a.r11 * b.r11 + a.r12 * b.r21;
    out.r12 = a.r10 * b.r02 + a.r11 * b.r12 + a.r12 * b.r22;
    out.r20 = a.r20 * b.r00 + a.r21 * b.r10 + a.r22 * b.r20;
    out.r21 = a.r20 * b.r01 + a.r21 * b.r11 + a.r22 * b.r21;
    out.r22 = a.r20 * b.r02 + a.r21 * b.r12 + a.r22 * b.r22;
    out.x = a.r00 * b.x + a.r01 * b.y + a.r02 * b.z + a.x;
    out.y = a.r10 * b.x + a.r11 * b.y + a.r12 * b.z + a.y;
    out.z = a.r20 * b.x + a.r21 * b.y + a.r22 * b.z + a.z;
    return out;
}

CFrame make_frame(const Vec3& pos, const Vec3& x, const Vec3& y,
                  const Vec3& z) {
    CFrame out;
    out.x = pos.x;
    out.y = pos.y;
    out.z = pos.z;
    out.r00 = x.x;
    out.r01 = y.x;
    out.r02 = z.x;
    out.r10 = x.y;
    out.r11 = y.y;
    out.r12 = z.y;
    out.r20 = x.z;
    out.r21 = y.z;
    out.r22 = z.z;
    return out;
}

const char* face_property(Face face) {
    switch (face) {
    case Face::Top:
        return "TopSurface";
    case Face::Bottom:
        return "BottomSurface";
    case Face::Right:
        return "RightSurface";
    case Face::Left:
        return "LeftSurface";
    case Face::Front:
        return "FrontSurface";
    case Face::Back:
        return "BackSurface";
    }
    return "TopSurface";
}

// Native C0/C1 rotation for each part face, from the live face-frame probe.
// The rotation is constant in the owning part's local frame; the position is
// the face centre.  C1 keeps this rotation and moves the origin to the same
// world point in the other part's local frame.
CFrame face_joint_frame(const Vector3& size, Face face) {
    const double hx = size.x * 0.5;
    const double hy = size.y * 0.5;
    const double hz = size.z * 0.5;
    CFrame out;
    switch (face) {
    case Face::Top:
        out = make_frame({0.0, hy, 0.0}, {-1.0, 0.0, 0.0},
                         {0.0, 0.0, 1.0}, {0.0, 1.0, 0.0});
        break;
    case Face::Bottom:
        out = make_frame({0.0, -hy, 0.0}, {1.0, 0.0, 0.0},
                         {0.0, 0.0, 1.0}, {0.0, -1.0, 0.0});
        break;
    case Face::Right:
        out = make_frame({hx, 0.0, 0.0}, {0.0, 0.0, -1.0},
                         {0.0, 1.0, 0.0}, {1.0, 0.0, 0.0});
        break;
    case Face::Left:
        out = make_frame({-hx, 0.0, 0.0}, {0.0, 0.0, 1.0},
                         {0.0, 1.0, 0.0}, {-1.0, 0.0, 0.0});
        break;
    case Face::Front:
        out = make_frame({0.0, 0.0, -hz}, {-1.0, 0.0, 0.0},
                         {0.0, 1.0, 0.0}, {0.0, 0.0, -1.0});
        break;
    case Face::Back:
        out = make_frame({0.0, 0.0, hz}, {1.0, 0.0, 0.0},
                         {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0});
        break;
    }
    return out;
}

// The face rectangle is a zero-thickness box in the part's local plane.  Its
// two in-plane half extents are returned separately from the native joint
// frame, which uses the constant C0 rotation above rather than this basis.
CFrame face_rect_frame(const CFrame& part, const Vector3& size, Face face,
                       Vec3* half) {
    const double hx = size.x * 0.5;
    const double hy = size.y * 0.5;
    const double hz = size.z * 0.5;
    CFrame local;
    switch (face) {
    case Face::Top:
        local = make_frame({0.0, hy, 0.0}, {1.0, 0.0, 0.0},
                           {0.0, 0.0, 1.0}, {0.0, 1.0, 0.0});
        *half = {size.x * 0.5, size.z * 0.5, 0.0};
        break;
    case Face::Bottom:
        local = make_frame({0.0, -hy, 0.0}, {1.0, 0.0, 0.0},
                           {0.0, 0.0, -1.0}, {0.0, -1.0, 0.0});
        *half = {size.x * 0.5, size.z * 0.5, 0.0};
        break;
    case Face::Right:
        local = make_frame({hx, 0.0, 0.0}, {0.0, 1.0, 0.0},
                           {0.0, 0.0, 1.0}, {1.0, 0.0, 0.0});
        *half = {size.y * 0.5, size.z * 0.5, 0.0};
        break;
    case Face::Left:
        local = make_frame({-hx, 0.0, 0.0}, {0.0, 1.0, 0.0},
                           {0.0, 0.0, 1.0}, {-1.0, 0.0, 0.0});
        *half = {size.y * 0.5, size.z * 0.5, 0.0};
        break;
    case Face::Front:
        local = make_frame({0.0, 0.0, -hz}, {1.0, 0.0, 0.0},
                           {0.0, 1.0, 0.0}, {0.0, 0.0, -1.0});
        *half = {size.x * 0.5, size.y * 0.5, 0.0};
        break;
    case Face::Back:
        local = make_frame({0.0, 0.0, hz}, {1.0, 0.0, 0.0},
                           {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0});
        *half = {size.x * 0.5, size.y * 0.5, 0.0};
        break;
    }
    return mul_cf(part, local);
}

// Separating-axis test for two oriented boxes.  The caller's surface is a
// zero-thickness box (a rectangle), so the same test proves that the face
// rectangle touches the other part's box at any rotation.
bool box_overlap_depth(const CFrame& a, const Vec3& ha, const CFrame& b,
                       const Vec3& hb, double eps, double* depth) {
    const Vec3 axes_a[3] = {axis(a, 0), axis(a, 1), axis(a, 2)};
    const Vec3 axes_b[3] = {axis(b, 0), axis(b, 1), axis(b, 2)};
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

int surface_value(const Instance* part, const char* prop) {
    Variant value;
    if (!part->get_prop(prop, value))
        return -1;
    const auto* item = std::get_if<EnumItem>(&value);
    return item ? item->value : -1;
}

int face_surface(const Instance* part, Face face) {
    return surface_value(part, face_property(face));
}

// C1 is the winning face's world point expressed in the other part's local
// frame, with the same rotation as C0.  Native probe: top/side and every
// other face keep the C0 rotation at both ends.
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

bool face_contact(Instance* caller, Instance* other, Face face, CFrame* c0,
                  CFrame* c1) {
    CFrame caller_cf{}, other_cf{};
    Vector3 caller_size{}, other_size{};
    if (!cframe_and_size(caller, &caller_cf, &caller_size) ||
        !cframe_and_size(other, &other_cf, &other_size))
        return false;

    Vec3 half{};
    const CFrame rect = face_rect_frame(caller_cf, caller_size, face, &half);
    const Vec3 caller_half{caller_size.x * 0.5, caller_size.y * 0.5,
                           caller_size.z * 0.5};
    const Vec3 other_half{other_size.x * 0.5, other_size.y * 0.5,
                          other_size.z * 0.5};
    double penetration = 0.0;
    if (box_overlap_depth(caller_cf, caller_half, other_cf, other_half,
                          kContactEps, &penetration) &&
        penetration > kPenetrationEps)
        return false;
    if (!box_overlap_depth(rect, half, other_cf, other_half, kContactEps,
                           &penetration))
        return false;

    // Coplanar faces that meet only along an edge have zero contact area and
    // do not create a native surface joint.
    const Vec3 in_plane[2] = {axis(rect, 0), axis(rect, 1)};
    const Vec3 other_axes[3] = {axis(other_cf, 0), axis(other_cf, 1),
                                axis(other_cf, 2)};
    const double in_half[2] = {half.x, half.y};
    const Vec3 centre_delta{other_cf.x - rect.x, other_cf.y - rect.y,
                            other_cf.z - rect.z};
    for (int i = 0; i < 2; ++i) {
        double other_radius = 0.0;
        for (int j = 0; j < 3; ++j) {
            const double g = j == 0 ? other_half.x
                             : j == 1 ? other_half.y
                                      : other_half.z;
            other_radius += g * std::abs(dot(in_plane[i], other_axes[j]));
        }
        if (in_half[i] + other_radius -
                std::abs(dot(centre_delta, in_plane[i])) <= kContactEps)
            return false;
    }

    *c0 = face_joint_frame(caller_size, face);
    *c1 = other_cframe(other_cf, *c0, rect);
    return true;
}

struct ContactFace {
    Instance* part = nullptr;
    Face face = Face::Top;
    int surface = -1;
    CFrame c0{};
    CFrame c1{};
};

void collect_contact_faces(Instance* part, Instance* other,
                           std::vector<ContactFace>* out) {
    for (int i = 0; i < kFaceCount; ++i) {
        const Face face = static_cast<Face>(i);
        CFrame c0{}, c1{};
        if (!face_contact(part, other, face, &c0, &c1))
            continue;
        out->push_back({part, face, face_surface(part, face), c0, c1});
    }
}

const char* dynamic_class(int surface) {
    switch (surface) {
    case kSurfaceHinge:
        return "Rotate";
    case kSurfaceMotor:
        return "RotateV";
    case kSurfaceSteppingMotor:
        return "RotateP";
    default:
        return nullptr;
    }
}

bool snap_surface(int surface) {
    return surface == kSurfaceStuds || surface == kSurfaceInlet ||
           surface == kSurfaceUniversal;
}

// Native Snap pairing: Studs+Inlet, Studs+Universal and Inlet+Universal
// mate; equal Studs and equal Inlet do not.  Universal+Universal does.
bool snap_pair(int a, int b) {
    return snap_surface(a) && snap_surface(b) && (a != b || a == kSurfaceUniversal);
}

const char* rigid_class(const std::vector<ContactFace>& own,
                        const std::vector<ContactFace>& other) {
    bool weld = false;
    bool glue = false;
    for (const ContactFace& face : own) {
        weld = weld || face.surface == kSurfaceWeld;
        glue = glue || face.surface == kSurfaceGlue;
    }
    for (const ContactFace& face : other) {
        weld = weld || face.surface == kSurfaceWeld;
        glue = glue || face.surface == kSurfaceGlue;
    }
    if (weld)
        return "Weld";
    if (glue)
        return "Glue";
    for (const ContactFace& a : own) {
        for (const ContactFace& b : other) {
            if (snap_pair(a.surface, b.surface))
                return "Snap";
        }
    }
    return nullptr;
}

const ContactFace* first_dynamic(const std::vector<ContactFace>& faces) {
    for (const ContactFace& face : faces) {
        if (dynamic_class(face.surface))
            return &face;
    }
    return nullptr;
}

bool has_joint_between(Instance* a, Instance* b) {
    std::vector<Instance*> joints;
    assembly::joints(a, &joints);
    for (Instance* joint : joints) {
        if (!joint || joint->destroyed() || !assembly::is_joint_like(joint))
            continue;
        Instance* p0 = assembly::joint_part(joint, "Part0");
        Instance* p1 = assembly::joint_part(joint, "Part1");
        if ((p0 == a && p1 == b) || (p0 == b && p1 == a))
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
    // Native parents the automatic joint to the rigid caller, or to the part
    // whose dynamic face chose the class.
    if (!joint->reparent(owner, nullptr))
        return;
}

} // namespace

void make_joints(Instance* part) {
    if (!part || !part->is_a("BasePart"))
        return;
    Instance* scope = assembly::workspace_scope(part);
    if (!scope)
        return;

    std::vector<Instance*> parts;
    collect_parts(scope, &parts);
    for (Instance* other : parts) {
        if (!other || other == part || other->destroyed())
            continue;

        std::vector<ContactFace> own;
        std::vector<ContactFace> theirs;
        collect_contact_faces(part, other, &own);
        collect_contact_faces(other, part, &theirs);
        if (own.empty() || theirs.empty())
            continue;

        // Dynamic faces win over rigid faces.  A dynamic face on the caller
        // wins over the other part's dynamic face (native order probe: a
        // lower Hinge+upper Motor makes Rotate when the lower calls, but
        // RotateV when the upper calls).
        const ContactFace* dynamic = first_dynamic(own);
        Instance* dynamic_owner = part;
        if (!dynamic) {
            dynamic = first_dynamic(theirs);
            dynamic_owner = other;
        }
        if (dynamic) {
            if (has_joint_between(part, other))
                continue;
            create_joint(dynamic_class(dynamic->surface), dynamic_owner,
                         dynamic_owner == part ? other : part, dynamic->c0,
                         dynamic->c1);
            continue;
        }

        const char* cls = rigid_class(own, theirs);
        if (!cls || has_joint_between(part, other))
            continue;
        // Rigid joints are native caller-owned.  C0 is the caller's
        // contacting face even when the other face supplied Weld/Glue.
        create_joint(cls, part, other, own.front().c0, own.front().c1);
    }
}

void make_joints_tree(Instance* root) {
    if (!root)
        return;
    // Native Model:MakeJoints walks the model's descendants in child order
    // and runs the BasePart pass on every part.  A nested model is therefore
    // processed before the model's later direct children, exactly like
    // recursion over the live child list; the per-part pass still sees the
    // whole Workspace as contact geometry (native scope probe).
    std::vector<Instance*> parts;
    collect_parts(root, &parts);
    for (Instance* part : parts)
        make_joints(part);
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
            if (!joint || joint->destroyed() ||
                !assembly::is_rigid_joint(joint))
                continue;
            if (std::find(doomed.begin(), doomed.end(), joint) == doomed.end())
                doomed.push_back(joint);
            Instance* p0 = assembly::joint_part(joint, "Part0");
            Instance* p1 = assembly::joint_part(joint, "Part1");
            Instance* other = p0 == current ? p1 : (p1 == current ? p0 : nullptr);
            if (other && other->is_a("BasePart") &&
                std::find(visited.begin(), visited.end(), other) ==
                    visited.end()) {
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
