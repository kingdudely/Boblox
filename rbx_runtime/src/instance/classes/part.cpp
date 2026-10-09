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
    };
    c.factory = [] { return Ref<Instance>(new Instance(find_class("Part"))); };
    register_class(std::move(c));
}

} // namespace rbx
