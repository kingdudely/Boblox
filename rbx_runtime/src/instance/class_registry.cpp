// class_registry.cpp — class metadata storage + Lua property marshalling.
#include "instance/class_registry.h"

#include "instance/instance.h"
#include "instance/signal.h"   // push_variant(Instance*) needs the instance metatable
#include "instance/vector3.h"
#include "instance/value_types.h"

#include "lua.h"
#include "lualib.h"

#include <map>
#include <mutex>
#include <utility>

namespace rbx {

namespace {

std::map<std::string, ClassInfo>& classes() {
    static std::map<std::string, ClassInfo> c;
    return c;
}

} // namespace

void register_class(ClassInfo info) {
    info.super = nullptr; // always linked by name afterwards (see header)
    classes()[info.name] = std::move(info);
}

Ref<Instance> generic_factory(const ClassInfo* cls) {
    return Ref<Instance>(new Instance(cls));
}

Ref<Instance> instantiate(const ClassInfo* cls) {
    return cls->factory ? cls->factory(cls) : generic_factory(cls);
}

bool link_super(const std::string& child, const std::string& parent) {
    auto it = classes().find(child);
    const ClassInfo* p = find_class(parent);
    if (it == classes().end() || !p)
        return false;
    it->second.super = p;
    return true;
}

const ClassInfo* find_class(const std::string& name) {
    auto it = classes().find(name);
    return it == classes().end() ? nullptr : &it->second;
}

namespace {

template <typename T>
const T* find_member(const ClassInfo* cls, const std::string& name,
                     const std::vector<T> ClassInfo::*field) {
    for (const ClassInfo* c = cls; c; c = c->super) { // derived -> base
        for (const T& m : c->*field) {
            if (m.name == name)
                return &m;
        }
    }
    return nullptr;
}

} // namespace

const MethodInfo* find_method(const ClassInfo* cls, const std::string& name) {
    return cls ? find_member(cls, name, &ClassInfo::methods) : nullptr;
}

const EventInfo* find_event(const ClassInfo* cls, const std::string& name) {
    return cls ? find_member(cls, name, &ClassInfo::events) : nullptr;
}

void attach_method(const char* cls, const char* method, int (*fn)(lua_State*)) {
    auto it = classes().find(cls ? cls : "");
    if (it == classes().end() || !fn)
        return;
    for (MethodInfo& m : it->second.methods) {
        if (m.name == method) {
            m.fn = fn;
            return;
        }
    }
    it->second.methods.push_back(MethodInfo{method ? method : "", fn});
}

void ensure_method(const char* cls, const char* method) {
    auto it = classes().find(cls ? cls : "");
    if (it == classes().end())
        return;
    for (const MethodInfo& m : it->second.methods)
        if (m.name == method)
            return;
    it->second.methods.push_back(MethodInfo{method ? method : "", nullptr});
}

void ensure_event(const char* cls, const char* event) {
    auto it = classes().find(cls ? cls : "");
    if (it == classes().end())
        return;
    for (const EventInfo& e : it->second.events)
        if (e.name == event)
            return;
    it->second.events.push_back(EventInfo{event ? event : ""});
}

void ensure_property(const char* cls, PropInfo prop) {
    auto it = classes().find(cls ? cls : "");
    if (it == classes().end() || find_property(&it->second, prop.name))
        return;
    it->second.props.push_back(std::move(prop));
}

void attach_property(const char* cls, PropInfo prop) {
    auto it = classes().find(cls ? cls : "");
    if (it == classes().end())
        return;
    for (PropInfo& p : it->second.props) {
        if (p.name == prop.name) {
            p = std::move(prop);
            return;
        }
    }
    it->second.props.push_back(std::move(prop));
}

bool class_isa(const ClassInfo* derived, const std::string& base) {
    for (const ClassInfo* c = derived; c; c = c->super) {
        if (c->name == base)
            return true;
    }
    return false;
}

const PropInfo* find_property(const ClassInfo* cls, const std::string& name) {
    for (const ClassInfo* c = cls; c; c = c->super) { // derived -> base
        for (const PropInfo& p : c->props) {
            if (p.name == name)
                return &p;
        }
    }
    return nullptr;
}

std::vector<PropInfo> effective_properties(const ClassInfo* cls) {
    std::vector<const ClassInfo*> chain;
    for (const ClassInfo* c = cls; c; c = c->super)
        chain.push_back(c); // derived -> base
    std::vector<PropInfo> out;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) // base -> derived
        out.insert(out.end(), (*it)->props.begin(), (*it)->props.end());
    return out;
}

