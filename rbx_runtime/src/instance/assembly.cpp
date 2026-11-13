#include "instance/assembly.h"

#include "instance/instance.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace rbx::assembly {
namespace {

bool bool_prop(const Instance* inst, const char* name, bool fallback) {
    Variant value;
    if (!inst->get_prop(name, value))
        return fallback;
    const auto* b = std::get_if<bool>(&value);
    return b ? *b : fallback;
}

Instance* top_ancestor(Instance* node) {
    while (node && node->parent())
        node = node->parent();
    return node;
}

int root_priority(const Instance* inst) {
    Variant value;
    if (!inst->get_prop("RootPriority", value))
        return 0;
    const auto* i = std::get_if<int64_t>(&value);
    if (!i)
        return 0;
    // Native stores RootPriority in a signed byte; its documented range is
    // -127..127.  Clamp so a script value outside that range cannot compare
    // as a wider number than the native field can hold.
    return int(std::max<int64_t>(-127, std::min<int64_t>(127, *i)));
}

Vector3 part_size(const Instance* inst) {
    Variant value;
    if (!inst->get_prop("Size", value))
        return Vector3{};
    if (const auto* size = std::get_if<Vector3>(&value))
        return *size;
    return Vector3{};
}

double root_score(const Instance* inst) {
    const Vector3 size = part_size(inst);
    const double largest_face =
        std::max({size.x * size.y, size.x * size.z, size.y * size.z});
    double multiplier = 1.0;
    if (inst->is_a("Seat") || inst->is_a("VehicleSeat"))
        multiplier = 20.0;
    else if (inst->name() == "HumanoidRootPart")
        multiplier = 10.0;
    else if (inst->name() == "Torso")
        multiplier = 5.0;
    // This is the native legacy sort: floor(maxPlanarSize * 50) times the
    // special multiplier.  The 50 is part of the native ordering (it matters
    // at floor boundaries), not a unit conversion.
    return std::floor(largest_face * 50.0) * multiplier;
}

// Native ties use the client's instance creation order.  A replicated part
// exists before the local script runs, so a later local part wins the mixed
// tie (native mixed-tie probe); the wire id itself is not the sort key.
uint64_t order_key(const Instance* inst) {
    return inst->creation_seq();
}

bool better_root_rank(const Instance* candidate, const Instance* current) {
    if (!current)
        return true;

    const bool candidate_massless = bool_prop(candidate, "Massless", false);
    const bool current_massless = bool_prop(current, "Massless", false);
    if (candidate_massless != current_massless)
        return !candidate_massless;

    const int candidate_priority = root_priority(candidate);
    const int current_priority = root_priority(current);
    if (candidate_priority != current_priority)
        return candidate_priority > current_priority;

    const double candidate_score = root_score(candidate);
    const double current_score = root_score(current);
    if (candidate_score != current_score)
        return candidate_score > current_score;

    return false;
}

bool better_candidate(const Instance* candidate, const Instance* current) {
    if (!current)
        return true;
    if (better_root_rank(candidate, current))
        return true;
    if (better_root_rank(current, candidate))
        return false;
    return order_key(candidate) > order_key(current);
}

void collect_joint_like(Instance* root, std::vector<Instance*>* out) {
    for (const auto& child : root->children()) {
        Instance* inst = child.get();
        if (is_joint_like(inst))
            out->push_back(inst);
        collect_joint_like(inst, out);
    }
}

bool contains_instance(const std::vector<Instance*>& list, Instance* inst) {
    return std::find(list.begin(), list.end(), inst) != list.end();
}

bool is_descendant_of(Instance* node, Instance* ancestor) {
    for (; node; node = node->parent())
        if (node == ancestor)
            return true;
    return false;
}

bool is_in_joints_service(Instance* joint) {
    for (Instance* node = joint; node; node = node->parent())
        if (node->is_a("JointsService"))
            return true;
    return false;
}

void collect_active_joints(Instance* part, std::vector<Instance*>* out) {
    out->clear();
    Instance* scope = workspace_scope(part);
    if (!scope)
        return;
    std::vector<Instance*> all;
    collect_joint_like(top_ancestor(part), &all);
    for (Instance* joint : all) {
        if (!is_descendant_of(joint, scope) && !is_in_joints_service(joint))
            continue;
        if (!is_rigid_joint(joint) || !joint_enabled(joint))
            continue;
        out->push_back(joint);
    }
}

Instance* best_candidate(const std::vector<Instance*>& candidates) {
    Instance* best = nullptr;
    for (Instance* candidate : candidates) {
        if (!candidate || candidate->destroyed())
            continue;
        if (better_candidate(candidate, best))
            best = candidate;
    }
    return best;
}

struct AssemblyResolution {
    std::vector<Instance*> members;
    std::vector<Instance*> active_joints;
    // Union-find over unanchored members only.  Anchored parts are separate
    // roots and never join an unanchored component.
    std::vector<size_t> parent;
    // The anchored winner for each unanchored component, or null when the
    // component has no anchor-incident joint.
    std::vector<Instance*> chosen_anchor;
};

size_t member_index(const std::vector<Instance*>& members, const Instance* inst) {
    for (size_t i = 0; i < members.size(); ++i)
        if (members[i] == inst)
            return i;
    return members.size();
}

size_t component_root(AssemblyResolution* res, size_t index) {
    size_t root = index;
    while (res->parent[root] != root)
        root = res->parent[root];
    while (res->parent[index] != index) {
        size_t next = res->parent[index];
        res->parent[index] = root;
        index = next;
    }
    return root;
}

size_t join_components(AssemblyResolution* res, size_t a, size_t b) {
    const size_t root_a = component_root(res, a);
    const size_t root_b = component_root(res, b);
    if (root_a == root_b)
        return root_a;
    res->parent[root_b] = root_a;
    return root_a;
}

struct JointEdge {
    Instance* joint = nullptr;
    Instance* a = nullptr;
    Instance* b = nullptr;
    Instance* anchor = nullptr;
};

bool edge_anchor_before(const JointEdge& lhs, const JointEdge& rhs) {
    if ((lhs.anchor != nullptr) != (rhs.anchor != nullptr))
        return lhs.anchor != nullptr;
    if (lhs.anchor && rhs.anchor) {
        if (better_root_rank(lhs.anchor, rhs.anchor))
            return true;
        if (better_root_rank(rhs.anchor, lhs.anchor))
            return false;
    }
    return lhs.joint->creation_seq() > rhs.joint->creation_seq();
}

// Native solver activity keeps anchored parts as separate roots and assigns
// each unanchored component to one winning anchor.  The winner is chosen by
// anchor rank, with the newest anchor-incident joint breaking an equal-rank
// tie.  Only that one anchor link is active; all other anchor links are
// inactive.  Within the unanchored component the links form a maximum
// spanning forest (newest first), so cycles are inactive.  This is the one
// selection pass used by both AssemblyRootPart and JointInstance.Active.
bool resolve_assembly(Instance* part, AssemblyResolution* out) {
    out->members.clear();
    out->active_joints.clear();
    out->parent.clear();
    out->chosen_anchor.clear();
    connected_parts(part, true, &out->members);
    if (out->members.empty())
        return false;

    std::vector<Instance*> joints;
    collect_active_joints(part, &joints);
    std::vector<JointEdge> unanchored_edges;
    std::vector<JointEdge> anchor_edges;
    for (Instance* joint : joints) {
        if (!is_rigid_joint(joint) || !joint_enabled(joint))
            continue;
        Instance* a = joint_part(joint, "Part0");
        Instance* b = joint_part(joint, "Part1");
        if (!a || !b || a == b)
            continue;
        if (member_index(out->members, a) == out->members.size() ||
            member_index(out->members, b) == out->members.size())
            continue;

        const bool a_anchor = bool_prop(a, "Anchored", false);
        const bool b_anchor = bool_prop(b, "Anchored", false);
        if (a_anchor && b_anchor)
            continue; // both anchored: no unanchored component can use it
        if (a_anchor || b_anchor) {
            anchor_edges.push_back({joint, a, b, a_anchor ? a : b});
        } else {
            unanchored_edges.push_back({joint, a, b, nullptr});
        }
    }

    const size_t count = out->members.size();
    out->parent.resize(count);
    out->chosen_anchor.assign(count, nullptr);
    for (size_t i = 0; i < count; ++i) {
        out->parent[i] = i;
    }

    // Build the unanchored components first.  Newest joints win within a
    // cycle, matching the native complete-tie ordering.
    std::stable_sort(unanchored_edges.begin(), unanchored_edges.end(),
                     [](const JointEdge& lhs, const JointEdge& rhs) {
                         return lhs.joint->creation_seq() >
                                rhs.joint->creation_seq();
                     });
    for (const JointEdge& edge : unanchored_edges) {
        const size_t a = member_index(out->members, edge.a);
        const size_t b = member_index(out->members, edge.b);
        const size_t root_a = component_root(out, a);
        const size_t root_b = component_root(out, b);
        if (root_a == root_b)
            continue; // cycle
        join_components(out, root_a, root_b);
        out->active_joints.push_back(edge.joint);
    }

    // For each unanchored component, choose at most one anchor link.  Anchor
    // rank dominates; equal-rank anchors fall back to the newest joint.
    std::vector<JointEdge> chosen(count);
    std::vector<bool> has_chosen(count, false);
    for (const JointEdge& edge : anchor_edges) {
        const bool a_anchor = bool_prop(edge.a, "Anchored", false);
        Instance* unanchored = a_anchor ? edge.b : edge.a;
        const size_t index = member_index(out->members, unanchored);
        if (index == out->members.size())
            continue;
        const size_t root = component_root(out, index);
        if (!has_chosen[root] || edge_anchor_before(edge, chosen[root])) {
            chosen[root] = edge;
            has_chosen[root] = true;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (!has_chosen[i])
            continue;
        out->chosen_anchor[i] = chosen[i].anchor;
        out->active_joints.push_back(chosen[i].joint);
    }
    return true;
}

Instance* resolved_root(AssemblyResolution* res, Instance* part) {
    const size_t index = member_index(res->members, part);
    if (index == res->members.size())
        return nullptr;
    if (bool_prop(part, "Anchored", false))
        return part;

    const size_t root = component_root(res, index);
    if (Instance* anchor = res->chosen_anchor[root])
        return anchor;

    // No anchored winner for this unanchored component: native root ranking
    // applies within the component.
    Instance* best = nullptr;
    for (size_t i = 0; i < res->members.size(); ++i) {
        Instance* candidate = res->members[i];
        if (!candidate || bool_prop(candidate, "Anchored", false) ||
            component_root(res, i) != root)
            continue;
        if (!best || better_candidate(candidate, best))
            best = candidate;
    }
    return best;
}

} // namespace

bool is_joint_like(const Instance* inst) {
    return inst && (inst->is_a("JointInstance") || inst->is_a("WeldConstraint"));
}

bool is_rigid_joint(const Instance* inst) {
    return inst && (inst->is_a("Weld") || inst->is_a("Snap") ||
                    inst->is_a("ManualWeld") || inst->is_a("Motor") ||
                    inst->is_a("Motor6D") || inst->is_a("WeldConstraint"));
}

bool joint_enabled(const Instance* inst) {
    Variant value;
    if (!inst || !inst->get_prop("Enabled", value))
        return false;
    const auto* enabled = std::get_if<bool>(&value);
    return enabled && *enabled;
}

Instance* joint_part(const Instance* joint, const char* prop) {
    Variant value;
    if (!joint || !joint->get_prop(prop, value))
        return nullptr;
    const auto* part = std::get_if<Instance*>(&value);
    return part ? *part : nullptr;
}

Instance* workspace_scope(Instance* part) {
    for (Instance* node = part; node; node = node->parent())
        if (node->is_a("Workspace") || node->is_a("WorldModel"))
            return node;
    return nullptr;
}

void connected_parts(Instance* part, bool recursive,
                     std::vector<Instance*>* out) {
    out->clear();
    if (!part || !part->is_a("BasePart"))
        return;
    out->push_back(part);
    if (!workspace_scope(part))
        return;

    std::vector<Instance*> joints;
    collect_active_joints(part, &joints);

    auto append_neighbours = [&](Instance* current,
                                 std::vector<Instance*>* neighbours) {
        for (Instance* joint : joints) {
            Instance* p0 = joint_part(joint, "Part0");
            Instance* p1 = joint_part(joint, "Part1");
            if (!p0 || !p1 || p0 == p1)
                continue;
            Instance* other =
                p0 == current ? p1 : (p1 == current ? p0 : nullptr);
            if (other && other->is_a("BasePart") && !other->destroyed() &&
                !contains_instance(*neighbours, other))
                neighbours->push_back(other);
        }
    };

    std::vector<Instance*> frontier{part};
    std::vector<Instance*> visited{part};
    while (!frontier.empty()) {
        Instance* current = frontier.back();
        frontier.pop_back();
        std::vector<Instance*> neighbours;
        append_neighbours(current, &neighbours);
        for (Instance* other : neighbours) {
            if (!contains_instance(*out, other))
                out->push_back(other);
            if (recursive && !contains_instance(visited, other)) {
                visited.push_back(other);
                frontier.push_back(other);
            }
        }
    }
}

void joints(Instance* part, std::vector<Instance*>* out) {
    out->clear();
    if (!part || !part->is_a("BasePart"))
        return;
    std::vector<Instance*> all;
    collect_joint_like(top_ancestor(part), &all);
    for (Instance* joint : all) {
        if (joint_part(joint, "Part0") == part ||
            joint_part(joint, "Part1") == part)
            out->push_back(joint);
    }
}

Instance* root_part(Instance* part) {
    if (!part || !part->is_a("BasePart") || !workspace_scope(part))
        return nullptr;
    if (bool_prop(part, "Anchored", false))
        return part;

    AssemblyResolution resolution;
    if (!resolve_assembly(part, &resolution))
        return nullptr;
    return resolved_root(&resolution, part);
}

bool joint_active(Instance* joint) {
    if (!joint || !is_joint_like(joint) || !is_rigid_joint(joint) ||
        !joint_enabled(joint))
        return false;
    Instance* a = joint_part(joint, "Part0");
    Instance* b = joint_part(joint, "Part1");
    if (!a || !b || a == b || !workspace_scope(a))
        return false;

    AssemblyResolution resolution;
    if (!resolve_assembly(a, &resolution))
        return false;
    return contains_instance(resolution.active_joints, joint);
}

} // namespace rbx::assembly
