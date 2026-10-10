// cframe.cpp — CFrame value type (see value_types.h).
//
// Docs-verified surface: new()/new(pos)/new(pos,lookAt)/new(x,y,z)/
// new(x,y,z,qX,qY,qZ,qW)/new(x,y,z,R00..R22), fromMatrix, fromAxisAngle,
// lookAt/lookAlong/fromRotationBetweenVectors, identity, X/Y/Z, Position,
// Rotation, Look/Right/Up/X/Y/ZVector, Inverse, Orthonormalize,
// GetComponents/components, ToWorldSpace/ToObjectSpace + Point/Vector
// variants, FuzzyEq, the four math metamethods. Euler/Lerp/Angle forms are
// skipped: their order/interpolation conventions are unverifiable offline.
//
// Storage is the GetComponents order (x, y, z, R00..R22); rows R0/R1/R2
// are the X/Y/Z basis vectors, LookVector = -Z.
#include "instance/value_types.h"

#include "instance/vector3.h"

#include "lua.h"
#include "lualib.h"

#include <cmath>
#include <cstring>

namespace rbx {

namespace {

void basis(const CFrame& c, double x[3], double y[3], double z[3]) {
    x[0] = c.r00;
    x[1] = c.r01;
    x[2] = c.r02;
    y[0] = c.r10;
    y[1] = c.r11;
    y[2] = c.r12;
    z[0] = c.r20;
    z[1] = c.r21;
    z[2] = c.r22;
}

CFrame mul(const CFrame& a, const CFrame& b) {
    CFrame o;
    o.r00 = a.r00 * b.r00 + a.r01 * b.r10 + a.r02 * b.r20;
    o.r01 = a.r00 * b.r01 + a.r01 * b.r11 + a.r02 * b.r21;
    o.r02 = a.r00 * b.r02 + a.r01 * b.r12 + a.r02 * b.r22;
    o.r10 = a.r10 * b.r00 + a.r11 * b.r10 + a.r12 * b.r20;
    o.r11 = a.r10 * b.r01 + a.r11 * b.r11 + a.r12 * b.r21;
    o.r12 = a.r10 * b.r02 + a.r11 * b.r12 + a.r12 * b.r22;
    o.r20 = a.r20 * b.r00 + a.r21 * b.r10 + a.r22 * b.r20;
    o.r21 = a.r20 * b.r01 + a.r21 * b.r11 + a.r22 * b.r21;
    o.r22 = a.r20 * b.r02 + a.r21 * b.r12 + a.r22 * b.r22;
    o.x = a.r00 * b.x + a.r01 * b.y + a.r02 * b.z + a.x;
    o.y = a.r10 * b.x + a.r11 * b.y + a.r12 * b.z + a.y;
    o.z = a.r20 * b.x + a.r21 * b.y + a.r22 * b.z + a.z;
    return o;
}

Vector3 rot_only(const CFrame& a, const Vector3& v) {
    return Vector3{a.r00 * v.x + a.r01 * v.y + a.r02 * v.z,
                   a.r10 * v.x + a.r11 * v.y + a.r12 * v.z,
                   a.r20 * v.x + a.r21 * v.y + a.r22 * v.z};
}

CFrame inverse(const CFrame& c) {
    CFrame o;
    o.r00 = c.r00;
    o.r01 = c.r10;
    o.r02 = c.r20;
    o.r10 = c.r01;
    o.r11 = c.r11;
    o.r12 = c.r21;
    o.r20 = c.r02;
    o.r21 = c.r12;
    o.r22 = c.r22;
    o.x = -(o.r00 * c.x + o.r01 * c.y + o.r02 * c.z);
    o.y = -(o.r10 * c.x + o.r11 * c.y + o.r12 * c.z);
    o.z = -(o.r20 * c.x + o.r21 * c.y + o.r22 * c.z);
    return o;
}

CFrame from_quat(double qx, double qy, double qz, double qw) {
    CFrame c;
    c.r00 = 1.0 - 2.0 * (qy * qy + qz * qz);
    c.r01 = 2.0 * (qx * qy - qz * qw);
    c.r02 = 2.0 * (qx * qz + qy * qw);
    c.r10 = 2.0 * (qx * qy + qz * qw);
    c.r11 = 1.0 - 2.0 * (qx * qx + qz * qz);
    c.r12 = 2.0 * (qy * qz - qx * qw);
    c.r20 = 2.0 * (qx * qz - qy * qw);
    c.r21 = 2.0 * (qy * qz + qx * qw);
    c.r22 = 1.0 - 2.0 * (qx * qx + qy * qy);
    return c;
}

// Orientation looking from pos toward target (-Z forward). up_override may
// be null (then Y-up candidates are tried until non-degenerate).
CFrame look_at_up(const Vector3& pos, const Vector3& target, const Vector3* up_override) {
    double zx = pos.x - target.x, zy = pos.y - target.y, zz = pos.z - target.z;
    const double zl = std::sqrt(zx * zx + zy * zy + zz * zz);
    if (zl > 0.0) {
        zx /= zl;
        zy /= zl;
        zz /= zl;
    }
    const double ups[3][3] = {{0.0, 1.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}};
    double xx = 0.0, xy = 0.0, xz = 0.0;
    if (up_override) {
        xx = up_override->y * zz - up_override->z * zy;
        xy = up_override->z * zx - up_override->x * zz;
        xz = up_override->x * zy - up_override->y * zx;
    } else {
        for (int u = 0; u < 3; u++) {
            const double* up = ups[u];
            xx = up[1] * zz - up[2] * zy;
            xy = up[2] * zx - up[0] * zz;
            xz = up[0] * zy - up[1] * zx;
            const double xl = std::sqrt(xx * xx + xy * xy + xz * xz);
            if (xl > 1e-9) {
                xx /= xl;
                xy /= xl;
                xz /= xl;
                break;
            }
            xx = xy = xz = 0.0;
        }
    }
    const double yx = zy * xz - zz * xy, yy = zz * xx - zx * xz, yz = zx * xy - zy * xx;
    CFrame c;
    c.x = pos.x;
    c.y = pos.y;
    c.z = pos.z;
    c.r00 = xx;
    c.r01 = xy;
    c.r02 = xz;
    c.r10 = yx;
    c.r11 = yy;
    c.r12 = yz;
    c.r20 = zx;
    c.r21 = zy;
    c.r22 = zz;
    return c;
}

CFrame look_at(const Vector3& pos, const Vector3& target) {
    return look_at_up(pos, target, nullptr);
}

int cf_index(lua_State* L) {
    const CFrame* v = static_cast<const CFrame*>(luaL_checkudata(L, 1, "CFrame"));
    const char* k = luaL_checkstring(L, 2);
    if (strcmp(k, "X") == 0 || strcmp(k, "x") == 0) {
        lua_pushnumber(L, v->x);
        return 1;
    }
    if (strcmp(k, "Y") == 0 || strcmp(k, "y") == 0) {
        lua_pushnumber(L, v->y);
        return 1;
    }
    if (strcmp(k, "Z") == 0 || strcmp(k, "z") == 0) {
        lua_pushnumber(L, v->z);
        return 1;
    }
    if (strcmp(k, "Position") == 0 || strcmp(k, "position") == 0) {
        push_vector3(L, Vector3{v->x, v->y, v->z});
        return 1;
    }
    if (strcmp(k, "Rotation") == 0 || strcmp(k, "rotation") == 0) {
        CFrame r = *v;
        r.x = r.y = r.z = 0.0;
        push_cframe(L, r);
        return 1;
    }
    if (strcmp(k, "LookVector") == 0 || strcmp(k, "lookVector") == 0) {
        push_vector3(L, Vector3{-v->r20, -v->r21, -v->r22});
        return 1;
    }
    if (strcmp(k, "RightVector") == 0 || strcmp(k, "rightVector") == 0) {
        push_vector3(L, Vector3{v->r00, v->r01, v->r02});
        return 1;
    }
    if (strcmp(k, "UpVector") == 0 || strcmp(k, "upVector") == 0) {
        push_vector3(L, Vector3{v->r10, v->r11, v->r12});
        return 1;
    }
    if (strcmp(k, "XVector") == 0 || strcmp(k, "xVector") == 0) {
        push_vector3(L, Vector3{v->r00, v->r01, v->r02});
        return 1;
    }
    if (strcmp(k, "YVector") == 0 || strcmp(k, "yVector") == 0) {
        push_vector3(L, Vector3{v->r10, v->r11, v->r12});
        return 1;
    }
    if (strcmp(k, "ZVector") == 0 || strcmp(k, "zVector") == 0) {
        push_vector3(L, Vector3{v->r20, v->r21, v->r22});
        return 1;
    }
    lua_getmetatable(L, 1);
    lua_getfield(L, -1, "Methods");
    lua_getfield(L, -1, k);
    return 1;
}

void push_comps(lua_State* L, const CFrame& v) {
    lua_pushnumber(L, v.x);
    lua_pushnumber(L, v.y);
    lua_pushnumber(L, v.z);
    lua_pushnumber(L, v.r00);
    lua_pushnumber(L, v.r01);
    lua_pushnumber(L, v.r02);
    lua_pushnumber(L, v.r10);
    lua_pushnumber(L, v.r11);
    lua_pushnumber(L, v.r12);
    lua_pushnumber(L, v.r20);
    lua_pushnumber(L, v.r21);
    lua_pushnumber(L, v.r22);
}

int cf_getcomponents(lua_State* L) {
    push_comps(L, check_cframe(L, 1));
    return 12;
}

int cf_inverse(lua_State* L) {
    push_cframe(L, inverse(check_cframe(L, 1)));
    return 1;
}

int cf_orthonormalize(lua_State* L) {
    const CFrame v = check_cframe(L, 1);
    double x[3], y[3], z[3];
    basis(v, x, y, z);
    double xl = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (xl > 0.0) {
        x[0] /= xl;
        x[1] /= xl;
        x[2] /= xl;
    }
    double d = y[0] * x[0] + y[1] * x[1] + y[2] * x[2];
    y[0] -= d * x[0];
    y[1] -= d * x[1];
    y[2] -= d * x[2];
    double yl = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    if (yl > 0.0) {
        y[0] /= yl;
        y[1] /= yl;
        y[2] /= yl;
    }
    z[0] = x[1] * y[2] - x[2] * y[1];
    z[1] = x[2] * y[0] - x[0] * y[2];
    z[2] = x[0] * y[1] - x[1] * y[0];
    CFrame o;
    o.x = v.x;
    o.y = v.y;
    o.z = v.z;
    o.r00 = x[0];
    o.r01 = x[1];
    o.r02 = x[2];
    o.r10 = y[0];
    o.r11 = y[1];
    o.r12 = y[2];
    o.r20 = z[0];
    o.r21 = z[1];
    o.r22 = z[2];
    push_cframe(L, o);
    return 1;
}

// ToWorldSpace/ToObjectSpace + Point/Vector variants: fold left to right.
int cf_toworld(lua_State* L) {
    CFrame acc = check_cframe(L, 1);
    const int top = lua_gettop(L);
    for (int i = 2; i <= top; i++)
        acc = mul(acc, check_cframe(L, i));
    push_cframe(L, acc);
    return 1;
}

int cf_toobject(lua_State* L) {
    CFrame acc = inverse(check_cframe(L, 1));
    const int top = lua_gettop(L);
    for (int i = 2; i <= top; i++)
        acc = mul(acc, check_cframe(L, i));
    push_cframe(L, acc);
    return 1;
}

// PointToWorldSpace(v...): full transform each; Vector*: rotation only.
int cf_point(lua_State* L, bool world) {
    const CFrame self = check_cframe(L, 1);
    const CFrame base = world ? self : inverse(self);
    const int top = lua_gettop(L);
    for (int i = 2; i <= top; i++) {
        const Vector3 v = check_vector3(L, i);
        push_vector3(L, Vector3{base.r00 * v.x + base.r01 * v.y + base.r02 * v.z + base.x,
                                base.r10 * v.x + base.r11 * v.y + base.r12 * v.z + base.y,
                                base.r20 * v.x + base.r21 * v.y + base.r22 * v.z + base.z});
    }
    return top - 1;
}

int cf_vector(lua_State* L, bool world) {
    const CFrame self = check_cframe(L, 1);
    const CFrame base = world ? self : inverse(self);
    const int top = lua_gettop(L);
    for (int i = 2; i <= top; i++)
        push_vector3(L, rot_only(base, check_vector3(L, i)));
    return top - 1;
}

int cf_pointworld(lua_State* L) {
    return cf_point(L, true);
}
int cf_pointobject(lua_State* L) {
    return cf_point(L, false);
}
int cf_vectorworld(lua_State* L) {
    return cf_vector(L, true);
}
int cf_vectorobject(lua_State* L) {
    return cf_vector(L, false);
}

int cf_fuzzy(lua_State* L) {
    const CFrame a = check_cframe(L, 1);
    const CFrame b = check_cframe(L, 2);
    const double eps = luaL_optnumber(L, 3, 0.00001);
    const double ds[] = {a.x - b.x, a.y - b.y, a.z - b.z,
                         a.r00 - b.r00, a.r01 - b.r01, a.r02 - b.r02,
                         a.r10 - b.r10, a.r11 - b.r11, a.r12 - b.r12,
                         a.r20 - b.r20, a.r21 - b.r21, a.r22 - b.r22};
    bool ok = true;
    for (double d : ds) {
        if (!(std::fabs(d) <= eps)) {
            ok = false;
            break;
        }
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// new() | new(pos) | new(pos, lookAt) | new(x,y,z) | new(x,y,z,qX..qW) |
// new(x,y,z,R00..R22)
int cf_new(lua_State* L) {
    const int top = lua_gettop(L);
    if (top == 0) {
        push_cframe(L, CFrame{});
        return 1;
    }
    if (top == 1 && lua_type(L, 1) != LUA_TNUMBER) {
        const Vector3 p = check_vector3(L, 1);
        CFrame c;
        c.x = p.x;
        c.y = p.y;
        c.z = p.z;
        push_cframe(L, c);
        return 1;
    }
    if (top == 2) {
        const Vector3 p = check_vector3(L, 1);
        const Vector3 t = check_vector3(L, 2);
        push_cframe(L, look_at(p, t));
        return 1;
    }
    if (top == 3 || top == 7 || top == 12) {
        CFrame c;
        c.x = luaL_checknumber(L, 1);
        c.y = luaL_checknumber(L, 2);
        c.z = luaL_checknumber(L, 3);
        if (top == 7) { // quaternion form
            const double qx = luaL_checknumber(L, 4), qy = luaL_checknumber(L, 5);
            const double qz = luaL_checknumber(L, 6), qw = luaL_checknumber(L, 7);
            CFrame r = from_quat(qx, qy, qz, qw);
            c.r00 = r.r00;
            c.r01 = r.r01;
            c.r02 = r.r02;
            c.r10 = r.r10;
            c.r11 = r.r11;
            c.r12 = r.r12;
            c.r20 = r.r20;
            c.r21 = r.r21;
            c.r22 = r.r22;
        } else if (top == 12) { // rotation matrix form
            c.r00 = luaL_checknumber(L, 4);
            c.r01 = luaL_checknumber(L, 5);
            c.r02 = luaL_checknumber(L, 6);
            c.r10 = luaL_checknumber(L, 7);
            c.r11 = luaL_checknumber(L, 8);
            c.r12 = luaL_checknumber(L, 9);
            c.r20 = luaL_checknumber(L, 10);
            c.r21 = luaL_checknumber(L, 11);
            c.r22 = luaL_checknumber(L, 12);
        }
        push_cframe(L, c);
        return 1;
    }
    luaL_error(L, "invalid CFrame.new arguments");
    return 0;
}

int cf_frommatrix(lua_State* L) {
    const Vector3 p = check_vector3(L, 1);
    const Vector3 vx = check_vector3(L, 2);
    const Vector3 vy = check_vector3(L, 3);
    const Vector3 vz = check_vector3(L, 4);
    CFrame c;
    c.x = p.x;
    c.y = p.y;
    c.z = p.z;
    c.r00 = vx.x;
    c.r01 = vx.y;
    c.r02 = vx.z;
    c.r10 = vy.x;
    c.r11 = vy.y;
    c.r12 = vy.z;
    c.r20 = vz.x;
    c.r21 = vz.y;
    c.r22 = vz.z;
    push_cframe(L, c);
    return 1;
}

int cf_fromaxisangle(lua_State* L) {
    Vector3 v = check_vector3(L, 1);
    const double r = luaL_checknumber(L, 2);
    const double l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (l > 0.0) {
        v.x /= l;
        v.y /= l;
        v.z /= l;
    }
    const double c = std::cos(r), s = std::sin(r), t = 1.0 - c;
    CFrame o;
    o.r00 = t * v.x * v.x + c;
    o.r01 = t * v.x * v.y - s * v.z;
    o.r02 = t * v.x * v.z + s * v.y;
    o.r10 = t * v.x * v.y + s * v.z;
    o.r11 = t * v.y * v.y + c;
    o.r12 = t * v.y * v.z - s * v.x;
    o.r20 = t * v.x * v.z - s * v.y;
    o.r21 = t * v.y * v.z + s * v.x;
    o.r22 = t * v.z * v.z + c;
    push_cframe(L, o);
    return 1;
}

int cf_lookat(lua_State* L) {
    const Vector3 at = check_vector3(L, 1);
    const Vector3 target = check_vector3(L, 2);
    if (lua_isnoneornil(L, 3)) {
        push_cframe(L, look_at(at, target));
        return 1;
    }
    const Vector3 up = check_vector3(L, 3);
    push_cframe(L, look_at_up(at, target, &up));
    return 1;
}

int cf_lookalong(lua_State* L) {
    const Vector3 at = check_vector3(L, 1);
    const Vector3 dir = check_vector3(L, 2);
    // lookAlong points -Z along `direction`: target = at - direction.
    const Vector3 target{at.x - dir.x, at.y - dir.y, at.z - dir.z};
    if (lua_isnoneornil(L, 3)) {
        push_cframe(L, look_at(at, target));
        return 1;
    }
    const Vector3 up = check_vector3(L, 3);
    push_cframe(L, look_at_up(at, target, &up));
    return 1;
}

// fromRotationBetweenVectors: shortest-arc quaternion, matrix form.
int cf_fromrotvecs(lua_State* L) {
    Vector3 f = check_vector3(L, 1);
    Vector3 t = check_vector3(L, 2);
    const double fl = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
    const double tl = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
    if (fl > 0.0) {
        f.x /= fl;
        f.y /= fl;
        f.z /= fl;
    }
    if (tl > 0.0) {
        t.x /= tl;
        t.y /= tl;
        t.z /= tl;
    }
    const double dot = f.x * t.x + f.y * t.y + f.z * t.z;
    double qx, qy, qz, qw;
    if (dot > 1.0 - 1e-9) {
        qx = qy = qz = 0.0;
        qw = 1.0;
    } else if (dot < -1.0 + 1e-9) { // antiparallel: rotate about a stable axis
        double ax = 1.0, ay = 0.0, az = 0.0;
        if (std::fabs(f.x) > 0.9) {
            ax = 0.0;
            ay = 1.0;
        }
        qx = f.y * az - f.z * ay;
        qy = f.z * ax - f.x * az;
        qz = f.x * ay - f.y * ax;
        qw = 0.0;
        const double ql = std::sqrt(qx * qx + qy * qy + qz * qz);
        qx /= ql;
        qy /= ql;
        qz /= ql;
    } else {
        qx = f.y * t.z - f.z * t.y;
        qy = f.z * t.x - f.x * t.z;
        qz = f.x * t.y - f.y * t.x;
        qw = 1.0 + dot;
        const double ql = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
        qx /= ql;
        qy /= ql;
        qz /= ql;
        qw /= ql;
    }
    push_cframe(L, from_quat(qx, qy, qz, qw));
    return 1;
}

int cf_tostring(lua_State* L) {
    const CFrame* v = static_cast<const CFrame*>(luaL_checkudata(L, 1, "CFrame"));
    lua_pushfstring(L, "%g, %g, %g, %g, %g, %g, %g, %g, %g, %g, %g, %g", v->x, v->y,
                    v->z, v->r00, v->r01, v->r02, v->r10, v->r11, v->r12, v->r20,
                    v->r21, v->r22);
    return 1;
}

int cf_eq(lua_State* L) {
    const CFrame a = check_cframe(L, 1);
    const CFrame b = check_cframe(L, 2);
    lua_pushboolean(L, a == b ? 1 : 0);
    return 1;
}

int cf_mul(lua_State* L) {
    // CFrame * CFrame vs CFrame * Vector3: probe the second operand's type.
    // Anything else falls through to check_cframe's clean type error.
    if (lua_getmetatable(L, 2)) { // [mt] (nothing pushed when absent)
        lua_getfield(L, -1, "__type"); // [mt, type|nil]
        const char* t = lua_tostring(L, -1);
        const bool is_v3 = t && std::strcmp(t, "Vector3") == 0;
        lua_pop(L, 2);
        if (is_v3) {
            const CFrame a = check_cframe(L, 1);
            const Vector3 v = check_vector3(L, 2);
            push_vector3(L,
                         Vector3{a.r00 * v.x + a.r01 * v.y + a.r02 * v.z + a.x,
                                 a.r10 * v.x + a.r11 * v.y + a.r12 * v.z + a.y,
                                 a.r20 * v.x + a.r21 * v.y + a.r22 * v.z + a.z});
            return 1;
        }
    }
    push_cframe(L, mul(check_cframe(L, 1), check_cframe(L, 2)));
    return 1;
}

int cf_add(lua_State* L) {
    const CFrame a = check_cframe(L, 1);
    const Vector3 v = check_vector3(L, 2);
    CFrame o = a;
    o.x += v.x;
    o.y += v.y;
    o.z += v.z;
    push_cframe(L, o);
    return 1;
}

int cf_sub(lua_State* L) {
    const CFrame a = check_cframe(L, 1);
    const Vector3 v = check_vector3(L, 2);
    CFrame o = a;
    o.x -= v.x;
    o.y -= v.y;
    o.z -= v.z;
    push_cframe(L, o);
    return 1;
}

} // namespace

void push_cframe(lua_State* L, const CFrame& v) {
    void* p = lua_newuserdata(L, sizeof(CFrame));
    *static_cast<CFrame*>(p) = v;
    if (luaL_newmetatable(L, "CFrame")) { // first time: fill it
        lua_newtable(L);
        lua_pushcfunction(L, cf_inverse, "Inverse");
        lua_setfield(L, -2, "Inverse");
        lua_pushcfunction(L, cf_orthonormalize, "Orthonormalize");
        lua_setfield(L, -2, "Orthonormalize");
        lua_pushcfunction(L, cf_toworld, "ToWorldSpace");
        lua_setfield(L, -2, "ToWorldSpace");
        lua_pushcfunction(L, cf_toobject, "ToObjectSpace");
        lua_setfield(L, -2, "ToObjectSpace");
        lua_pushcfunction(L, cf_pointworld, "PointToWorldSpace");
        lua_setfield(L, -2, "PointToWorldSpace");
        lua_pushcfunction(L, cf_pointobject, "PointToObjectSpace");
        lua_setfield(L, -2, "PointToObjectSpace");
        lua_pushcfunction(L, cf_vectorworld, "VectorToWorldSpace");
        lua_setfield(L, -2, "VectorToWorldSpace");
        lua_pushcfunction(L, cf_vectorobject, "VectorToObjectSpace");
        lua_setfield(L, -2, "VectorToObjectSpace");
        lua_pushcfunction(L, cf_getcomponents, "GetComponents");
        lua_setfield(L, -2, "GetComponents");
        lua_pushcfunction(L, cf_getcomponents, "components");
        lua_setfield(L, -2, "components");
        lua_pushcfunction(L, cf_fuzzy, "FuzzyEq");
        lua_setfield(L, -2, "FuzzyEq");
        lua_setfield(L, -2, "Methods");
        lua_pushcfunction(L, cf_index, "__index");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, cf_tostring, "__tostring");
        lua_setfield(L, -2, "__tostring");
        lua_pushcfunction(L, cf_eq, "__eq");
        lua_setfield(L, -2, "__eq");
        lua_pushcfunction(L, cf_mul, "__mul");
        lua_setfield(L, -2, "__mul");
        lua_pushcfunction(L, cf_add, "__add");
        lua_setfield(L, -2, "__add");
        lua_pushcfunction(L, cf_sub, "__sub");
        lua_setfield(L, -2, "__sub");
        lua_pushstring(L, "CFrame"); // typeof() == "CFrame" (Roblox parity)
        lua_setfield(L, -2, "__type");
    }
    lua_setmetatable(L, -2);
}

CFrame check_cframe(lua_State* L, int idx) {
    return *static_cast<const CFrame*>(luaL_checkudata(L, idx, "CFrame"));
}

void create_cframe_class(lua_State* L) {
    push_cframe(L, CFrame{});
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, cf_new, "new");
    lua_setfield(L, -2, "new");
    lua_pushcfunction(L, cf_frommatrix, "fromMatrix");
    lua_setfield(L, -2, "fromMatrix");
    lua_pushcfunction(L, cf_fromaxisangle, "fromAxisAngle");
    lua_setfield(L, -2, "fromAxisAngle");
    lua_pushcfunction(L, cf_lookat, "lookAt");
    lua_setfield(L, -2, "lookAt");
    lua_pushcfunction(L, cf_lookalong, "lookAlong");
    lua_setfield(L, -2, "lookAlong");
    lua_pushcfunction(L, cf_fromrotvecs, "fromRotationBetweenVectors");
    lua_setfield(L, -2, "fromRotationBetweenVectors");
    push_cframe(L, CFrame{}); // identity
    lua_setfield(L, -2, "identity");
    lua_setglobal(L, "CFrame");
}

} // namespace rbx