// input_object.cpp — see input_object.h.
#include "instance/input_object.h"

#include "instance/class_registry.h"

namespace rbx {
namespace input_objects {

Ref<Instance> make(lua_State* L, int input_type, int input_state, int keycode,
                   double x, double y) {
    (void)L;
    const ClassInfo* ic = find_class("InputObject");
    if (!ic)
        return Ref<Instance>();
    Ref<Instance> obj = instantiate(ic);
    obj->set_prop("UserInputType",
                  Variant(EnumItem{"UserInputType", input_type}), nullptr);
    obj->set_prop("UserInputState",
                  Variant(EnumItem{"UserInputState", input_state}), nullptr);
    if (keycode != 0)
        obj->set_prop("KeyCode", Variant(EnumItem{"KeyCode", keycode}),
                      nullptr);
    obj->set_prop("Position", Variant(Vector3{x, y, 0.0}), nullptr);
    obj->set_prop("Delta", Variant(Vector3{0.0, 0.0, 0.0}), nullptr);
    return obj;
}

} // namespace input_objects
} // namespace rbx
