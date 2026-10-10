// classes/part.cpp — the first concrete engine class (proves the pattern:
// declarative properties, zero marshalling code, base Instance methods for
// free). More classes (MeshPart, Script, Player, …) follow this shape.
#include "instance/class_registry.h"
#include "instance/instance.h"

namespace rbx {

void register_class_part() {
    ClassInfo c;
    c.name = "Part";
    // Super linked by name in classes.cpp (Part->FormFactorPart); never set
    // here — registration order must not matter (see class_registry.h).
    c.creatable = true;
    // Delta-only: everything else arrives through the generated BasePart
    // chain with identical types (Anchored, Locked, Transparency,
    // Reflectance, Position). Hand rows exist ONLY where the dump cannot
    // supply the value: the class name itself and real defaults the Mini
    // dump does not carry (Size, CanCollide, Elasticity, Friction).
    c.props = {
        PropInfo{"Name", PropType::String, std::string("Part"), false},
        PropInfo{"Size", PropType::Vector3, Vector3{4.0, 1.0, 2.0}, false},
        PropInfo{"CanCollide", PropType::Bool, true, false},
        PropInfo{"Elasticity", PropType::Double, 0.5, false}, // defaultElasticity()
        PropInfo{"Friction", PropType::Double, 0.3, false},   // defaultFriction()
    };
    register_class(std::move(c)); // factory null = generic (see instantiate)
}

} // namespace rbx
