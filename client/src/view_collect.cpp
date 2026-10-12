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

} // namespace rbxclient
