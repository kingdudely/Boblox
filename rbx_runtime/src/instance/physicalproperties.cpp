// physicalproperties.cpp — PhysicalProperties value type (see value_types.h).
//
// Docs-verified surface: new(density, friction, elasticity[, frictionWeight,
// elasticityWeight[, acousticAbsorption]]) + the 6 properties.
// (The material-enum constructor waits for the Enum kind.)
#include "instance/value_types.h"

#include "lua.h"
#include "lualib.h"

#include <cstring>

namespace rbx {

namespace {

int pp_index(lua_State* L) {
    const PhysicalProperties* v =
        static_cast<const PhysicalProperties*>(luaL_checkudata(L, 1, "PhysicalProperties"));
    const char* k = luaL_checkstring(L, 2);
    if (strcmp(k, "Density") == 0 || strcmp(k, "density") == 0) {
        lua_pushnumber(L, v->density);
        return 1;
    }
    if (strcmp(k, "Friction") == 0 || strcmp(k, "friction") == 0) {
        lua_pushnumber(L, v->friction);
        return 1;
    }
    if (strcmp(k, "Elasticity") == 0 || strcmp(k, "elasticity") == 0) {
        lua_pushnumber(L, v->elasticity);
        return 1;
    }
    if (strcmp(k, "FrictionWeight") == 0 || strcmp(k, "frictionWeight") == 0) {
        lua_pushnumber(L, v->friction_weight);
        return 1;
    }
    if (strcmp(k, "ElasticityWeight") == 0 || strcmp(k, "elasticityWeight") == 0) {
        lua_pushnumber(L, v->elasticity_weight);
        return 1;
    }
    if (strcmp(k, "AcousticAbsorption") == 0 || strcmp(k, "acousticAbsorption") == 0) {
        lua_pushnumber(L, v->acoustic_absorption);
        return 1;
    }
    lua_getmetatable(L, 1);
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

int pp_new(lua_State* L) {
    PhysicalProperties v;
    v.density = luaL_checknumber(L, 1);
    v.friction = luaL_checknumber(L, 2);
    v.elasticity = luaL_checknumber(L, 3);
    v.friction_weight = luaL_optnumber(L, 4, 0.0);
    v.elasticity_weight = luaL_optnumber(L, 5, 0.0);
    v.acoustic_absorption = luaL_optnumber(L, 6, 0.0);
    push_physicalproperties(L, v);
    return 1;
}

int pp_eq(lua_State* L) {
    const PhysicalProperties a = check_physicalproperties(L, 1);
    const PhysicalProperties b = check_physicalproperties(L, 2);
    lua_pushboolean(L, a == b ? 1 : 0);
    return 1;
}

} // namespace

void push_physicalproperties(lua_State* L, const PhysicalProperties& v) {
    void* p = lua_newuserdata(L, sizeof(PhysicalProperties));
    *static_cast<PhysicalProperties*>(p) = v;
    if (luaL_newmetatable(L, "PhysicalProperties")) { // first time: fill it
        lua_newtable(L);
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, pp_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, pp_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushstring(L, "PhysicalProperties"); // typeof() parity
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

PhysicalProperties check_physicalproperties(lua_State* L, int idx) {
    return *static_cast<const PhysicalProperties*>(
        luaL_checkudata(L, idx, "PhysicalProperties"));
}

void create_physicalproperties_class(lua_State* L) {
    push_physicalproperties(L, PhysicalProperties{});
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, pp_new, "new");
    lua_setfield(L, -2, "new");
    lua_setglobal(L, "PhysicalProperties");
}

} // namespace rbx