// ---- Lua marshalling --------------------------------------------------------

void push_variant(lua_State* L, const Variant& v) {
    struct Visitor {
        lua_State* L;
        void operator()(bool b) const {
            lua_pushboolean(L, b ? 1 : 0);
        }
        void operator()(int64_t i) const {
            // This Luau build's integer64 TValue tag is not consumed by the
            // ordinary number APIs (lua_tonumber/tostring read it as 0).  Push
            // a double instead: Roblox integer properties are numeric to
            // scripts, and every real UserId stays exact inside 2^53.
            lua_pushnumber(L, double(i));
        }
        void operator()(double d) const {
            lua_pushnumber(L, d);
        }
        void operator()(const std::string& s) const {
            lua_pushlstring(L, s.data(), s.size());
        }
        void operator()(const Vector3& v3) const {
            push_vector3(L, v3);
        }
        void operator()(const Color3& v) const {
            push_color3(L, v);
        }
        void operator()(const CFrame& v) const {
            push_cframe(L, v);
        }
        void operator()(const Vector2& v) const {
            push_vector2(L, v);
        }
        void operator()(const BrickColor& v) const {
            push_brickcolor(L, v);
        }
        void operator()(const UDim& v) const {
            push_udim(L, v);
        }
        void operator()(const UDim2& v) const {
            push_udim2(L, v);
        }
        void operator()(const Rect& v) const {
            push_rect(L, v);
        }
        void operator()(const NumberRange& v) const {
            push_numberrange(L, v);
        }
        void operator()(const NumberSequence& v) const {
            push_numbersequence(L, v);
        }
        void operator()(const ColorSequence& v) const {
            push_colorsequence(L, v);
        }
        void operator()(const Content& v) const {
            push_content(L, v);
        }
        void operator()(const PhysicalProperties& v) const {
            push_physicalproperties(L, v);
        }
        void operator()(const Ray& v) const {
            push_ray(L, v);
        }
        void operator()(const Region3& v) const {
            push_region3(L, v);
        }
        void operator()(const DateTime& v) const {
            push_datetime(L, v);
        }
        void operator()(const EnumItem& v) const {
            push_enumitem(L, v);
        }
        void operator()(const Font& v) const {
            push_font(L, v);
        }
        void operator()(const TweenInfo& v) const {
            push_tweeninfo(L, v);
        }
        void operator()(const Faces& v) const {
            lua_createtable(L, 0, 6);
            auto put = [&](const char* k, bool b) {
                lua_pushboolean(L, b ? 1 : 0);
                lua_setfield(L, -2, k);
            };
            put("Top", v.top); put("Bottom", v.bottom);
            put("Left", v.left); put("Right", v.right);
            put("Front", v.front); put("Back", v.back);
        }
        void operator()(const Axes& v) const {
            lua_createtable(L, 0, 3);
            auto put = [&](const char* k, bool b) {
                lua_pushboolean(L, b ? 1 : 0);
                lua_setfield(L, -2, k);
            };
            put("X", v.x); put("Y", v.y); put("Z", v.z);
        }
        void operator()(Instance* inst) const {
            if (inst)
                Instance::push(L, inst);
            else
                lua_pushnil(L);
        }
    };
    std::visit(Visitor{L}, v);
}

