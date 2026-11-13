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
    c.props = {
        PropInfo{"Name", PropType::String, std::string("Workspace"), false},
        // Client camera. The GUI module creates a default Camera when --gui
        // is on; a renderer can replace it without changing the tree model.
        PropInfo{"CurrentCamera", PropType::Instance,
                 static_cast<Instance*>(nullptr), false},
        // Native class defaults. The API dump leaves these at primitive zero,
        // but the live client starts every place at Earth gravity and lets
        // NetworkOwnerV3 grants choose the local solver. Without the defaults
        // a place that does not replicate Workspace.Gravity (189707) would
        // have no gravity at all and every local body would stay kinematic.
        PropInfo{"Gravity", PropType::Double, 196.2, false},
        PropInfo{"AuthorityMode", PropType::Enum,
                 EnumItem{"AuthorityMode", 1}, false, "AuthorityMode"},
    };
    register_class(std::move(c)); // factory null = generic (see instantiate)
}

} // namespace rbx
