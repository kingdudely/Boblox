// remote.cpp — RemoteEvent client->server encoder (see remote.h).
//
// Every layout here is pinned against a live native TX capture (Rum 2.739,
// 2026-11-05, place 1818) and the native decoder in libroblox.so. The
// self-describing argument tags are the same numbering as snapshot values:
//
//   0x00 nil                 0x02 string
//   0x07 Enum               0x09 bool
//   0x0c number (f64)       0x0d UDim
//   0x0e UDim2              0x0f Ray
//   0x12 BrickColor         0x13 Color3
//   0x15 Vector2            0x16 Vector3
//   0x1b CFrame             0x1c Instance ref
//   0x1e ValueArray         0x1f ValueTable
//
// CFrame uses the native's compact forms: kind 2 for an exact identity
// rotation, otherwise kind 0 plus the 6-byte quantized quaternion. The
// quantizer is the exact inverse of the native decoder (sub_33BEB0A), which
// is validated byte-for-byte against the captured CFrame.Angles call.
#include "instance/remote.h"

#include "instance/class_registry.h"
#include "instance/instance.h"
#include "instance/value_types.h"
#include "instance/vector3.h"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace rbx {

namespace {

RemoteSendFn g_send = nullptr;
void* g_send_user = nullptr;

void put_varint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(uint8_t(v) | 0x80);
        v >>= 7;
    }
    out.push_back(uint8_t(v));
}

void put_u16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(uint8_t(v));
    out.push_back(uint8_t(v >> 8));
}

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 24));
}

void put_f32(std::vector<uint8_t>& out, double d) {
    const float f = float(d);
    uint8_t b[4];
    std::memcpy(b, &f, 4);
    out.insert(out.end(), b, b + 4);
}

void put_f64(std::vector<uint8_t>& out, double d) {
    uint8_t b[8];
    std::memcpy(b, &d, 8);
    out.insert(out.end(), b, b + 8);
}

int round_clamp15(double v) {
    if (!(v == v))
        return 0; // NaN
    double r = std::round(v);
    if (r > 16383.0)
        r = 16383.0;
    if (r < -16383.0)
        r = -16383.0;
    return int(r);
}

// Matrix -> quaternion for the native's rotation convention (the standard
// quaternion-to-matrix form used by sub_33BEB0A's decode side).
void quat_from_cframe(const CFrame& c, double q[4]) {
    const double m00 = c.r00, m01 = c.r01, m02 = c.r02;
    const double m10 = c.r10, m11 = c.r11, m12 = c.r12;
    const double m20 = c.r20, m21 = c.r21, m22 = c.r22;
    const double tr = m00 + m11 + m22;
    if (tr > 0.0) {
        const double s = std::sqrt(tr + 1.0) * 2.0;
        q[3] = 0.25 * s;
        q[0] = (m21 - m12) / s;
        q[1] = (m02 - m20) / s;
        q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        q[3] = (m21 - m12) / s;
        q[0] = 0.25 * s;
        q[1] = (m01 + m10) / s;
        q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        q[3] = (m02 - m20) / s;
        q[0] = (m01 + m10) / s;
        q[1] = 0.25 * s;
        q[2] = (m12 + m21) / s;
    } else {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        q[3] = (m10 - m01) / s;
        q[0] = (m02 + m20) / s;
        q[1] = (m12 + m21) / s;
        q[2] = 0.25 * s;
    }
    const double n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (n > 0.0) {
        const double inv = 1.0 / std::sqrt(n);
        for (int i = 0; i < 4; ++i)
            q[i] *= inv;
    }
}

bool identity_rotation(const CFrame& c) {
    return c.r00 == 1.0 && c.r01 == 0.0 && c.r02 == 0.0 &&
           c.r10 == 0.0 && c.r11 == 1.0 && c.r12 == 0.0 &&
           c.r20 == 0.0 && c.r21 == 0.0 && c.r22 == 1.0;
}

#include "instance/enum_wire_ids.inc"

