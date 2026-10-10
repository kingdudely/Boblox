// classes/workspace.cpp — the Workspace service class (the root of the
// DataModel tree). Phase J registers the surface inherited from Instance;
// world APIs (Gravity, Camera, FindPartOnRay, …) join here later.
#include "instance/class_registry.h"
#include "instance/instance.h"

namespace rbx {

void register_class_workspace() {
    ClassInfo c;
    c.name = "Workspace";
    // Super linked by name in classes.cpp (Workspace->WorldRoot); see part.cpp.
    c.creatable = false; // services are NotCreatable (native rejects new)
    c.props = {PropInfo{"Name", PropType::String, std::string("Workspace"), false}};
    register_class(std::move(c)); // factory null = generic (see instantiate)
}

} // namespace rbx
