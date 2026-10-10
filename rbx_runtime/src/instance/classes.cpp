// classes.cpp — the explicit list of built-in engine classes.
//
// Same discipline as api/registry.cpp: a static-lib linker drops
// unreferenced translation units, so every class registration function is
// declared here and called explicitly. Adding a class:
//   1. write instance/classes/<name>.cpp exposing register_class_<name>()
//   2. declare + call it below
//   3. add the .cpp to challenge_core in CMakeLists.txt
#include "instance/class_registry.h"
#include "instance/instance.h"

namespace rbx {

void register_class_part();      // classes/part.cpp (hand behavior)
void register_class_workspace(); // classes/workspace.cpp (hand behavior)
void register_generated_classes(); // classes/generated.cpp (dump registry)

void register_builtin_classes() {
    static bool done = false;
    if (done)
        return;
    done = true;

    ClassInfo base;
    base.name = "Instance";
    // Name is the only property here; ClassName is served dynamically by the
    // instance glue (its value is the concrete class name).
    base.props = {PropInfo{"Name", PropType::String, std::string("Instance"), false}};
    register_class(std::move(base)); // factory null = generic (see instantiate)

    // Shells first (order-free: nobody resolves supers at register time)...
    register_class_part();
    register_class_workspace();
    register_generated_classes();
    // ...then links, by name. Hand supers stay dump-faithful with an
    // Instance fallback if generation ever lags behind.
    if (!link_super("Part", "FormFactorPart"))
        link_super("Part", "Instance");
    if (!link_super("Workspace", "WorldRoot"))
        link_super("Workspace", "Instance");
}

} // namespace rbx