int enum_wire_id(const std::string& name) {
    static std::map<std::string, int> cache;
    auto it = cache.find(name);
    if (it != cache.end())
        return it->second;
    int id = -1;
    for (const EnumWireId& e : kEnumWireIds) {
        if (name == e.name) {
            id = e.id;
            break;
        }
    }
    cache[name] = id;
    return id;
}

const char* metatable_type(lua_State* L, int idx) {
    if (!lua_getmetatable(L, idx))
        return nullptr;
    lua_getfield(L, -1, "__type");
    const char* t = lua_tostring(L, -1);
    lua_pop(L, 2);
    return t;
}

// Recursion bound matches the snapshot parser's container bound.
constexpr int kMaxDepth = 32;

bool encode_value(lua_State* L, int idx, std::vector<uint8_t>& out, int depth,
                  std::string* err);

bool encode_table(lua_State* L, int idx, std::vector<uint8_t>& out, int depth,
                  std::string* err) {
    // Count entries and classify. The native sends arrays as ValueArray and
    // string-keyed dictionaries as ValueTable; anything else has no wire
    // shape (the native serializer rejects it too).
    const int abs = lua_absindex(L, idx);
    int total = 0;
    int max_index = 0;
    bool array = true;
    bool string_keys = true;
    lua_pushnil(L);
    while (lua_next(L, abs) != 0) {
        ++total;
        const int kt = lua_type(L, -2);
        if (kt == LUA_TNUMBER) {
            string_keys = false;
            const double d = lua_tonumber(L, -2);
            if (!(d >= 1.0) || d != std::floor(d) || d > 255.0)
                array = false;
            else
                max_index = std::max(max_index, int(d));
        } else {
            array = false;
            if (kt != LUA_TSTRING)
                string_keys = false;
        }
        lua_pop(L, 1); // value
    }
    if (array && total == max_index) {
        out.push_back(0x1e); // ValueArray
        put_varint(out, uint64_t(total));
        for (int i = 1; i <= total; ++i) {
            lua_rawgeti(L, abs, i);
            if (!encode_value(L, -1, out, depth + 1, err)) {
                lua_pop(L, 1);
                return false;
            }
            lua_pop(L, 1);
        }
        return true;
    }
    if (!string_keys) {
        if (err)
            *err = "table has mixed or non-string keys";
        return false;
    }
    out.push_back(0x1f); // ValueTable
    put_varint(out, uint64_t(total));
    // Roblox tables have no insertion order; Lua's iteration order is hash
    // dependent. Sort keys so one table always serializes the same way.
    std::vector<std::string> keys;
    keys.reserve(size_t(total));
    lua_pushnil(L);
    while (lua_next(L, abs) != 0) {
        size_t len = 0;
        const char* key = lua_tolstring(L, -2, &len);
        keys.emplace_back(key, len);
        lua_pop(L, 1);
    }
    std::sort(keys.begin(), keys.end());
    for (const std::string& key : keys) {
        // ValueTable keys are raw strings (no value tag).
        put_varint(out, uint64_t(key.size()));
        out.insert(out.end(), key.begin(), key.end());
        lua_getfield(L, abs, key.c_str());
        if (!encode_value(L, -1, out, depth + 1, err)) {
            lua_pop(L, 1);
            return false;
        }
        lua_pop(L, 1);
    }
    return true;
}

