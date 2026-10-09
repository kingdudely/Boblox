// classes/workspace.cpp — the Workspace service class (the root of the
// DataModel tree). Phase J registers the surface inherited from Instance;
// world APIs (Gravity, Camera, FindPartOnRay, …) join here later.
#include "instance/class_registry.h"
#include "instance/instance.h"

namespace rbx {

void register_class_workspace() {
    ClassInfo c;
    c.name = "Workspace";
    c.super = find_class("Instance");
    c.props = {PropInfo{"Name", PropType::String, std::string("Workspace"), false}};
    c.factory = [] { return Ref<Instance>(new Instance(find_class("Workspace"))); };
    register_class(std::move(c));
}

} // namespace rbx
