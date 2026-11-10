// control_state.cpp — see control_state.h.
//
// The API follows the dump's ControlState class:
//   AddBoolField(name, default)
//   AddCFrameField(name, default)
//   AddInstanceField(name)
//   AddIntField(name, default, min, max)
//   AddNumberField(name, default, min, max)
//   AddUnitVector3Field(name, default)
//   AddVector2Field(name, default, maxMagnitude)
//   AddVector3Field(name, default, maxMagnitude)
//   GetState(ref), GetChangedState(prev, next), HasField(name)
//   SetField(name, value), UpdateFields(table), GetReplicationWeight()
//
// Values are kept in two snapshots. SetField updates `previous` to the value
// before that write; UpdateFields snapshots once and applies the whole table,
// so GetChangedState reports a batch rather than one field at a time.
#include "instance/control_state.h"

#include "instance/class_registry.h"
#include "instance/instance.h"
#include "instance/signal.h"
#include "instance/value_types.h"
#include "instance/vector3.h"

#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace rbx {
namespace {

ControlStateData* state_of(Instance* inst) {
    if (!inst->control_state())
        inst->set_control_state(make_control_state());
    return inst->control_state().get();
}

bool is_control_state(Instance* inst) {
    return inst->is_a("ControlState") || inst->is_a("PlayerControlState");
}

ControlStateField* find_field(ControlStateData* state, const std::string& name) {
    for (ControlStateField& f : state->fields) {
        if (f.name == name)
            return &f;
    }
    return nullptr;
}

const ControlStateField* find_field(const ControlStateData* state,
                                    const std::string& name) {
    for (const ControlStateField& f : state->fields) {
        if (f.name == name)
            return &f;
    }
    return nullptr;
}

bool same_value(const Variant& a, const Variant& b) {
    // Every Variant alternative has value equality (Instance* is identity).
    return a == b;
}

void clamp_magnitude(Vector3* v, double max_magnitude) {
    if (!v || !(max_magnitude > 0.0))
        return;
    const double m2 = v->x * v->x + v->y * v->y + v->z * v->z;
    if (m2 > max_magnitude * max_magnitude && m2 > 0.0) {
        const double s = max_magnitude / std::sqrt(m2);
        v->x *= s;
        v->y *= s;
        v->z *= s;
    }
}

void clamp_magnitude(Vector2* v, double max_magnitude) {
    if (!v || !(max_magnitude > 0.0))
        return;
    const double m2 = v->x * v->x + v->y * v->y;
    if (m2 > max_magnitude * max_magnitude && m2 > 0.0) {
        const double s = max_magnitude / std::sqrt(m2);
        v->x *= s;
        v->y *= s;
    }
}

Vector3 unit_or_zero(Vector3 v) {
    const double m2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (m2 <= 1e-24)
        return Vector3{};
    const double s = 1.0 / std::sqrt(m2);
    v.x *= s;
    v.y *= s;
    v.z *= s;
    return v;
}

// Read a Lua value into a Variant, enforcing the field's type and limits.
// Raises through luaL_error/check_* on a type mismatch.
Variant read_field_value(lua_State* L, int idx, const ControlStateField& f) {
    switch (f.kind) {
    case ControlStateFieldKind::Bool:
        luaL_checktype(L, idx, LUA_TBOOLEAN);
        return Variant(lua_toboolean(L, idx) != 0);
    case ControlStateFieldKind::CFrame:
        return Variant(check_cframe(L, idx));
    case ControlStateFieldKind::Instance:
        if (lua_isnil(L, idx))
            return Variant(static_cast<Instance*>(nullptr));
        return Variant(Instance::check(L, idx));
    case ControlStateFieldKind::Int: {
        luaL_checktype(L, idx, LUA_TNUMBER);
        double d = double(lua_tonumber(L, idx));
        if (d < f.min_value)
            d = f.min_value;
        if (d > f.max_value)
            d = f.max_value;
        return Variant(int64_t(std::llround(d)));
    }
    case ControlStateFieldKind::Number: {
        luaL_checktype(L, idx, LUA_TNUMBER);
        double d = double(lua_tonumber(L, idx));
        if (d < f.min_value)
            d = f.min_value;
        if (d > f.max_value)
            d = f.max_value;
        return Variant(d);
    }
    case ControlStateFieldKind::UnitVector3: {
        Vector3 v = check_vector3(L, idx);
        v = unit_or_zero(v);
        return Variant(v);
    }
    case ControlStateFieldKind::Vector2: {
        Vector2 v = check_vector2(L, idx);
        clamp_magnitude(&v, f.max_magnitude);
        return Variant(v);
    }
    case ControlStateFieldKind::Vector3: {
        Vector3 v = check_vector3(L, idx);
        clamp_magnitude(&v, f.max_magnitude);
        return Variant(v);
    }
    }
    return Variant();
}

void push_state_table(lua_State* L, const ControlStateData& state,
                      const std::map<std::string, Variant>& values) {
    lua_createtable(L, 0, int(state.fields.size()));
    for (const ControlStateField& f : state.fields) {
        const auto it = values.find(f.name);
        if (it == values.end())
            continue;
        lua_pushlstring(L, f.name.data(), f.name.size());
        push_variant(L, it->second);
        lua_settable(L, -3);
    }
}

const std::map<std::string, Variant>& state_frame(
    const ControlStateData& state, int frame) {
    // StateReferenceFrame: CurrentState=0, PreviousState=1, LastObservedState=2.
    // LastObserved is the most recent state the owner has seen, which for this
    // local schema is the current snapshot.
    return frame == 1 ? state.previous : state.current;
}

int frame_arg(lua_State* L, int idx, int fallback) {
    if (lua_isnoneornil(L, idx))
        return fallback;
    const EnumItem item = check_enumitem(L, idx);
    if (item.enum_name != "StateReferenceFrame")
        luaL_error(L, "expected Enum.StateReferenceFrame, got Enum.%s",
                   item.enum_name.c_str());
    return item.value;
}

int field_add_common(lua_State* L, ControlStateField field) {
    Instance* inst = Instance::check(L, 1);
    if (!is_control_state(inst))
        luaL_error(L, "%s is not a ControlState", inst->class_name().c_str());
    ControlStateData* state = state_of(inst);
    if (find_field(state, field.name))
        luaL_error(L, "ControlState field already exists: %s",
                   field.name.c_str());
    state->fields.push_back(field);
    state->current[field.name] = field.default_value;
    state->previous[field.name] = field.default_value;
    return 0;
}

std::string field_name(lua_State* L, int idx) {
    size_t n = 0;
    const char* s = luaL_checklstring(L, idx, &n);
    if (!s || n == 0)
        luaL_error(L, "ControlState field name must be a non-empty string");
    return std::string(s ? s : "", n);
}

int m_add_bool(lua_State* L) {
    ControlStateField f;
    f.name = field_name(L, 2);
    f.kind = ControlStateFieldKind::Bool;
    bool value = false;
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TBOOLEAN);
        value = lua_toboolean(L, 3) != 0;
    }
    f.default_value = Variant(value);
    return field_add_common(L, std::move(f));
}