bool encode_value(lua_State* L, int idx, std::vector<uint8_t>& out, int depth,
                  std::string* err) {
    if (depth > kMaxDepth) {
        if (err)
            *err = "table nesting too deep";
        return false;
    }
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        out.push_back(0x00);
        return true;
    case LUA_TBOOLEAN:
        out.push_back(0x09);
        out.push_back(uint8_t(lua_toboolean(L, idx) ? 1 : 0));
        return true;
    case LUA_TNUMBER:
        out.push_back(0x0c);
        put_f64(out, lua_tonumber(L, idx));
        return true;
    case LUA_TSTRING: {
        size_t len = 0;
        const char* s = lua_tolstring(L, idx, &len);
        out.push_back(0x02);
        put_varint(out, uint64_t(len));
        out.insert(out.end(), s, s + len);
        return true;
    }
    case LUA_TTABLE:
        return encode_table(L, idx, out, depth, err);
    default:
        break;
    }

    const char* type = metatable_type(L, idx);
    if (!type) {
        if (err)
            *err = std::string("unsupported argument type ") +
                   luaL_typename(L, idx);
        return false;
    }
    const std::string t(type);
    if (t == "EnumItem") {
        const EnumItem v = check_enumitem(L, idx);
        const int id = enum_wire_id(v.enum_name);
        if (id < 0) {
            if (err)
                *err = "Enum." + v.enum_name + " has no wire id";
            return false;
        }
        out.push_back(0x07);
        put_u16(out, uint16_t(id));
        put_varint(out, uint64_t(int64_t(v.value)));
        return true;
    }
    if (t == "Instance") {
        Instance* inst = Instance::check(L, idx);
        if (!inst->network_id()) {
            if (err)
                *err = "Instance has no replication id";
            return false;
        }
        out.push_back(0x1c);
        put_varint(out, inst->network_lo() ? inst->network_lo() : 1);
        put_u32(out, inst->network_id());
        return true;
    }
    if (t == "Vector3") {
        const Vector3 v = check_vector3(L, idx);
        out.push_back(0x16);
        put_f32(out, v.x);
        put_f32(out, v.y);
        put_f32(out, v.z);
        return true;
    }
    if (t == "Vector2") {
        const Vector2 v = check_vector2(L, idx);
        out.push_back(0x15);
        put_f32(out, v.x);
        put_f32(out, v.y);
        return true;
    }
    if (t == "CFrame") {
        out.push_back(0x1b);
        const CFrame c = check_cframe(L, idx);
        encode_cframe_body(c, &out);
        return true;
    }
    if (t == "Color3") {
        const Color3 v = check_color3(L, idx);
        out.push_back(0x13);
        put_f32(out, v.r);
        put_f32(out, v.g);
        put_f32(out, v.b);
        return true;
    }
    if (t == "UDim") {
        const UDim v = check_udim(L, idx);
        out.push_back(0x0d);
        put_f32(out, v.scale);
        put_u32(out, uint32_t(int32_t(v.offset)));
        return true;
    }
    if (t == "UDim2") {
        const UDim2 v = check_udim2(L, idx);
        out.push_back(0x0e);
        put_f32(out, v.x.scale);
        put_u32(out, uint32_t(int32_t(v.x.offset)));
        put_f32(out, v.y.scale);
        put_u32(out, uint32_t(int32_t(v.y.offset)));
        return true;
    }
    if (t == "BrickColor") {
        const BrickColor v = check_brickcolor(L, idx);
        out.push_back(0x12);
        put_u16(out, uint16_t(v.number));
        return true;
    }
    if (t == "Rect") {
        const Rect v = check_rect(L, idx);
        out.push_back(0x28);
        put_f32(out, v.min.x);
        put_f32(out, v.min.y);
        put_f32(out, v.max.x);
        put_f32(out, v.max.y);
        return true;
    }
    if (t == "NumberRange") {
        const NumberRange v = check_numberrange(L, idx);
        out.push_back(0x25);
        put_f32(out, v.min);
        put_f32(out, v.max);
        return true;
    }
    if (t == "Ray") {
        const Ray v = check_ray(L, idx);
        out.push_back(0x0f);
        put_f32(out, v.origin.x);
        put_f32(out, v.origin.y);
        put_f32(out, v.origin.z);
        put_f32(out, v.direction.x);
        put_f32(out, v.direction.y);
        put_f32(out, v.direction.z);
        return true;
    }
    if (t == "DateTime") {
        const DateTime v = check_datetime(L, idx);
        out.push_back(0x30);
        put_f64(out, double(v.millis));
        return true;
    }
    if (err)
        *err = "unsupported argument type " + t;
    return false;
}

