// classes/part.cpp — the first concrete engine class (proves the pattern:
// declarative properties, zero marshalling code, base Instance methods for
// free). More classes (MeshPart, Script, Player, …) follow this shape.
#include "instance/class_registry.h"
#include "instance/instance.h"

namespace rbx {

void register_class_part() {
    ClassInfo c;
    c.name = "Part";
    c.super = find_class("Instance");
    c.props = {
        PropInfo{"Name", PropType::String, std::string("Part"), false},
        PropInfo{"Position", PropType::Vector3, Vector3{0.0, 0.0, 0.0}, false},
        PropInfo{"Size", PropType::Vector3, Vector3{4.0, 1.0, 2.0}, false},
        PropInfo{"Anchored", PropType::Bool, false, false},
        // 2016-sourced (PartInstance.cpp prop_* — same script names, types,
        // and defaults in the modern API; defaults cross-checked):
        PropInfo{"Transparency", PropType::Double, 0.0, false},
        PropInfo{"Reflectance", PropType::Double, 0.0, false},
        PropInfo{"Elasticity", PropType::Double, 0.5, false}, // defaultElasticity()
        PropInfo{"Friction", PropType::Double, 0.3, false},   // defaultFriction()
        PropInfo{"CanCollide", PropType::Bool, true, false},
        PropInfo{"Locked", PropType::Bool, false, false},
    };
    c.factory = [] { return Ref<Instance>(new Instance(find_class("Part"))); };
    register_class(std::move(c));
}

} // namespace rbx
