// classes.cpp — the explicit list of built-in engine classes.
//
// Same discipline as api/registry.cpp: a static-lib linker drops
// unreferenced translation units, so every class registration function is
// declared here and called explicitly. Adding a class:
//   1. write instance/classes/<name>.cpp exposing register_class_<name>()
//   2. declare + call it below
//   3. add the .cpp to challenge_core in CMakeLists.txt
#include "instance/class_registry.h"
#include "instance/control_state.h"
#include "instance/gui_input.h"
#include "instance/instance.h"

namespace rbx {

void register_class_part();      // classes/part.cpp (hand behavior)
void register_class_workspace(); // classes/workspace.cpp (hand behavior)
void register_generated_classes(); // classes/generated.cpp (dump registry)
void register_instance_methods();   // methods.cpp (hand behaviors by name)

void register_builtin_classes() {
    static bool done = false;
    if (done)
        return;
    done = true;

    ClassInfo base;
    base.name = "Instance";
    // Name is the only property here; ClassName is served dynamically by the
    // instance glue (its value is the concrete class name).
    base.props = {
        PropInfo{"Name", PropType::String, std::string("Instance"), false},
        PropInfo{"Archivable", PropType::Bool, true, false},
    };
    // Universal Instance signals. Generated classes inherit these through the
    // super chain; the hand-owned root has to declare them itself.
    base.events = {
        EventInfo{"ChildAdded"},         EventInfo{"ChildRemoved"},
        EventInfo{"DescendantAdded"},    EventInfo{"DescendantRemoving"},
        EventInfo{"AncestryChanged"},    EventInfo{"Destroying"},
        EventInfo{"AttributeChanged"},
    };
    register_class(std::move(base)); // factory null = generic (see instantiate)

    // Shells first (order-free: nobody resolves supers at register time)...
    register_class_part();
    register_class_workspace();
    register_generated_classes();
    // Native Weld/WeldConstraint instances are enabled when created; the
    // generated dump leaves JointInstance.Enabled at its primitive default.
    // The assembly query uses this bit, so restore the native default here.
    attach_property("JointInstance",
                    PropInfo{"Enabled", PropType::Bool, true, false});
    attach_property("WeldConstraint",
                    PropInfo{"Enabled", PropType::Bool, true, false});
    // Wire-only network ownership field.  The native client exposes it as a
    // hidden BasePart property; replication writes it as an unsigned varint
    // (tag 34), and the client physics world uses it to decide which bodies
    // it is allowed to simulate.  Zero means the server owns the part.
    ensure_property("BasePart",
                    PropInfo{"NetworkOwnerV3", PropType::Int, int64_t(0),
                             false});
    // ...then links, by name. Hand supers stay dump-faithful with an
    // Instance fallback if generation ever lags behind.
    if (!link_super("Part", "FormFactorPart"))
        link_super("Part", "Instance");
    if (!link_super("Workspace", "WorldRoot"))
        link_super("Workspace", "Instance");

    // Hand behaviors attach onto method slots (generated or hand alike).
    register_instance_methods();
    install_control_state_methods();
    gui::install_methods();
}

} // namespace rbx
