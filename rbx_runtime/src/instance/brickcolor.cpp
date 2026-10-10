// brickcolor.cpp — BrickColor value type (see value_types.h).
//
// Palette facts (number/RGB/name) come from the 2016 reference tree
// (BrickColor::insert calls); the modern palette is stable legacy data.
// Docs-verified surface: new(number), new(r, g, b), new(name),
// new(color3 -> closest), palette/random semantics per docs; closest uses
// the L1 RGB metric like the reference implementation.
#include "instance/value_types.h"

#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace rbx {

namespace {

struct BrickEntry {
    int number;
    const char* name;
    uint8_t r, g, b;
};

#include "instance/brickcolor_palette.inc"

const BrickEntry* entry_by_number(int n) {
    for (size_t i = 0; i < kBrickPaletteSize; i++) {
        if (kBrickPalette[i].number == n)
            return &kBrickPalette[i];
    }
    return nullptr;
}

const BrickEntry* entry_by_name(const char* name) {
    for (size_t i = 0; i < kBrickPaletteSize; i++) {
        if (std::strcmp(kBrickPalette[i].name, name) == 0)
            return &kBrickPalette[i];
    }
    return nullptr;
}

// Closest palette color by L1 RGB distance (reference metric); exact match
// short-circuits. Ties resolve to the lowest palette number (stable order).
const BrickEntry* closest(uint8_t r, uint8_t g, uint8_t b) {
    const BrickEntry* best = &kBrickPalette[0];
    int best_d = 3 * 255 + 1;
    for (size_t i = 0; i < kBrickPaletteSize; i++) {
        const BrickEntry& e = kBrickPalette[i];
        const int d = std::abs((int)e.r - r) + std::abs((int)e.g - g) +
                      std::abs((int)e.b - b);
        if (d < best_d) {
            best_d = d;
            best = &e;
            if (d == 0)
                break;
        }
    }
    return best;
}

int named_number(const char* name, const char* fallback) {
    const BrickEntry* e = entry_by_name(name);
    if (!e)
        e = entry_by_name(fallback);
    return e ? e->number : 1;
}

int bc_index(lua_State* L) {
    const BrickColor* v = static_cast<const BrickColor*>(luaL_checkudata(L, 1, "BrickColor"));
    const BrickEntry* e = entry_by_number(v->number);
    const char* k = luaL_checkstring(L, 2);
    if (strcmp(k, "Number") == 0 || strcmp(k, "number") == 0) {
        lua_pushnumber(L, v->number);
        return 1;
    }
    if (strcmp(k, "Name") == 0 || strcmp(k, "name") == 0) {
        lua_pushstring(L, e ? e->name : "Unknown");
        return 1;
    }
    if (strcmp(k, "r") == 0) {
        lua_pushnumber(L, e ? e->r : 0);
        return 1;
    }
    if (strcmp(k, "g") == 0) {
        lua_pushnumber(L, e ? e->g : 0);
        return 1;
    }
    if (strcmp(k, "b") == 0) {
        lua_pushnumber(L, e ? e->b : 0);
        return 1;
    }
    if (strcmp(k, "Color") == 0 || strcmp(k, "color") == 0) {
        Color3 c;
        if (e) {
            c.r = e->r / 255.0;
            c.g = e->g / 255.0;
            c.b = e->b / 255.0;
        }
        push_color3(L, c);
        return 1;
    }
    lua_getmetatable(L, 1);
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

// new(number) | new(r, g, b) | new(name) | new(color3)
int bc_new(lua_State* L) {
    BrickColor v;
    if (lua_type(L, 1) == LUA_TSTRING) {
        const BrickEntry* e = entry_by_name(lua_tostring(L, 1));
        if (!e) {
            luaL_error(L, "invalid BrickColor name");
            return 0;
        }
        v.number = e->number;
    } else if (lua_gettop(L) >= 3) {
        const int r = (int)luaL_checknumber(L, 1);
        const int g = (int)luaL_checknumber(L, 2);
        const int b = (int)luaL_checknumber(L, 3);
        v.number = closest((uint8_t)r, (uint8_t)g, (uint8_t)b)->number;
    } else if (lua_type(L, 1) == LUA_TNUMBER) {
        const int n = (int)luaL_checknumber(L, 1);
        if (!entry_by_number(n)) {
            luaL_error(L, "invalid BrickColor number");
            return 0;
        }
        v.number = n;
    } else {
        const Color3 c = check_color3(L, 1);
        v.number = closest((uint8_t)(c.r * 255.0), (uint8_t)(c.g * 255.0),
                           (uint8_t)(c.b * 255.0))
                       ->number;
    }
    push_brickcolor(L, v);
    return 1;
}

int bc_random(lua_State* L) {
    (void)L;
    BrickColor v;
    v.number = kBrickPaletteOrder[(size_t)std::rand() % kBrickPaletteOrderSize];
    push_brickcolor(L, v);
    return 1;
}

int bc_named(lua_State* L, const char* name) {
    BrickColor v;
    v.number = named_number(name, "White");
    push_brickcolor(L, v);
    return 1;
}

int bc_white(lua_State* L) { return bc_named(L, "White"); }
int bc_black(lua_State* L) { return bc_named(L, "Black"); }
int bc_red(lua_State* L) { return bc_named(L, "Bright red"); }
int bc_yellow(lua_State* L) { return bc_named(L, "Bright yellow"); }
int bc_green(lua_State* L) { return bc_named(L, "Bright green"); }
int bc_blue(lua_State* L) { return bc_named(L, "Bright blue"); }

int bc_tostring(lua_State* L) {
    const BrickColor* v = static_cast<const BrickColor*>(luaL_checkudata(L, 1, "BrickColor"));
    const BrickEntry* e = entry_by_number(v->number);
    lua_pushstring(L, e ? e->name : "Unknown");
    return 1;
}

int bc_eq(lua_State* L) {
    const BrickColor a = check_brickcolor(L, 1);
    const BrickColor b = check_brickcolor(L, 2);
    lua_pushboolean(L, a == b ? 1 : 0);
    return 1;
}

} // namespace

void push_brickcolor(lua_State* L, const BrickColor& v) {
    void* p = lua_newuserdata(L, sizeof(BrickColor));
    *static_cast<BrickColor*>(p) = v;
    if (luaL_newmetatable(L, "BrickColor")) { // first time: fill it
        lua_newtable(L);                     // Methods (empty: lookup fallback)
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, bc_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, bc_tostring, "__tostring");
        lua_setfield(L, -2, "__tostring");
        lua_pushcfunction(L, bc_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushstring(L, "BrickColor"); // typeof() == "BrickColor" (Roblox parity)
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

BrickColor check_brickcolor(lua_State* L, int idx) {
    return *static_cast<const BrickColor*>(luaL_checkudata(L, idx, "BrickColor"));
}

void create_brickcolor_class(lua_State* L) {
    push_brickcolor(L, BrickColor{});
    lua_pop(L, 1);

    // Certain mappings only (Gray/DarkGray/palette(n) skipped: unverified).
    lua_newtable(L);
    lua_pushcfunction(L, bc_new, "new");
    lua_setfield(L, -2, "new");
    lua_pushcfunction(L, bc_random, "random");
    lua_setfield(L, -2, "random");
    lua_pushcfunction(L, bc_white, "White");
    lua_setfield(L, -2, "White");
    lua_pushcfunction(L, bc_black, "Black");
    lua_setfield(L, -2, "Black");
    lua_pushcfunction(L, bc_red, "Red");
    lua_setfield(L, -2, "Red");
    lua_pushcfunction(L, bc_yellow, "Yellow");
    lua_setfield(L, -2, "Yellow");
    lua_pushcfunction(L, bc_green, "Green");
    lua_setfield(L, -2, "Green");
    lua_pushcfunction(L, bc_blue, "Blue");
    lua_setfield(L, -2, "Blue");
    lua_setglobal(L, "BrickColor");
}

} // namespace rbx
