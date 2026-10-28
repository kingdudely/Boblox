// view_collect.cpp — pure snapshot-walk to renderable boxes (see view.h).
//
// Renderable rule: an instance contributes a box when its class chain
// declares CFrame AND Size (BasePart descendants). Color is a fixed
// neutral: snapshot Parts carry Color3uint8/BrickColor, both currently
// skipped at apply (undeclared/unresolvable) — renderer color mapping is
// a later phase, not faked here.
//
// GL-free on purpose: this TU never includes raylib, so the snapshot
// seam (collectBoxes golden in unit-engine) builds and tests everywhere,
// even when the raylib window backend is skipped for missing SDL3.
#include "view/view.h"

#include "instance/class_registry.h"
#include "instance/instance.h"
#include "instance/variant.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <variant>

namespace rbxclient {

namespace {

// The replicated avatar is a Workspace Model named after the Player. The
// official client links it into Player.Character; we use the same link when
// present. If no account id is installed (offline replay), fall back to the
// first player that has a character so a screenshot still shows an avatar.
const rbx::Instance* findLocalCharacter(const rbx::Instance* root) {
    if (!root)
        return nullptr;
    const rbx::Instance* players =
        root->find_first_child_of_class("Players");
    if (!players)
        return nullptr;
    const long long self = rbx::local_user_id();
    if (std::getenv("RBX_VIEW_DEBUG"))
        std::printf("view: players=%zu self=%lld\n", players->children().size(),
                    self);
    const rbx::Instance* fallback = nullptr;
    for (const auto& child : players->children()) {
        const rbx::Instance* player = child.get();
        if (!player->is_a("Player"))
            continue;
        rbx::Variant ch;
        if (!player->get_prop("Character", ch))
            continue;
        const auto* character = std::get_if<rbx::Instance*>(&ch);
        if (!character || !*character)
            continue;
        if (!fallback)
            fallback = *character;
        if (self == 0)
            continue;
        rbx::Variant uid;
        if (player->get_prop("UserId", uid)) {
            const auto* id = std::get_if<int64_t>(&uid);
            if (std::getenv("RBX_VIEW_DEBUG"))
                std::printf("view: player %s uid=%lld char=%s\n",
                            player->name().c_str(), id ? *id : -1,
                            (*character)->name().c_str());
            if (id && *id == self) {
                if (std::getenv("RBX_VIEW_DEBUG")) {
                    std::printf("view: local char children=%zu\n",
                                (*character)->children().size());
                    const rbx::Instance* bc = nullptr;
                    {
                        std::vector<const rbx::Instance*> bs{*character};
                        while (!bs.empty() && !bc) {
                            const rbx::Instance* n = bs.back();
                            bs.pop_back();
                            if (n->is_a("BodyColors")) {
                                bc = n;
                                break;
                            }
                            for (const auto& c : n->children())
                                bs.push_back(c.get());
                        }
                    }
                    if (bc) {
                        for (const char* k : {"HeadColor3", "TorsoColor3",
                                             "LeftArmColor3", "RightArmColor3",
                                             "LeftLegColor3", "RightLegColor3"}) {
                            rbx::Variant v;
                            if (bc->get_prop(k, v))
                                if (const auto* c = std::get_if<rbx::Color3>(&v))
                                    std::printf("view: bodycolor %s=(%.3f,%.3f,%.3f)\n",
                                                k, c->r, c->g, c->b);
                        }
                    }
                    std::vector<const rbx::Instance*> d{( *character )};
                    int shown = 0;
                    while (!d.empty() && shown < 600) {
                        const rbx::Instance* n = d.back();
                        d.pop_back();
                        std::printf("view:   child %s:%s\n",
                                    n->name().c_str(),
                                    n->class_name().c_str());
                        ++shown;
                        for (const auto& c : n->children())
                            d.push_back(c.get());
                    }
                }
                return *character;
            }
        }
    }
    return fallback;
}

bool isDescendantOf(const rbx::Instance* inst, const rbx::Instance* ancestor) {
    for (const rbx::Instance* p = inst; p; p = p->parent())
        if (p == ancestor)
            return true;
    return false;
}

} // namespace

std::vector<ViewBox> collectBoxes(const rbx::Instance* root) {
    std::vector<ViewBox> out;
    if (!root)
        return out;
    const rbx::Instance* local_character = findLocalCharacter(root);
    // Iterative DFS (worlds are thousands deep via services; no recursion).
    std::vector<const rbx::Instance*> stack{root};
    while (!stack.empty()) {
        const rbx::Instance* cur = stack.back();
        stack.pop_back();
        for (const auto& ch : cur->children())
            stack.push_back(ch.get());
        if (cur == root)
            continue;
        // Accessory handles have no replicated mesh here. Rendering their
        // bounding box paints a black slab over the avatar (the old viewer's
        // torso was mostly an accessory handle); the official client draws
        // the accessory mesh instead. Omit the box until mesh data exists.
        if (cur->name() == "Handle" && cur->parent() &&
            cur->parent()->is_a("Accessory"))
            continue;
        const rbx::PropInfo* cf = rbx::find_property(cur->class_info(), "CFrame");
        const rbx::PropInfo* sz = rbx::find_property(cur->class_info(), "Size");
        if (!cf || !sz)
            continue;
        if (cf->type != rbx::PropType::CFrame || sz->type != rbx::PropType::Vector3)
            continue;
        rbx::Variant vcf, vsz;
        if (!cur->get_prop("CFrame", vcf) || !cur->get_prop("Size", vsz))
            continue;
        const rbx::CFrame* c = std::get_if<rbx::CFrame>(&vcf);
        const rbx::Vector3* s = std::get_if<rbx::Vector3>(&vsz);
        if (!c || !s)
            continue;
        ViewBox b;
        b.px = c->x;
        b.py = c->y;
        b.pz = c->z;
        b.r00 = c->r00;
        b.r01 = c->r01;
        b.r02 = c->r02;
        b.r10 = c->r10;
        b.r11 = c->r11;
        b.r12 = c->r12;
        b.r20 = c->r20;
        b.r21 = c->r21;
        b.r22 = c->r22;
        b.sx = s->x;
        b.sy = s->y;
        b.sz = s->z;
        // Part appearance: Color3uint8 is aliased onto Color at apply,
        // Transparency and Shape ride the same BasePart/Part chains.
        rbx::Variant vcol, vtr, vshape;
        if (cur->get_prop("Color", vcol)) {
            if (const rbx::Color3* col = std::get_if<rbx::Color3>(&vcol)) {
                b.r = col->r;
                b.g = col->g;
                b.b = col->b;
            }
        }
        if (cur->get_prop("Transparency", vtr)) {
            if (const double* t = std::get_if<double>(&vtr))
                b.transparency = std::clamp(*t, 0.0, 1.0);
        }
        if (cur->get_prop("Shape", vshape)) {
            if (const rbx::EnumItem* e = std::get_if<rbx::EnumItem>(&vshape))
                b.shape = e->value;
        }
        rbx::Variant vmat, vref;
        if (cur->get_prop("Material", vmat)) {
            if (const rbx::EnumItem* e = std::get_if<rbx::EnumItem>(&vmat))
                b.material = int(e->value);
        }
        if (cur->get_prop("Reflectance", vref)) {
            if (const double* d = std::get_if<double>(&vref))
                b.reflectance = std::clamp(*d, 0.0, 1.0);
        }
        b.local_avatar = local_character &&
                         isDescendantOf(cur, local_character);
        b.name = cur->name();
        out.push_back(b);
    }
    return out;
}

CameraFocus focusCamera(const std::vector<ViewBox>& boxes) {
    CameraFocus f;
    if (boxes.empty())
        return f;
    // Non-finite positions (NaN/Inf f32s ride the wire in live physics
    // data) can never frame: skip them here so one bad Part cannot NaN
    // the whole camera. Callers can compare counts for diagnostics.
    std::vector<ViewBox> finite;
    finite.reserve(boxes.size());
    for (const auto& b : boxes) {
        if (std::isfinite(b.px) && std::isfinite(b.py) &&
            std::isfinite(b.pz) && std::isfinite(b.sx) &&
            std::isfinite(b.sy) && std::isfinite(b.sz))
            finite.push_back(b);
    }
    if (finite.empty())
        return f;
    // Local avatar first (official third-person start), then spawn pads.
    // Fall back to everything when neither exists.
    const std::vector<ViewBox>* set = &finite;
    std::vector<ViewBox> avatars;
    for (const auto& b : finite) {
        if (b.local_avatar)
            avatars.push_back(b);
    }
    std::vector<ViewBox> spawns;
    if (avatars.empty()) {
        for (const auto& b : finite) {
            if (b.name == "SpawnLocation")
                spawns.push_back(b);
        }
    }
    if (!avatars.empty()) {
        set = &avatars;
        f.has_avatar = true;
        for (const auto& b : avatars) {
            if (b.name != "HumanoidRootPart")
                continue;
            // CFrame LookVector is -Z: (-R02, -R12, -R22).
            f.fx = -b.r02;
            f.fy = -b.r12;
            f.fz = -b.r22;
            const double len = std::sqrt(f.fx * f.fx + f.fy * f.fy +
                                         f.fz * f.fz);
            if (len > 0.0) {
                f.fx /= len;
                f.fy /= len;
                f.fz /= len;
            } else {
                f.fx = 0.0;
                f.fy = 0.0;
                f.fz = -1.0;
            }
            break;
        }
    } else if (!spawns.empty()) {
        set = &spawns;
    }
    // Coordinate-wise median center (a mean would follow outliers).
    auto median_axis = [](std::vector<double> v) {
        const size_t n = v.size();
        std::nth_element(v.begin(), v.begin() + n / 2, v.end());
        if (n % 2 == 1)
            return v[n / 2];
        std::nth_element(v.begin(), v.begin() + n / 2 - 1, v.end());
        return 0.5 * (v[n / 2 - 1] + v[n / 2]);
    };
    std::vector<double> xs, ys, zs;
    xs.reserve(set->size());
    ys.reserve(set->size());
    zs.reserve(set->size());
    for (const auto& b : *set) {
        xs.push_back(b.px);
        ys.push_back(b.py);
        zs.push_back(b.pz);
    }
    f.cx = median_axis(std::move(xs));
    f.cy = median_axis(std::move(ys));
    f.cz = median_axis(std::move(zs));
    // Reach (distance + half-extent) per box, ascending. Frame the 90th
    // percentile, not the farthest box: live worlds carry skybox/decor
    // parts thousands of studs out (5,480 in the Crossroads capture) that
    // would otherwise shrink the playable area to sub-pixel specks. The
    // old "stop at the first 10x jump" rule failed on gradual chains
    // (0, 1, 10, 100, …), so the percentile is explicit here.
    std::vector<double> reach;
    reach.reserve(set->size());
    for (const auto& b : *set) {
        const double d = std::sqrt((b.px - f.cx) * (b.px - f.cx) +
                                   (b.py - f.cy) * (b.py - f.cy) +
                                   (b.pz - f.cz) * (b.pz - f.cz));
        const double half =
            0.5 * std::sqrt(b.sx * b.sx + b.sy * b.sy + b.sz * b.sz);
        reach.push_back(d + half);
    }
    std::sort(reach.begin(), reach.end());
    double radius = reach[size_t(0.90 * (reach.size() - 1))];
    if (radius < 1.0)
        radius = 1.0;
    f.radius = radius;
    return f;
}

} // namespace rbxclient