int m_add_cframe(lua_State* L) {
    ControlStateField f;
    f.name = field_name(L, 2);
    f.kind = ControlStateFieldKind::CFrame;
    f.default_value = lua_isnoneornil(L, 3)
                          ? Variant(CFrame{})
                          : Variant(check_cframe(L, 3));
    return field_add_common(L, std::move(f));
}

int m_add_instance(lua_State* L) {
    ControlStateField f;
    f.name = field_name(L, 2);
    f.kind = ControlStateFieldKind::Instance;
    f.default_value = lua_isnoneornil(L, 3)
                          ? Variant(static_cast<Instance*>(nullptr))
                          : Variant(Instance::check(L, 3));
    return field_add_common(L, std::move(f));
}

int m_add_int(lua_State* L) {
    ControlStateField f;
    f.name = field_name(L, 2);
    f.kind = ControlStateFieldKind::Int;
    const double def = luaL_optnumber(L, 3, 0.0);
    const double min = luaL_optnumber(L, 4, 0.0);
    const double max = luaL_optnumber(L, 5, 255.0);
    if (min > max)
        luaL_error(L, "ControlState int field %s has min > max",
                   f.name.c_str());
    f.min_value = min;
    f.max_value = max;
    f.default_value = Variant(int64_t(std::llround(def)));
    return field_add_common(L, std::move(f));
}

