// font.cpp — the Font value type (Engine profile, --gui only).
//
// Roblox's Font API is deliberately value-like: Font.new/fromEnum/fromName/
// fromId produce an immutable font value carrying a family and the two face
// axes. TextLabel/TextButton/TextBox.FontFace is typed Font in the pinned API
// dump, so this one value is what scripts assign to the property.
//
// The family string is stored verbatim and no asset file is opened: a headless
// client can execute a GUI script without carrying the content tree. A
// renderer that needs glyphs resolves the family through its own content root.
#include "instance/value_types.h"

#include "instance/teardown.h"

#include "lua.h"
#include "lualib.h"

#include <cstring>
#include <string>

namespace rbx {

namespace {

int opt_enum_value(lua_State* L, int idx, const char* enum_name, int fallback) {
    if (lua_isnoneornil(L, idx))
        return fallback;
    const EnumItem item = check_enumitem(L, idx);
    if (item.enum_name != enum_name) {
        luaL_error(L, "expected Enum.%s, got Enum.%s", enum_name,
                   item.enum_name.c_str());
        return fallback;
    }
    return item.value;
}

std::string enum_family(const EnumItem& item) {
    std::string name = enum_item_name(item);
    if (!name.empty())
        return name;
    return "Font" + std::to_string(item.value);
}

int font_index(lua_State* L) {
    const Font v = check_font(L, 1);
    const char* k = luaL_checkstring(L, 2);
    if (std::strcmp(k, "Family") == 0 || std::strcmp(k, "family") == 0) {
        lua_pushlstring(L, v.family.data(), v.family.size());
        return 1;
    }
    if (std::strcmp(k, "Weight") == 0 || std::strcmp(k, "weight") == 0) {
        push_enumitem(L, EnumItem{"FontWeight", v.weight});
        return 1;
    }
    if (std::strcmp(k, "Style") == 0 || std::strcmp(k, "style") == 0) {
        push_enumitem(L, EnumItem{"FontStyle", v.style});
        return 1;
    }
    lua_getmetatable(L, 1); // Methods table lookup
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

int font_new(lua_State* L) {
    Font v;
    if (lua_isstring(L, 1)) {
        size_t len = 0;
        const char* s = lua_tolstring(L, 1, &len);
        v.family.assign(s, len);
    } else if (lua_isuserdata(L, 1)) {
        const EnumItem item = check_enumitem(L, 1);
        if (item.enum_name != "Font") {
            luaL_error(L, "Font.new expects a string or Enum.Font, got Enum.%s",
                       item.enum_name.c_str());
            return 0;
        }
        v.family = enum_family(item);
    } else {
        luaL_error(L, "Font.new expects a string or Enum.Font");
        return 0;
    }
    v.weight = opt_enum_value(L, 2, "FontWeight", 400);
    v.style = opt_enum_value(L, 3, "FontStyle", 0);
    push_font(L, v);
    return 1;
}

int font_fromenum(lua_State* L) {
    const EnumItem item = check_enumitem(L, 1);
    if (item.enum_name != "Font") {
        luaL_error(L, "Font.fromEnum expects Enum.Font, got Enum.%s",
                   item.enum_name.c_str());
        return 0;
    }
    Font v;
    v.family = enum_family(item);
    v.weight = opt_enum_value(L, 2, "FontWeight", 400);
    v.style = opt_enum_value(L, 3, "FontStyle", 0);
    push_font(L, v);
    return 1;
}

int font_fromname(lua_State* L) {
    Font v;
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    v.family.assign(s, len);
    v.weight = opt_enum_value(L, 2, "FontWeight", 400);
    v.style = opt_enum_value(L, 3, "FontStyle", 0);
    push_font(L, v);
    return 1;
}

int font_fromid(lua_State* L) {
    Font v;
    v.family = "rbxassetid://" + std::to_string((long long)luaL_checknumber(L, 1));
    v.weight = opt_enum_value(L, 2, "FontWeight", 400);
    v.style = opt_enum_value(L, 3, "FontStyle", 0);
    push_font(L, v);
    return 1;
}

int font_eq(lua_State* L) {
    lua_pushboolean(L, check_font(L, 1) == check_font(L, 2) ? 1 : 0);
    return 1;
}

int font_tostring(lua_State* L) {
    const Font v = check_font(L, 1);
    lua_pushlstring(L, v.family.data(), v.family.size());
    return 1;
}

} // namespace

void push_font(lua_State* L, const Font& v) {
    Slab* s = slab(L);
    const auto idx = (uint32_t)s->fonts.size();
    s->fonts.push_back(std::make_unique<Font>(v));
    void* p = lua_newuserdata(L, sizeof(idx));
    *static_cast<uint32_t*>(p) = idx;
    if (luaL_newmetatable(L, "Font")) {
        lua_newtable(L);
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, font_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, font_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushcfunction(L, font_tostring, "__tostring");
        lua_setfield(L, -2, "__tostring");
        lua_pushstring(L, "Font"); // typeof(Font.new(...)) == "Font"
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

Font check_font(lua_State* L, int idx) {
    const uint32_t h =
        *static_cast<const uint32_t*>(luaL_checkudata(L, idx, "Font"));
    Slab* s = slab(L);
    if (h >= s->fonts.size() || !s->fonts[h])
        luaL_error(L, "Font handle out of range");
    return *s->fonts[h];
}

void create_font_class(lua_State* L) {
    push_font(L, Font{});
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, font_new, "new");
    lua_setfield(L, -2, "new");
    lua_pushcfunction(L, font_fromenum, "fromEnum");
    lua_setfield(L, -2, "fromEnum");
    lua_pushcfunction(L, font_fromname, "fromName");
    lua_setfield(L, -2, "fromName");
    lua_pushcfunction(L, font_fromid, "fromId");
    lua_setfield(L, -2, "fromId");
    lua_setglobal(L, "Font");
}

} // namespace rbx
