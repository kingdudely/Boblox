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

void register_class_part();      // classes/part.cpp
void register_class_workspace(); // classes/workspace.cpp

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
    base.factory = [] { return Ref<Instance>(new Instance(find_class("Instance"))); };
    register_class(std::move(base));

    register_class_part();
    register_class_workspace();
}

} // namespace rbx
