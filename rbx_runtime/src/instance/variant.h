// variant.h — property value types for the instance reflection system.
//
// Grow the list (and the marshalling in class_registry.cpp) as more classes
// land: CFrame, EnumItem, Color3, arrays, … Keep Lua marshalling in ONE
// place (class_registry.cpp) so every class gets it for free.
#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace rbx {

struct Vector3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    bool operator==(const Vector3& o) const {
        return x == o.x && y == o.y && z == o.z;
    }
};

enum class PropType : uint8_t { Bool, Int, Double, String, Vector3, Instance };

using Variant = std::variant<bool, int64_t, double, std::string, Vector3, struct Instance*>;

} // namespace rbx