int m_fire_server(lua_State* L) {
    Instance* inst = Instance::check(L, 1);
    if (!(inst->is_a("RemoteEvent") || inst->is_a("UnreliableRemoteEvent"))) {
        luaL_error(L, "FireServer can only be called on a RemoteEvent");
        return 0;
    }
    if (!g_send) {
        luaL_error(L, "RemoteEvent:FireServer has no network transport");
        return 0;
    }
    std::vector<uint8_t> payload;
    std::string err;
    if (!encode_remote_event(L, 1, 2, &payload, &err)) {
        luaL_error(L, "Unable to serialize RemoteEvent argument: %s",
                   err.c_str());
        return 0;
    }
    const bool unreliable = inst->is_a("UnreliableRemoteEvent");
    if (!g_send(g_send_user, payload.data(), payload.size(), unreliable)) {
        luaL_error(L, "RemoteEvent:FireServer failed to send");
        return 0;
    }
    return 0;
}

} // namespace

void set_remote_send(RemoteSendFn fn, void* user) {
    g_send = fn;
    g_send_user = user;
}

void install_remote_methods() {
    attach_method("RemoteEvent", "FireServer", m_fire_server);
    attach_method("UnreliableRemoteEvent", "FireServer", m_fire_server);
}

void encode_cframe_body(const CFrame& c, std::vector<uint8_t>* out) {
    put_f32(*out, c.x);
    put_f32(*out, c.y);
    put_f32(*out, c.z);
    if (identity_rotation(c)) {
        out->push_back(0x02); // table kind 2 = identity
        return;
    }
    double q[4];
    quat_from_cframe(c, q);
    int drop = 3;
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(q[i]) > std::fabs(q[drop]))
            drop = i;
    }
    if (q[drop] < 0.0) {
        for (int i = 0; i < 4; ++i)
            q[i] = -q[i];
    }
    static const int kTab[12] = {1, 2, 3, 0, 2, 3, 0, 1, 3, 0, 1, 2};
    const int i0 = kTab[3 * drop], i1 = kTab[3 * drop + 1],
              i2 = kTab[3 * drop + 2];
    const double scale = std::sqrt(2.0) * 16383.0;
    const int f1 = round_clamp15(q[i0] * scale);
    const int v6 = round_clamp15(q[i1] * scale);
    const int f0 = round_clamp15(q[i2] * scale);
    out->push_back(0x00); // kind 0 = quantized quaternion follows
    const uint16_t a = uint16_t(f1 & 0x7fff);
    const uint32_t n = uint32_t(f0 & 0x7fff) |
                       (uint32_t(v6 & 0x7fff) << 15) |
                       (uint32_t(drop) << 30);
    out->push_back(uint8_t(a));
    out->push_back(uint8_t(a >> 8));
    put_u32(*out, n);
}

bool encode_remote_event(lua_State* L, int instance_index, int first_arg,
                         std::vector<uint8_t>* out, std::string* err) {
    if (!out) {
        if (err)
            *err = "no output buffer";
        return false;
    }
    out->clear();
    Instance* inst = nullptr;
    {
        // The public entry point is called from m_fire_server (already
        // checked) and from tests; a non-instance is a hard error.
        inst = Instance::check(L, instance_index);
    }
    if (!inst->network_id()) {
        if (err)
            *err = "RemoteEvent has no replication id";
        return false;
    }
    const int argc = lua_gettop(L) - first_arg + 1;
    if (argc < 0 || argc > 255) {
        if (err)
            *err = "too many arguments";
        return false;
    }
    out->push_back(0x83);
    out->push_back(0x07);
    put_varint(*out, inst->network_lo() ? inst->network_lo() : 1);
    put_u32(*out, inst->network_id());
    put_u16(*out, 0x11bb); // native RemoteEvent invocation id
    out->push_back(uint8_t(argc));
    for (int i = first_arg; i < first_arg + argc; ++i) {
        if (!encode_value(L, i, *out, 0, err))
            return false;
    }
    out->push_back(0x00); // chain end
    return true;
}

} // namespace rbx
