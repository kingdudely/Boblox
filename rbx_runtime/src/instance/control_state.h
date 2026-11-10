// control_state.h — the script-facing ControlState schema/value object.
//
// ControlState is a real Roblox API surface: a schema of named, typed fields
// whose values are snapshotted for replication. The native client uses its
// PlayerControlState for the PlayerModule's move vector, which is the payload
// behind the 141-byte 0xA6 frames. This module implements the object the
// controller can drive; the wire serializer can then read the same fields
// instead of guessing a movement value.
#pragma once

#include "instance/variant.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct lua_State;

namespace rbx {

class Instance;

enum class ControlStateFieldKind : uint8_t {
    Bool,
    CFrame,
    Instance,
    Int,
    Number,
    UnitVector3,
    Vector2,
    Vector3,
};

struct ControlStateField {
    std::string name;
    ControlStateFieldKind kind = ControlStateFieldKind::Bool;
    Variant default_value;
    double min_value = 0.0;
    double max_value = 0.0;
    double max_magnitude = 0.0;
};

struct ControlStateData {
    std::vector<ControlStateField> fields;
    std::map<std::string, Variant> current;
    std::map<std::string, Variant> previous;
    bool dirty = false;
};

// Allocate an empty state. Each ControlState/PlayerControlState instance owns
// one lazily; an ordinary Instance pays only the empty shared_ptr slot.
std::shared_ptr<ControlStateData> make_control_state();

// Attach the typed-field methods to ControlState/PlayerControlState and the
// Player:GetControlState accessor. Idempotent; called once by the built-in
// class registry after the generated method rows exist.
void install_control_state_methods();

} // namespace rbx