int m_add_number(lua_State* L) {
    ControlStateField f;
    f.name = field_name(L, 2);
    f.kind = ControlStateFieldKind::Number;
    const double def = luaL_optnumber(L, 3, 0.0);
    const double min = luaL_optnumber(L, 4, 0.0);
    const double max = luaL_optnumber(L, 5, 1.0);
    if (min > max)
        luaL_error(L, "ControlState number field %s has min > max",
                   f.name.c_str());
    f.min_value = min;
    f.max_value = max;
    f.default_value = Variant(def);
    return field_add_common(L, std::move(f));
}

int m_add_unit_vector3(lua_State* L) {
    ControlStateField f;
    f.name = field_name(L, 2);
    f.kind = ControlStateFieldKind::UnitVector3;
    Vector3 v{};
    if (!lua_isnoneornil(L, 3))
        v = unit_or_zero(check_vector3(L, 3));
    f.default_value = Variant(v);
    return field_add_common(L, std::move(f));
}

int m_add_vector2(lua_State* L) {
    ControlStateField f;
    f.name = field_name(L, 2);
    f.kind = ControlStateFieldKind::Vector2;
    Vector2 v{};
    if (!lua_isnoneornil(L, 3))
        v = check_vector2(L, 3);
    f.max_magnitude = luaL_optnumber(L, 4, 1.0);
    clamp_magnitude(&v, f.max_magnitude);
    f.default_value = Variant(v);
    return field_add_common(L, std::move(f));
}

int m_add_vector3(lua_State* L) {
    ControlStateField f;
    f.name = field_name(L, 2);
    f.kind = ControlStateFieldKind::Vector3;
    Vector3 v{};
    if (!lua_isnoneornil(L, 3))
        v = check_vector3(L, 3);
    f.max_magnitude = luaL_optnumber(L, 4, 100.0);
    clamp_magnitude(&v, f.max_magnitude);
    f.default_value = Variant(v);
    return field_add_common(L, std::move(f));
}

int m_has_field(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    if (!is_control_state(inst))
        luaL_error(L, "%s is not a ControlState", inst->class_name().c_str());
    const std::string name = field_name(L, 2);
    ControlStateData* state = state_of(inst);
    lua_pushboolean(L, find_field(state, name) != nullptr);
    return 1;
}

int m_get_state(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    if (!is_control_state(inst))
        luaL_error(L, "%s is not a ControlState", inst->class_name().c_str());
    ControlStateData* state = state_of(inst);
    const int frame = frame_arg(L, 2, 0);
    push_state_table(L, *state, state_frame(*state, frame));
    return 1;
}

int m_get_changed_state(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    if (!is_control_state(inst))
        luaL_error(L, "%s is not a ControlState", inst->class_name().c_str());
    ControlStateData* state = state_of(inst);
    const int prev = frame_arg(L, 2, 1); // PreviousState
    const int next = frame_arg(L, 3, 0); // CurrentState
    const auto& before = state_frame(*state, prev);
    const auto& after = state_frame(*state, next);
    lua_createtable(L, 0, int(state->fields.size()));
    for (const ControlStateField& f : state->fields) {
        const auto a = before.find(f.name);
        const auto b = after.find(f.name);
        if (a == before.end() || b == after.end())
            continue;
        if (same_value(a->second, b->second))
            continue;
        lua_pushlstring(L, f.name.data(), f.name.size());
        push_variant(L, b->second);
        lua_settable(L, -3);
    }
    return 1;
}

void fire_state_changed(lua_State* L, Instance* inst) {
    if (find_event(inst->class_info(), "OnStateChanged"))
        inst->event_signal("OnStateChanged")->fire(L);
}

int m_set_field(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    if (!is_control_state(inst))
        luaL_error(L, "%s is not a ControlState", inst->class_name().c_str());
    const std::string name = field_name(L, 2);
    ControlStateData* state = state_of(inst);
    ControlStateField* f = find_field(state, name);
    if (!f)
        luaL_error(L, "ControlState has no field named %s", name.c_str());
    Variant value = read_field_value(L, 3, *f);
    state->previous = state->current;
    const bool changed = !same_value(state->current[name], value);
    state->current[name] = value;
    if (changed) {
        state->dirty = true;
        fire_state_changed(L, inst);
    }
    return 0;
}

