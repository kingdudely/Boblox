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
#include "view.h"

#include "instance/class_registry.h"
#include "instance/instance.h"
#include "instance/variant.h"

#include <algorithm>
#include <cmath>
#include <variant>

namespace rbxclient {

std::vector<ViewBox> collectBoxes(const rbx::Instance* root) {
    std::vector<ViewBox> out;
    if (!root)
        return out;
    // Iterative DFS (worlds are thousands deep via services; no recursion).
    std::vector<const rbx::Instance*> stack{root};
    while (!stack.empty()) {
        const rbx::Instance* cur = stack.back();
        stack.pop_back();
        for (const auto& ch : cur->children())
            stack.push_back(ch.get());
        if (cur == root)
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
    // Spawn-focus subset; fall back to everything when no SpawnLocation
    // boxes exist (name match is exact: renamed spawns don't count).
    const std::vector<ViewBox>* set = &finite;
    std::vector<ViewBox> spawns;
    for (const auto& b : finite) {
        if (b.name == "SpawnLocation")
            spawns.push_back(b);
    }
    if (!spawns.empty())
        set = &spawns;
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
    // Distances from center, ascending; cut before the first 10x jump so
    // far-flung (but finite) physics outliers cannot empty the frame.
    struct DistHalf {
        double d = 0.0;
        double half = 0.0;
    };
    std::vector<DistHalf> ds;
    ds.reserve(set->size());
    for (const auto& b : *set) {
        const double d = std::sqrt((b.px - f.cx) * (b.px - f.cx) +
                                   (b.py - f.cy) * (b.py - f.cy) +
                                   (b.pz - f.cz) * (b.pz - f.cz));
        const double half =
            0.5 * std::sqrt(b.sx * b.sx + b.sy * b.sy + b.sz * b.sz);
        ds.push_back({d, half});
    }
    std::sort(ds.begin(), ds.end(), [](const DistHalf& a, const DistHalf& b) {
        return a.d < b.d;
    });
    double radius = 0;
    for (size_t i = 0; i < ds.size(); ++i) {
        // Cut before the first order-of-magnitude jump (floor keeps
        // legitimately close neighbors together from a zero base).
        if (i > 0 && ds[i].d > 10.0 * std::max({ds[i - 1].d, radius, 1.0}))
            break; // outliers beyond this cannot empty the frame
        if (ds[i].d + ds[i].half > radius)
            radius = ds[i].d + ds[i].half;
    }
    if (radius < 1.0)
        radius = 1.0;
    f.radius = radius;
    return f;
}

} // namespace rbxclient
