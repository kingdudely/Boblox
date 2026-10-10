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
            lua_pushinteger(L, i);
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
        void operator()(Instance* inst) const {
            if (inst)
                Instance::push(L, inst);
            else
                lua_pushnil(L);
        }
    };
    std::visit(Visitor{L}, v);
}

Variant check_variant(lua_State* L, int idx, PropType type) {
    switch (type) {
    case PropType::Bool:
        luaL_checktype(L, idx, LUA_TBOOLEAN);
        return Variant(lua_toboolean(L, idx) != 0);
    case PropType::Int: {
        luaL_checktype(L, idx, LUA_TNUMBER);
        return Variant(int64_t(lua_tointeger(L, idx)));
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