int m_update_fields(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    if (!is_control_state(inst))
        luaL_error(L, "%s is not a ControlState", inst->class_name().c_str());
    luaL_checktype(L, 2, LUA_TTABLE);
    ControlStateData* state = state_of(inst);

    // Validate and decode every entry before touching the snapshots, so a bad
    // value leaves the previous/current pair untouched.
    std::vector<std::pair<std::string, Variant>> updates;
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
        if (!lua_isstring(L, -2))
            luaL_error(L, "ControlState:UpdateFields keys must be strings");
        size_t n = 0;
        const char* key = lua_tolstring(L, -2, &n);
        const std::string name(key ? key : "", n);
        const ControlStateField* f = find_field(state, name);
        if (!f)
            luaL_error(L, "ControlState has no field named %s", name.c_str());
        updates.emplace_back(name, read_field_value(L, -1, *f));
        lua_pop(L, 1);
    }

    state->previous = state->current;
    bool changed = false;
    for (const auto& [name, value] : updates) {
        if (!same_value(state->current[name], value)) {
            changed = true;
            state->current[name] = value;
        }
    }
    if (changed) {
        state->dirty = true;
        fire_state_changed(L, inst);
    }
    return 0;
}

int m_get_replication_weight(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    if (!is_control_state(inst))
        luaL_error(L, "%s is not a ControlState", inst->class_name().c_str());
    ControlStateData* state = state_of(inst);
    // A dirty schema needs a slot in the next input frame; a clean one costs
    // nothing. This is deliberately conservative until the native weight
    // function is recovered.
    lua_pushnumber(L, state->dirty ? 1.0 : 0.0);
    return 1;
}

int m_player_get_control_state(lua_State* L) {
    Instance* player = Instance::check(L, 1);
    if (!player->is_a("Player"))
        luaL_error(L, "GetControlState is only available on a Player");
    for (const auto& child : player->children()) {
        if (child->is_a("PlayerControlState")) {
            Instance::push(L, child.get());
            return 1;
        }
    }
    const ClassInfo* cls = find_class("PlayerControlState");
    if (!cls)
        luaL_error(L, "PlayerControlState class is not registered");
    Ref<Instance> state = instantiate(cls);
    state->set_name("PlayerControlState");
    std::string err;
    state->set_prop("Owner", Variant(player), &err);
    if (!state->reparent(player, &err))
        luaL_error(L, "could not attach PlayerControlState: %s", err.c_str());
    Instance::push(L, state.get());
    return 1;
}

} // namespace

std::shared_ptr<ControlStateData> make_control_state() {
    return std::make_shared<ControlStateData>();
}

void install_control_state_methods() {
    // PlayerControlState is the player-owned face of the same schema. Its
    // generated row omits Owner, so add it here rather than editing the
    // generated dump table.
    attach_property("PlayerControlState",
                    PropInfo{"Owner", PropType::Instance,
                             static_cast<Instance*>(nullptr), false});

    for (const char* cls : {"ControlState", "PlayerControlState"}) {
        attach_method(cls, "AddBoolField", m_add_bool);
        attach_method(cls, "AddCFrameField", m_add_cframe);
        attach_method(cls, "AddInstanceField", m_add_instance);
        attach_method(cls, "AddIntField", m_add_int);
        attach_method(cls, "AddNumberField", m_add_number);
        attach_method(cls, "AddUnitVector3Field", m_add_unit_vector3);
        attach_method(cls, "AddVector2Field", m_add_vector2);
        attach_method(cls, "AddVector3Field", m_add_vector3);
        attach_method(cls, "GetChangedState", m_get_changed_state);
        attach_method(cls, "GetReplicationWeight", m_get_replication_weight);
        attach_method(cls, "GetState", m_get_state);
        attach_method(cls, "HasField", m_has_field);
        attach_method(cls, "SetField", m_set_field);
        attach_method(cls, "UpdateFields", m_update_fields);
    }
    // Not present in the public dump for this build, but it is the only way a
    // local script can reach the non-creatable PlayerControlState. The native
    // PlayerModule has the same relationship through its own hidden plumbing.
    attach_method("Player", "GetControlState", m_player_get_control_state);
}

} // namespace rbx
