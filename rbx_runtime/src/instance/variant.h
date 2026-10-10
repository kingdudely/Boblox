// variant.h — property value types for the instance reflection system.
//
// Grow the list (and the marshalling in class_registry.cpp) as more classes
// land. Keep Lua marshalling in ONE place (class_registry.cpp) so every
// class gets it for free.
//
// Discipline: PropType kinds and Variant alternatives stay parallel and
// APPEND-ONLY (set_prop compares alternative indices; reordering would
// silently break that check).
#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace rbx {

struct Vector3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    bool operator==(const Vector3& o) const {
        return x == o.x && y == o.y && z == o.z;
    }
};

// 12 numbers: position (x, y, z) + rotation rows R00..R22 — the exact
// GetComponents order, so the 12-arg constructor maps 1:1.
struct CFrame {
    double x = 0.0, y = 0.0, z = 0.0;
    double r00 = 1.0, r01 = 0.0, r02 = 0.0;
    double r10 = 0.0, r11 = 1.0, r12 = 0.0;
    double r20 = 0.0, r21 = 0.0, r22 = 1.0;

    bool operator==(const CFrame& o) const {
        return x == o.x && y == o.y && z == o.z && r00 == o.r00 && r01 == o.r01 &&
               r02 == o.r02 && r10 == o.r10 && r11 == o.r11 && r12 == o.r12 &&
               r20 == o.r20 && r21 == o.r21 && r22 == o.r22;
    }
};

struct Vector2 {
    double x = 0.0;
    double y = 0.0;

    bool operator==(const Vector2& o) const {
        return x == o.x && y == o.y;
    }
};

struct Color3 {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;

    bool operator==(const Color3& o) const {
        return r == o.r && g == o.g && b == o.b;
    }
};

// Resolved through the static palette (brickcolor.cpp): number is the whole
// identity (name/RGB derive from it).
struct BrickColor {
    int number = 0;

    bool operator==(const BrickColor& o) const {
        return number == o.number;
    }
};

struct UDim {
    double scale = 0.0;
    double offset = 0.0;

    bool operator==(const UDim& o) const {
        return scale == o.scale && offset == o.offset;
    }
};

struct UDim2 {
    UDim x;
    UDim y;

    bool operator==(const UDim2& o) const {
        return x == o.x && y == o.y;
    }
};

struct Rect {
    Vector2 min;
    Vector2 max;

    bool operator==(const Rect& o) const {
        return min == o.min && max == o.max;
    }
};

struct NumberRange {
    double min = 0.0;
    double max = 0.0;

    bool operator==(const NumberRange& o) const {
        return min == o.min && max == o.max;
    }
};

// Keypoints are userdata (typeof/Keypoints arrays) but never property types,
// so they live outside Variant/PropType.
struct NumberSequenceKeypoint {
    double time = 0.0;
    double value = 0.0;
    double envelope = 0.0;

    bool operator==(const NumberSequenceKeypoint& o) const {
        return time == o.time && value == o.value && envelope == o.envelope;
    }
};

struct NumberSequence {
    std::vector<NumberSequenceKeypoint> keys;

    bool operator==(const NumberSequence& o) const {
        return keys == o.keys;
    }
};

struct ColorSequenceKeypoint {
    double time = 0.0;
    Color3 color;
    double envelope = 0.0;

    bool operator==(const ColorSequenceKeypoint& o) const {
        return time == o.time && color == o.color && envelope == o.envelope;
    }
};

struct ColorSequence {
    std::vector<ColorSequenceKeypoint> keys;

    bool operator==(const ColorSequence& o) const {
        return keys == o.keys;
    }
};

// Source kinds mirror Enum.ContentSourceType values (api_dump.json):
// 0 none, 1 uri, 2 object. (Opaque has no script surface.)
struct Content {
    int source = 0;
    std::string uri;
    struct Instance* object = nullptr; // non-owning, like Variant Instance*

    bool operator==(const Content& o) const {
        return source == o.source && uri == o.uri && object == o.object;
    }
};

struct PhysicalProperties {
    double density = 0.0;
    double friction = 0.0;
    double elasticity = 0.0;
    double friction_weight = 0.0;
    double elasticity_weight = 0.0;
    double acoustic_absorption = 0.0;

    bool operator==(const PhysicalProperties& o) const {
        return density == o.density && friction == o.friction && elasticity == o.elasticity &&
               friction_weight == o.friction_weight && elasticity_weight == o.elasticity_weight &&
               acoustic_absorption == o.acoustic_absorption;
    }
};

struct Ray {
    Vector3 origin;
    Vector3 direction;

    bool operator==(const Ray& o) const {
        return origin == o.origin && direction == o.direction;
    }
};

struct Region3 {
    Vector3 min;
    Vector3 max;

    bool operator==(const Region3& o) const {
        return min == o.min && max == o.max;
    }
};

// Millis since the Unix epoch (int64: exact round-trips through Lua numbers
// only up to 2^53 — construction/props use integers, never floats).
struct DateTime {
    int64_t millis = 0;

    bool operator==(const DateTime& o) const {
        return millis == o.millis;
    }
};

// EnumItem: (owning enum, value). Equality needs BOTH (cross-enum items
// with the same value compare false, matching Roblox).
struct EnumItem {
    std::string enum_name;
    int value = 0;

    bool operator==(const EnumItem& o) const {
        return enum_name == o.enum_name && value == o.value;
    }
};

enum class PropType : uint8_t {
    Bool,
    Int,
    Double,
    String,
    Vector3,
    Instance,
    Color3,
    CFrame,
    Vector2,
    BrickColor,
    UDim,
    UDim2,
    Rect,
    NumberRange,
    NumberSequence,
    ColorSequence,
    Content,
    PhysicalProperties,
    Ray,
    Region3,
    DateTime,
    Enum // values are EnumItem structs (owning enum + value)
};

using Variant = std::variant<bool, int64_t, double, std::string, Vector3, struct Instance*,
                             Color3, CFrame, Vector2, BrickColor, UDim, UDim2, Rect,
                             NumberRange, NumberSequence, ColorSequence, Content,
                             PhysicalProperties, Ray, Region3, DateTime, EnumItem>;

} // namespace rbx