Variant check_variant(lua_State* L, int idx, PropType type, const std::string& enum_type) {
    switch (type) {
    case PropType::Bool:
        luaL_checktype(L, idx, LUA_TBOOLEAN);
        return Variant(lua_toboolean(L, idx) != 0);
    case PropType::Int: {
        luaL_checktype(L, idx, LUA_TNUMBER);
        // Convert the ordinary Lua number; lua_tointeger64 only accepts an
        // integer-tagged TValue and returns 0 for every script-supplied 7.
        return Variant(int64_t(lua_tonumber(L, idx)));
    }
    case PropType::Double:
        luaL_checktype(L, idx, LUA_TNUMBER);
        return Variant(double(lua_tonumber(L, idx)));
    case PropType::String: {
        luaL_checktype(L, idx, LUA_TSTRING);
        size_t len = 0;
        const char* s = lua_tolstring(L, idx, &len);
        return Variant(std::string(s, len));
    }
    case PropType::Vector3: {
        const void* p = luaL_checkudata(L, idx, "Vector3");
        return Variant(*static_cast<const Vector3*>(p));
    }
    case PropType::Color3:
        return Variant(check_color3(L, idx));
    case PropType::CFrame:
        return Variant(check_cframe(L, idx));
    case PropType::Vector2:
        return Variant(check_vector2(L, idx));
    case PropType::BrickColor:
        return Variant(check_brickcolor(L, idx));
    case PropType::UDim:
        return Variant(check_udim(L, idx));
    case PropType::UDim2:
        return Variant(check_udim2(L, idx));
    case PropType::Rect:
        return Variant(check_rect(L, idx));
    case PropType::NumberRange:
        return Variant(check_numberrange(L, idx));
    case PropType::NumberSequence:
        return Variant(check_numbersequence(L, idx));
    case PropType::ColorSequence:
        return Variant(check_colorsequence(L, idx));
    case PropType::Content:
        return Variant(check_content(L, idx));
    case PropType::PhysicalProperties:
        return Variant(check_physicalproperties(L, idx));
    case PropType::Ray:
        return Variant(check_ray(L, idx));
    case PropType::Region3:
        return Variant(check_region3(L, idx));
    case PropType::DateTime:
        return Variant(check_datetime(L, idx));
    case PropType::Enum: {
        const EnumItem item = check_enumitem(L, idx); // validates userdata
        if (!enum_type.empty() && item.enum_name != enum_type) {
            luaL_error(L, "wrong enum type (expected %s)", enum_type.c_str());
            return Variant(EnumItem{});
        }
        return Variant(item);
    }
    case PropType::Font:
        return Variant(check_font(L, idx));
    case PropType::TweenInfo:
        return Variant(check_tweeninfo(L, idx));
    case PropType::Faces: {
        luaL_checktype(L, idx, LUA_TTABLE);
        auto get = [&](const char* k) {
            lua_getfield(L, idx, k);
            const bool b = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            return b;
        };
        Faces f;
        f.top = get("Top"); f.bottom = get("Bottom");
        f.left = get("Left"); f.right = get("Right");
        f.front = get("Front"); f.back = get("Back");
        return Variant(f);
    }
    case PropType::Axes: {
        luaL_checktype(L, idx, LUA_TTABLE);
        auto get = [&](const char* k) {
            lua_getfield(L, idx, k);
            const bool b = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            return b;
        };
        Axes a;
        a.x = get("X"); a.y = get("Y"); a.z = get("Z");
        return Variant(a);
    }
    case PropType::Instance: {
        if (lua_isnil(L, idx))
            return Variant(static_cast<Instance*>(nullptr));
        return Variant(Instance::check(L, idx));
    }
    }
    luaL_error(L, "unhandled property type");
    return Variant(); // not reached
}

} // namespace rbx
