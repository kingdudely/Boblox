// value_types.h — one declaration point for every Engine value type.
//
// Each datatype lives in its own instance/<name>.cpp (Vector3 pattern):
// struct ops + userdata metatable (__type drives typeof) + the global class
// table (constructors). This header declares the trio per type:
//   push_X    — push a value as userdata (metatable "X")
//   check_X   — read at idx; raises a Lua error on mismatch
//   create_X_class — install metatable + global (idempotent)
#pragma once

#include "instance/variant.h"

struct lua_State;

namespace rbx {

void create_color3_class(lua_State* L);
void push_color3(lua_State* L, const Color3& v);
Color3 check_color3(lua_State* L, int idx);

void create_cframe_class(lua_State* L);
void push_cframe(lua_State* L, const CFrame& v);
CFrame check_cframe(lua_State* L, int idx);

void create_vector2_class(lua_State* L);
void push_vector2(lua_State* L, const Vector2& v);
Vector2 check_vector2(lua_State* L, int idx);

void create_brickcolor_class(lua_State* L);
void push_brickcolor(lua_State* L, const BrickColor& v);
BrickColor check_brickcolor(lua_State* L, int idx);

void create_udim_class(lua_State* L);
void push_udim(lua_State* L, const UDim& v);
UDim check_udim(lua_State* L, int idx);

void create_udim2_class(lua_State* L);
void push_udim2(lua_State* L, const UDim2& v);
UDim2 check_udim2(lua_State* L, int idx);

void create_rect_class(lua_State* L);
void push_rect(lua_State* L, const Rect& v);
Rect check_rect(lua_State* L, int idx);

void create_numberrange_class(lua_State* L);
void push_numberrange(lua_State* L, const NumberRange& v);
NumberRange check_numberrange(lua_State* L, int idx);

void create_numbersequence_class(lua_State* L);
void push_numbersequence(lua_State* L, const NumberSequence& v);
NumberSequence check_numbersequence(lua_State* L, int idx);
void push_numbersequencekeypoint(lua_State* L, const NumberSequenceKeypoint& v);
NumberSequenceKeypoint check_numbersequencekeypoint(lua_State* L, int idx);

void create_colorsequence_class(lua_State* L);
void push_colorsequence(lua_State* L, const ColorSequence& v);
ColorSequence check_colorsequence(lua_State* L, int idx);
void push_colorsequencekeypoint(lua_State* L, const ColorSequenceKeypoint& v);
ColorSequenceKeypoint check_colorsequencekeypoint(lua_State* L, int idx);

void create_content_class(lua_State* L);
void push_content(lua_State* L, const Content& v);
Content check_content(lua_State* L, int idx);

void create_physicalproperties_class(lua_State* L);
void push_physicalproperties(lua_State* L, const PhysicalProperties& v);
PhysicalProperties check_physicalproperties(lua_State* L, int idx);

void create_ray_class(lua_State* L);
void push_ray(lua_State* L, const Ray& v);
Ray check_ray(lua_State* L, int idx);

void create_region3_class(lua_State* L);
void push_region3(lua_State* L, const Region3& v);
Region3 check_region3(lua_State* L, int idx);

void create_datetime_class(lua_State* L);
void push_datetime(lua_State* L, const DateTime& v);
DateTime check_datetime(lua_State* L, int idx);

// The `Enum` global (all 648 dump enums): typeof(Enum.X) == "Enum",
// Enum.X.Y are EnumItem userdata (typeof "EnumItem"). Enum definitions
// come from the generated table (enums.inc); this builds the runtime side.
void create_enum_class(lua_State* L);
void push_enumitem(lua_State* L, const EnumItem& v);
EnumItem check_enumitem(lua_State* L, int idx);

} // namespace rbx
