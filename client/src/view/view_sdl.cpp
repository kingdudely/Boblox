// view_sdl.cpp — SDL3 presentation backend for runViewer()/Live().
//
// No raylib and no other third-party renderer: SDL3 owns the window, the
// input, and the GL context; this file owns the OpenGL 3.3 core pipeline.
//
// What it does beyond the old per-part DrawMesh loop:
//   * instanced drawing — one draw call per part shape for the whole world,
//     instead of two shader uniform uploads and one draw per part
//   * a real directional shadow pass (2048² depth map, 3x3 PCF), so parts
//     sit on the baseplate instead of floating
//   * MSAA 4x through an offscreen multisample target that is blit-resolved,
//     so --shot captures exactly what the window shows
//   * frustum culling on the CPU, alpha pass sorted back-to-front
//   * sky gradient + sun bloom, hemisphere ambient, filmic shoulder, haze
//   * correct silhouettes: real wedge prism and X-axis cylinder meshes
//   * material response for Neon/Glass/Metal/ForceField/Water
//
// Built only when SDL3 + OpenGL dev are present at configure time. Without
// them view_stub.cpp takes over and --headful fails loudly.
#include "view/view.h"

#include "gui/gui_layout.h"
#include "instance/gui_input.h"
#include "instance/host_input.h"
#include "view/mesh_asset.h"
#include "view/gui_renderer.h"

#include <SDL3/SDL.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GL/glcorearb.h>

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "stb_image.h"

namespace rbxclient {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// SDL keycodes are close to Roblox Enum.KeyCode but arrows and modifiers use
// SDL's scancode-derived constants. Roblox letters are lowercase ASCII (W =
// 119), which is exactly SDLK_A..SDLK_Z, so only the special keys need a
// translation.
int roblox_keycode(SDL_Keycode key) {
    switch (key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        return 13;
    case SDLK_ESCAPE: return 27;
    case SDLK_BACKSPACE: return 8;
    case SDLK_TAB: return 9;
    case SDLK_SPACE: return 32;
    case SDLK_DELETE: return 127;
    case SDLK_UP: return 273;
    case SDLK_DOWN: return 274;
    case SDLK_RIGHT: return 275;
    case SDLK_LEFT: return 276;
    case SDLK_INSERT: return 277;
    case SDLK_HOME: return 278;
    case SDLK_END: return 279;
    case SDLK_PAGEUP: return 280;
    case SDLK_PAGEDOWN: return 281;
    case SDLK_LSHIFT: return 304;
    case SDLK_RSHIFT: return 303;
    case SDLK_LCTRL: return 306;
    case SDLK_RCTRL: return 305;
    case SDLK_LALT: return 308;
    case SDLK_RALT: return 307;
    case SDLK_LGUI: return 310; // Enum.KeyCode.LeftMeta
    case SDLK_RGUI: return 309; // Enum.KeyCode.RightMeta
    default:
        // Letters, digits and punctuation already match the enum.
        return int(key) >= 32 && int(key) < 127 ? int(key) : 0;
    }
}

// ---- small math library ----------------------------------------------------

struct V3 {
    float x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
inline V3 normalize(V3 a) {
    const float l = std::sqrt(dot(a, a));
    return l > 1e-8f ? a * (1.0f / l) : V3{0, 0, 0};
}
inline V3 lerp(V3 a, V3 b, float t) { return a + (b - a) * t; }

// Column-major 4x4, element (row, col) at m[col * 4 + row] — the layout
// GLSL's mat4 constructor and glUniformMatrix4fv transpose=GL_FALSE expect.
struct M4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

M4 mul(const M4& a, const M4& b) {
    M4 r;
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k)
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    }
    return r;
}

M4 perspective(float fovy_deg, float aspect, float znear, float zfar) {
    M4 p;
    std::memset(p.m, 0, sizeof(p.m));
    const float f = 1.0f / std::tan(fovy_deg * kPi / 360.0f);
    p.m[0] = f / std::max(aspect, 1e-4f);
    p.m[5] = f;
    p.m[10] = (zfar + znear) / (znear - zfar);
    p.m[11] = -1.0f;
    p.m[14] = (2.0f * zfar * znear) / (znear - zfar);
    return p;
}

M4 ortho(float l, float r, float b, float t, float n, float f) {
    M4 o;
    std::memset(o.m, 0, sizeof(o.m));
    o.m[0] = 2.0f / (r - l);
    o.m[5] = 2.0f / (t - b);
    o.m[10] = -2.0f / (f - n);
    o.m[12] = -(r + l) / (r - l);
    o.m[13] = -(t + b) / (t - b);
    o.m[14] = -(f + n) / (f - n);
    o.m[15] = 1.0f;
    return o;
}

M4 lookAt(V3 eye, V3 center, V3 up) {
    const V3 f = normalize(center - eye);
    const V3 s = normalize(cross(f, up));
    const V3 u = cross(s, f);
    M4 v;
    std::memset(v.m, 0, sizeof(v.m));
    v.m[0] = s.x;
    v.m[1] = u.x;
    v.m[2] = -f.x;
    v.m[4] = s.y;
    v.m[5] = u.y;
    v.m[6] = -f.y;
    v.m[8] = s.z;
    v.m[9] = u.z;
    v.m[10] = -f.z;
    v.m[12] = -dot(s, eye);
    v.m[13] = -dot(u, eye);
    v.m[14] = dot(f, eye);
    v.m[15] = 1.0f;
    return v;
}

// General 4x4 inverse (MESA's gluInvertMatrix) — only the sky pass needs it,
// to turn an NDC corner into a world-space view ray.
bool invert(const M4& in, M4& out) {
    const float* m = in.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] -
             m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] +
             m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] -
             m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] +
              m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] +
             m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] -
             m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] +
             m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] -
              m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] -
             m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] +
             m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] -
              m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] +
              m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] +
             m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] -
             m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] +
              m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] -
              m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-20f)
        return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i)
        out.m[i] = inv[i] * det;
    return true;
}

// ---- meshes ----------------------------------------------------------------

struct MeshData {
    std::vector<float> v; // position(3) normal(3) uv(2)
    std::vector<unsigned> idx;
    void vert(float x, float y, float z, float nx, float ny, float nz,
              float u = 0.0f, float w = 0.0f) {
        v.push_back(x);
        v.push_back(y);
        v.push_back(z);
        v.push_back(nx);
        v.push_back(ny);
        v.push_back(nz);
        v.push_back(u);
        v.push_back(w);
        idx.push_back(unsigned(idx.size()));
    }
    void tri(const float* p, float nx, float ny, float nz,
             float u = 0.0f, float w = 0.0f) {
        for (int i = 0; i < 3; ++i)
            vert(p[i * 3 + 0], p[i * 3 + 1], p[i * 3 + 2], nx, ny, nz, u, w);
    }
    // A quad must use one diagonal consistently: (a,b,c) + (a,c,d).
    // Emitting (a,b,c) + (b,c,d) crosses diagonals and leaves a hole.
    void quad(const float* p, float nx, float ny, float nz,
              float u = 0.0f, float w = 0.0f) {
        tri(p, nx, ny, nz, u, w);
        const float second[] = {p[0],  p[1],  p[2],  p[6],  p[7],  p[8],
                                p[9],  p[10], p[11]};
        tri(second, nx, ny, nz, u, w);
    }
};

// Native Mesh::makeWedge (ROBLOX2016 App/v8world/Mesh.cpp): six vertices,
// rectangular base, vertical +Z back, and a slope from the +Z top edge down
// to the -Z bottom edge.
MeshData wedgeMesh() {
    MeshData md;
    // Native face table: rectangular base, full-height +Z back, and the
    // raised +Z top edge that slopes down to the -Z bottom edge.
    const float back[] = {0.5f,  0.5f,  0.5f, -0.5f, 0.5f,  0.5f,
                          -0.5f, -0.5f, 0.5f, 0.5f,  -0.5f, 0.5f};
    md.quad(&back[0], 0, 0, 1);
    const float slope[] = {-0.5f, 0.5f,  0.5f, 0.5f,  0.5f,  0.5f,
                           0.5f,  -0.5f, -0.5f, -0.5f, -0.5f, -0.5f};
    const float k = 0.70710678f;
    md.quad(&slope[0], 0, k, -k);
    const float right[] = {0.5f, 0.5f, 0.5f, 0.5f, -0.5f, 0.5f,
                           0.5f, -0.5f, -0.5f};
    md.tri(right, 1, 0, 0);
    const float left[] = {-0.5f, 0.5f, 0.5f, -0.5f, -0.5f, -0.5f,
                          -0.5f, -0.5f, 0.5f};
    md.tri(left, -1, 0, 0);
    const float bottom[] = {-0.5f, -0.5f, -0.5f, 0.5f,  -0.5f, -0.5f,
                            0.5f,  -0.5f, 0.5f,  -0.5f, -0.5f, 0.5f};
    md.quad(&bottom[0], 0, -1, 0);
    return md;
}

// Native Mesh::makeCornerWedge (ROBLOX2016 App/v8world/Mesh.cpp): five
// vertices. The bottom is a complete rectangle; the apex is the +X/-Z top
// corner, giving two vertical triangular sides (+X, -Z) and two slopes
// (toward +Z and -X). Do not reuse the Wedge mesh here.
MeshData cornerWedgeMesh() {
    MeshData md;
    const float k = 0.70710678f;
    const float plusX[] = {0.5f, -0.5f, 0.5f, 0.5f, -0.5f, -0.5f,
                           0.5f, 0.5f, -0.5f};
    md.tri(plusX, 1, 0, 0);
    const float minusZ[] = {0.5f, -0.5f, -0.5f, -0.5f, -0.5f, -0.5f,
                            0.5f, 0.5f, -0.5f};
    md.tri(minusZ, 0, 0, -1);
    const float slopeZ[] = {0.5f, -0.5f, 0.5f, 0.5f, 0.5f, -0.5f,
                            -0.5f, -0.5f, 0.5f};
    md.tri(slopeZ, 0, k, k);
    const float slopeX[] = {-0.5f, -0.5f, -0.5f, -0.5f, -0.5f, 0.5f,
                            0.5f, 0.5f, -0.5f};
    md.tri(slopeX, -k, k, 0);
    const float bottom[] = {0.5f,  -0.5f, -0.5f, 0.5f,  -0.5f, 0.5f,
                            -0.5f, -0.5f, 0.5f,  -0.5f, -0.5f, -0.5f};
    md.quad(&bottom[0], 0, -1, 0);
    return md;
}

// Roblox cylinders are X-axis parts; a Y-axis mesh would visibly stand pipes
// on end. Size maps to (length, diameter, diameter) exactly like a Part.
MeshData cylinderXMesh(int slices = 24) {
    MeshData md;
    for (int i = 0; i < slices; ++i) {
        const float a0 = 2.0f * kPi * float(i) / float(slices);
        const float a1 = 2.0f * kPi * float(i + 1) / float(slices);
        const float y0 = std::cos(a0) * 0.5f, z0 = std::sin(a0) * 0.5f;
        const float y1 = std::cos(a1) * 0.5f, z1 = std::sin(a1) * 0.5f;
        const float n0y = std::cos(a0), n0z = std::sin(a0);
        const float n1y = std::cos(a1), n1z = std::sin(a1);
        // side quad
        md.vert(-0.5f, y0, z0, 0, n0y, n0z, 0, 0);
        md.vert(0.5f, y0, z0, 0, n0y, n0z, 1, 0);
        md.vert(0.5f, y1, z1, 0, n1y, n1z, 1, 1);
        md.vert(-0.5f, y0, z0, 0, n0y, n0z, 0, 0);
        md.vert(0.5f, y1, z1, 0, n1y, n1z, 1, 1);
        md.vert(-0.5f, y1, z1, 0, n1y, n1z, 0, 1);
        // caps
        md.vert(-0.5f, 0, 0, -1, 0, 0, 0.5f, 0.5f);
        md.vert(-0.5f, y1, z1, -1, 0, 0, 0, 0);
        md.vert(-0.5f, y0, z0, -1, 0, 0, 1, 0);
        md.vert(0.5f, 0, 0, 1, 0, 0, 0.5f, 0.5f);
        md.vert(0.5f, y0, z0, 1, 0, 0, 0, 0);
        md.vert(0.5f, y1, z1, 1, 0, 0, 1, 0);
    }
    return md;
}

MeshData sphereMesh(int segments = 28, int rings = 18) {
    MeshData md;
    for (int j = 0; j < rings; ++j) {
        const float t0 = kPi * float(j) / float(rings);
        const float t1 = kPi * float(j + 1) / float(rings);
        for (int i = 0; i < segments; ++i) {
            const float p0 = 2.0f * kPi * float(i) / float(segments);
            const float p1 = 2.0f * kPi * float(i + 1) / float(segments);
            auto at = [](float theta, float phi, float* o) {
                o[0] = std::sin(theta) * std::cos(phi) * 0.5f;
                o[1] = std::cos(theta) * 0.5f;
                o[2] = std::sin(theta) * std::sin(phi) * 0.5f;
            };
            float a[3], b[3], c[3], d[3];
            at(t0, p0, a);
            at(t1, p0, b);
            at(t1, p1, c);
            at(t0, p1, d);
            auto put = [&](float* p) {
                const float l = std::sqrt(p[0] * p[0] + p[1] * p[1] +
                                          p[2] * p[2]);
                md.vert(p[0], p[1], p[2], p[0] / l, p[1] / l, p[2] / l);
            };
            put(a);
            put(b);
            put(c);
            put(a);
            put(c);
            put(d);
        }
    }
    return md;
}

MeshData boxMesh() {
    MeshData md;
    // Six faces, flat normals, unit cube centred at the origin.
    struct Face {
        float n[3];
        float p[4][3];
    };
    const Face faces[6] = {
        {{0, 0, 1},
         {{-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f},
          {-0.5f, 0.5f, 0.5f}}},
        {{0, 0, -1},
         {{0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
          {0.5f, 0.5f, -0.5f}}},
        {{1, 0, 0},
         {{0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f},
          {0.5f, 0.5f, 0.5f}}},
        {{-1, 0, 0},
         {{-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f},
          {-0.5f, 0.5f, -0.5f}}},
        {{0, 1, 0},
         {{-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f},
          {-0.5f, 0.5f, -0.5f}}},
        {{0, -1, 0},
         {{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f},
          {-0.5f, -0.5f, 0.5f}}},
    };
    for (const Face& f : faces) {
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (int k : order) {
            md.vert(f.p[k][0], f.p[k][1], f.p[k][2], f.n[0], f.n[1], f.n[2],
                    k == 1 || k == 2 ? 1.0f : 0.0f,
                    k >= 2 ? 1.0f : 0.0f);
        }
    }
    return md;
}

// ---- shaders ---------------------------------------------------------------

const char* kSceneVS = R"GLSL(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aM0;
layout(location = 4) in vec4 aM1;
layout(location = 5) in vec4 aM2;
layout(location = 6) in vec4 aM3;
layout(location = 7) in vec4 aColor;
layout(location = 8) in vec4 aParams;

uniform mat4 uViewProj;
uniform mat4 uLightVP;

out vec3 vWorld;
out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;
out vec4 vParams;
out vec4 vLightPos;

void main() {
    mat4 model = mat4(aM0, aM1, aM2, aM3);
    vec4 world = model * vec4(aPos, 1.0);
    vWorld = world.xyz;
    mat3 rot = mat3(model);
    // Inverse-transpose of R*S is R*S^-1: exact normals under non-uniform
    // scale (thin walls, flat roads) instead of the usual skewed normal.
    vec3 s = sqrt(max(vec3(dot(rot[0], rot[0]), dot(rot[1], rot[1]),
                           dot(rot[2], rot[2])), vec3(1e-8)));
    vNormal = normalize(rot * (aNormal / s));
    vUV = aUV;
    vColor = aColor;
    vParams = aParams;
    vLightPos = uLightVP * world;
    gl_Position = uViewProj * world;
}
)GLSL";

const char* kSceneFS = R"GLSL(#version 330 core
in vec3 vWorld;
in vec3 vNormal;
in vec2 vUV;
in vec4 vColor;
in vec4 vParams;
in vec4 vLightPos;
out vec4 fragColor;

uniform vec3 uViewPos;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform vec3 uSkyColor;
uniform vec3 uGroundColor;
uniform vec3 uFogColor;
uniform float uFogDensity;
uniform sampler2D uShadow;
uniform sampler2D uTexture;
uniform float uShadowTexel;
uniform float uShadowStrength;

float shadowFactor(vec3 n, vec3 l) {
    vec3 p = vLightPos.xyz / max(vLightPos.w, 1e-6);
    p = p * 0.5 + 0.5;
    if (p.x <= 0.0 || p.x >= 1.0 || p.y <= 0.0 || p.y >= 1.0 || p.z >= 1.0)
        return 1.0;
    float ndl = max(dot(n, l), 0.0);
    float bias = mix(0.0030, 0.0007, ndl);
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float d = texture(uShadow, p.xy + vec2(x, y) * uShadowTexel).r;
            sum += (p.z - bias > d) ? 0.0 : 1.0;
        }
    }
    return sum / 9.0;
}

vec3 toLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
vec3 tonemap(vec3 c) { return c / (c + vec3(0.55)) * 1.55; }

void main() {
    vec3 base = toLinear(vColor.rgb);
    float alpha = vColor.a;
    if (vParams.z > 0.5) {
        vec4 texel = texture(uTexture, vUV);
        base *= toLinear(texel.rgb);
        // Native MeshPart.TextureID ignores the image alpha channel; the
        // texture alpha is only a SpecialMesh/FileMesh feature.  Transparent
        // texels therefore contribute their RGB (usually black), matching the
        // native renderer instead of making the face disappear.
        if (vParams.w > 0.5)
            alpha *= texel.a;
    }
    vec3 n = normalize(vNormal);
    vec3 l = normalize(uLightDir);
    vec3 v = normalize(uViewPos - vWorld);
    if (!gl_FrontFacing)
        n = -n;

    float ndl = max(dot(n, l), 0.0);
    float wrap = ndl * 0.5 + 0.5;
    float hemi = 0.5 + 0.5 * n.y;
    vec3 ambient = mix(toLinear(uGroundColor), toLinear(uSkyColor), hemi) * 1.05;
    vec3 sun = toLinear(uLightColor);

    float shadow = shadowFactor(n, l);
    float sunVis = mix(1.0, shadow, uShadowStrength);

    vec3 diffuse = sun * (ndl * 0.95 + wrap * 0.22) * mix(0.30, 1.0, sunVis);
    vec3 h = normalize(l + v);
    float spec = pow(max(dot(n, h), 0.0), 72.0) * vParams.y;
    float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0) * 0.10;

    vec3 lit = base * (ambient + diffuse) + sun * spec * mix(0.15, 1.0, sunVis) +
               toLinear(uSkyColor) * rim;
    lit = mix(lit, base * 1.15, clamp(vParams.x, 0.0, 1.0));

    float dist = length(uViewPos - vWorld);
    float fog = 1.0 - exp(-uFogDensity * uFogDensity * dist * dist);
    vec3 outColor = mix(tonemap(lit), tonemap(toLinear(uFogColor)),
                        clamp(fog, 0.0, 0.88));
    fragColor = vec4(pow(max(outColor, vec3(0.0)), vec3(1.0 / 2.2)), alpha);
}
)GLSL";

const char* kShadowVS = R"GLSL(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 3) in vec4 aM0;
layout(location = 4) in vec4 aM1;
layout(location = 5) in vec4 aM2;
layout(location = 6) in vec4 aM3;
uniform mat4 uLightVP;
void main() {
    gl_Position = uLightVP * (mat4(aM0, aM1, aM2, aM3) * vec4(aPos, 1.0));
}
)GLSL";

const char* kShadowFS = R"GLSL(#version 330 core
void main() {}
)GLSL";

const char* kSkyVS = R"GLSL(#version 330 core
out vec2 vNdc;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vNdc = p * 2.0 - 1.0;
    gl_Position = vec4(vNdc, 1.0, 1.0);
}
)GLSL";

const char* kSkyFS = R"GLSL(#version 330 core
in vec2 vNdc;
out vec4 fragColor;
uniform mat4 uInvViewProj;
uniform vec3 uCamPos;
uniform vec3 uSunDir;

vec3 skyColor(vec3 d) {
    float t = clamp(d.y, 0.0, 1.0);
    vec3 zenith = vec3(0.055, 0.175, 0.470);
    vec3 mid = vec3(0.220, 0.440, 0.780);
    vec3 horizon = vec3(0.720, 0.840, 0.950);
    vec3 c = mix(horizon, mid, smoothstep(0.0, 0.28, t));
    c = mix(c, zenith, smoothstep(0.20, 0.95, t));
    // The official sky is brighter and slightly warm near the horizon.
    c = mix(c, vec3(0.86, 0.89, 0.92), smoothstep(0.05, -0.10, d.y) * 0.55);
    float sd = max(dot(normalize(d), normalize(uSunDir)), 0.0);
    c += vec3(1.00, 0.86, 0.62) * pow(sd, 800.0) * 8.0;
    c += vec3(1.00, 0.82, 0.55) * pow(sd, 18.0) * 0.18;
    return c;
}

void main() {
    vec4 far = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 d = normalize(far.xyz / far.w - uCamPos);
    fragColor = vec4(skyColor(d), 1.0);
}
)GLSL";

// ---- GL helpers ------------------------------------------------------------

GLuint compile(GLenum type, const char* src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        glGetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
        std::fprintf(stderr, "view: shader compile failed: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint linkProgram(const char* vsSrc, const char* fsSrc) {
    const GLuint vs = compile(GL_VERTEX_SHADER, vsSrc);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs) {
        if (vs)
            glDeleteShader(vs);
        if (fs)
            glDeleteShader(fs);
        return 0;
    }
    const GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {0};
        glGetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
        std::fprintf(stderr, "view: program link failed: %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

// One instance = model matrix + sRGB colour/alpha + material params.
struct Instance {
    float m[16];
    float color[4];
    float params[4]; // x = emissive, y = specular
};
static_assert(sizeof(Instance) == 96, "instance layout must stay packed");

struct GpuMesh {
    GLuint vao = 0, vbo = 0, ibo = 0;
    GLsizei count = 0;
};

GpuMesh uploadMesh(const MeshData& md, GLuint instanceVbo, bool withInstances) {
    GpuMesh g;
    g.count = GLsizei(md.idx.size());
    glGenVertexArrays(1, &g.vao);
    glBindVertexArray(g.vao);

    glGenBuffers(1, &g.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(md.v.size() * sizeof(float)),
                 md.v.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float),
                          (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float),
                          (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float),
                          (void*)(6 * sizeof(float)));

    if (withInstances) {
        glBindBuffer(GL_ARRAY_BUFFER, instanceVbo);
        for (int i = 0; i < 4; ++i) {
            glEnableVertexAttribArray(3 + i);
            glVertexAttribPointer(3 + i, 4, GL_FLOAT, GL_FALSE,
                                  sizeof(Instance),
                                  (void*)(size_t(i) * 16));
            glVertexAttribDivisor(3 + i, 1);
        }
        glEnableVertexAttribArray(7);
        glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, sizeof(Instance),
                              (void*)(64));
        glVertexAttribDivisor(7, 1);
        glEnableVertexAttribArray(8);
        glVertexAttribPointer(8, 4, GL_FLOAT, GL_FALSE, sizeof(Instance),
                              (void*)(80));
        glVertexAttribDivisor(8, 1);
    }

    glGenBuffers(1, &g.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 GLsizeiptr(md.idx.size() * sizeof(unsigned)), md.idx.data(),
                 GL_STATIC_DRAW);
    glBindVertexArray(0);
    return g;
}

// Upload RGBA and generate the mip chain; MeshPart textures are minified on
// angled faces and native filtering would otherwise alias badly.
GLuint uploadTexture(const std::vector<uint8_t>& bytes) {
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(
        bytes.data(), int(bytes.size()), &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels)
            stbi_image_free(pixels);
        return 0;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    stbi_image_free(pixels);
    return tex;
}

std::string meshKey(const ViewBox& box) {
    return std::to_string(box.mesh_mode) + "|" + box.mesh_uri + "|" +
           box.texture_uri;
}

// ---- material response -----------------------------------------------------

struct Look {
    float emissive = 0.0f;
    float specular = 0.18f;
    float alpha = 1.0f;
};

Look lookFor(const ViewBox& b) {
    Look l;
    switch (b.material) {
    case 288: // Neon
        l.emissive = 0.92f;
        l.specular = 0.0f;
        break;
    case 1568: // Glass
        l.alpha = std::clamp(0.20f + float(1.0 - b.transparency) * 0.24f, 0.14f,
                             0.60f);
        l.specular = 0.9f;
        break;
    case 1584: // ForceField
        l.alpha = std::clamp(0.27f + float(1.0 - b.transparency) * 0.28f, 0.18f,
                             0.60f);
        l.emissive = 0.45f;
        l.specular = 0.3f;
        break;
    case 1088: // Metal
    case 1040: // CorrodedMetal
    case 1056: // DiamondPlate
    case 1072: // Foil
        l.specular = 0.55f + float(b.reflectance) * 0.45f;
        break;
    case 2048: // Water (parts painted water, not terrain)
        l.alpha = std::clamp(0.47f + float(1.0 - b.transparency) * 0.24f, 0.24f,
                             0.75f);
        l.specular = 0.8f;
        break;
    default:
        l.specular = 0.12f + float(b.reflectance) * 0.7f;
        break;
    }
    if (b.transparency > 0.005)
        l.alpha = std::min(l.alpha, float(std::max(0.0, 1.0 - b.transparency)));
    return l;
}

Instance makeInstance(const ViewBox& b) {
    const Look look = lookFor(b);
    Instance in{};
    const double sx = b.mesh_mode == 2 ? b.msx : b.sx;
    const double sy = b.mesh_mode == 2 ? b.msy : b.sy;
    const double sz = b.mesh_mode == 2 ? b.msz : b.sz;
    // SpecialMesh.Offset is in the part's local frame.
    const double tx = b.px + b.r00 * b.mox + b.r01 * b.moy + b.r02 * b.moz;
    const double ty = b.py + b.r10 * b.mox + b.r11 * b.moy + b.r12 * b.moz;
    const double tz = b.pz + b.r20 * b.mox + b.r21 * b.moy + b.r22 * b.moz;
    // model = T * R * S; column-major, so column i is R's i-th column times
    // the i-th size component.
    in.m[0] = float(b.r00 * sx);
    in.m[1] = float(b.r10 * sx);
    in.m[2] = float(b.r20 * sx);
    in.m[3] = 0.0f;
    in.m[4] = float(b.r01 * sy);
    in.m[5] = float(b.r11 * sy);
    in.m[6] = float(b.r21 * sy);
    in.m[7] = 0.0f;
    in.m[8] = float(b.r02 * sz);
    in.m[9] = float(b.r12 * sz);
    in.m[10] = float(b.r22 * sz);
    in.m[11] = 0.0f;
    in.m[12] = float(tx);
    in.m[13] = float(ty);
    in.m[14] = float(tz);
    in.m[15] = 1.0f;
    in.color[0] = float(std::clamp(b.r, 0.0, 1.0));
    in.color[1] = float(std::clamp(b.g, 0.0, 1.0));
    in.color[2] = float(std::clamp(b.b, 0.0, 1.0));
    in.color[3] = look.alpha;
    in.params[0] = look.emissive;
    in.params[1] = look.specular;
    return in;
}

// ---- PNG (zlib) ------------------------------------------------------------

void putBE32(std::vector<unsigned char>& out, uint32_t v) {
    out.push_back((unsigned char)(v >> 24));
    out.push_back((unsigned char)(v >> 16));
    out.push_back((unsigned char)(v >> 8));
    out.push_back((unsigned char)v);
}

bool writePng(const std::string& path, int w, int h,
              const std::vector<unsigned char>& rgba) {
    // rgba is top-down, RGBA8.
    std::vector<unsigned char> raw;
    raw.reserve(size_t(h) * (size_t(w) * 4 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0); // filter: none
        const unsigned char* row = rgba.data() + size_t(y) * size_t(w) * 4;
        raw.insert(raw.end(), row, row + size_t(w) * 4);
    }
    uLongf compLen = compressBound((uLong)raw.size());
    std::vector<unsigned char> comp(compLen);
    if (compress2(comp.data(), &compLen, raw.data(), (uLong)raw.size(), 6) !=
        Z_OK)
        return false;
    comp.resize(compLen);

    std::vector<unsigned char> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A,
                                      0x0A};
    auto chunk = [&](const char* type, const unsigned char* data, size_t n) {
        putBE32(png, (uint32_t)n);
        const size_t start = png.size();
        png.insert(png.end(), type, type + 4);
        if (n)
            png.insert(png.end(), data, data + n);
        const uLong crc = crc32(0L, png.data() + start, (uInt)(png.size() - start));
        putBE32(png, (uint32_t)crc);
    };
    unsigned char ihdr[13];
    auto be32 = [](unsigned char* p, uint32_t v) {
        p[0] = (unsigned char)(v >> 24);
        p[1] = (unsigned char)(v >> 16);
        p[2] = (unsigned char)(v >> 8);
        p[3] = (unsigned char)v;
    };
    be32(ihdr + 0, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 6;  // colour type: RGBA
    ihdr[10] = 0; // deflate
    ihdr[11] = 0; // adaptive filtering
    ihdr[12] = 0; // no interlace
    chunk("IHDR", ihdr, sizeof(ihdr));
    chunk("IDAT", comp.data(), comp.size());
    chunk("IEND", nullptr, 0);

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    std::fclose(f);
    return ok;
}

// ---- renderer --------------------------------------------------------------

enum ShapeIndex { kBall = 0, kBlock = 1, kCylinder = 2, kWedge = 3, kCorner = 4,
                  kShapeCount = 5 };

struct Plane {
    float nx, ny, nz, d;
};

struct Renderer {
    SDL_Window* window = nullptr;
    SDL_GLContext gl = nullptr;
    int fbW = 0, fbH = 0;
    bool msaa = true;

    GLuint sceneProg = 0, shadowProg = 0, skyProg = 0;
    GLint uViewProj = -1, uLightVP = -1, uViewPos = -1, uLightDir = -1,
          uLightColor = -1, uSkyColor = -1, uGroundColor = -1, uFogColor = -1,
          uFogDensity = -1, uShadow = -1, uShadowTexel = -1,
          uShadowStrength = -1, uTexture = -1;
    GLint uSkyInvVP = -1, uSkyCam = -1, uSkySun = -1;

    GLuint instanceVbo = 0;
    GpuMesh meshes[kShapeCount];
    GLuint emptyVao = 0;

    struct GpuAsset {
        GpuMesh mesh;
        GLuint texture = 0;
        bool valid = false;
    };
    std::unordered_map<std::string, GpuAsset> assets;
    std::string asset_cookie;

    GLuint msFbo = 0, msColor = 0, msDepth = 0;
    GLuint resolveFbo = 0, resolveTex = 0;

    GLuint shadowFbo = 0, shadowTex = 0;
    static constexpr int kShadowSize = 2048;
    GuiRenderer gui;

    std::vector<Instance> batches[kShapeCount];

    bool init(int width, int height, std::string* err,
              const std::string& cookie);
    void shutdown();
    bool resize(int w, int h);
    void draw(const std::vector<ViewBox>& boxes, const CameraFocus& focus,
              V3 camPos, V3 camTarget, bool haveWorld, float fovY);
    GpuAsset* asset_for(const ViewBox& box);
    void drawGui(const std::vector<GuiDrawCommand>& commands);
    bool capture(const std::string& path, std::string* err);
};

Renderer::GpuAsset* Renderer::asset_for(const ViewBox& box) {
    if (box.mesh_uri.empty())
        return nullptr;
    const std::string key = meshKey(box);
    const auto found = assets.find(key);
    if (found != assets.end())
        return found->second.valid ? &found->second : nullptr;

    GpuAsset asset;
    std::vector<uint8_t> bytes;
    std::string error;
    if (!fetch_asset(box.mesh_uri, asset_cookie, &bytes, &error)) {
        std::fprintf(stderr, "view: mesh %s: %s\n", box.mesh_uri.c_str(),
                     error.c_str());
        assets.emplace(key, std::move(asset));
        return nullptr;
    }
    MeshAsset mesh;
    if (!parse_mesh(bytes.data(), bytes.size(), &mesh, &error)) {
        std::fprintf(stderr, "view: mesh %s: %s\n", box.mesh_uri.c_str(),
                     error.c_str());
        assets.emplace(key, std::move(asset));
        return nullptr;
    }
    if (box.mesh_mode == 1) {
        // MeshPart: Size is the authoritative bounding box, so normalise the
        // file to a unit cube before the instance matrix applies Size.
        const float extent[3] = {
            std::max(mesh.max[0] - mesh.min[0], 1e-6f),
            std::max(mesh.max[1] - mesh.min[1], 1e-6f),
            std::max(mesh.max[2] - mesh.min[2], 1e-6f)};
        const float center[3] = {(mesh.min[0] + mesh.max[0]) * 0.5f,
                                 (mesh.min[1] + mesh.max[1]) * 0.5f,
                                 (mesh.min[2] + mesh.max[2]) * 0.5f};
        for (size_t i = 0; i < mesh.vertices.size(); i += 8) {
            for (int c = 0; c < 3; ++c)
                mesh.vertices[i + c] =
                    (mesh.vertices[i + c] - center[c]) / extent[c];
        }
    }
    MeshData data;
    data.v = std::move(mesh.vertices);
    data.idx = std::move(mesh.indices);
    asset.mesh = uploadMesh(data, instanceVbo, true);
    if (!box.texture_uri.empty()) {
        std::vector<uint8_t> texture_bytes;
        if (fetch_asset(box.texture_uri, asset_cookie, &texture_bytes, &error))
            asset.texture = uploadTexture(texture_bytes);
        else
            std::fprintf(stderr, "view: texture %s: %s\n",
                         box.texture_uri.c_str(), error.c_str());
    }
    asset.valid = asset.mesh.count > 0;
    if (asset.valid && std::getenv("RBX_VIEW_DEBUG"))
        std::fprintf(stderr, "view: mesh loaded %s tris=%u tex=%u\n",
                     box.mesh_uri.c_str(),
                     unsigned(asset.mesh.count / 3), asset.texture);
    const auto inserted = assets.emplace(key, std::move(asset));
    return inserted.first->second.valid ? &inserted.first->second : nullptr;
}

bool Renderer::init(int width, int height, std::string* err,
                    const std::string& cookie) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

    window = SDL_CreateWindow("Boblox", width, height,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) {
        if (err)
            *err = std::string("SDL window failed (no display? use "
                               "--headless): ") + SDL_GetError();
        return false;
    }
    gl = SDL_GL_CreateContext(window);
    if (!gl) {
        if (err)
            *err = std::string("GL context failed: ") + SDL_GetError();
        return false;
    }
    SDL_GL_SetSwapInterval(1);

    std::printf("view: %s | %s\n", glGetString(GL_RENDERER),
                glGetString(GL_VERSION));
    std::string gui_err;
    if (!gui.init(&gui_err, cookie))
        std::fprintf(stderr, "view: GUI disabled: %s\n", gui_err.c_str());

    sceneProg = linkProgram(kSceneVS, kSceneFS);
    shadowProg = linkProgram(kShadowVS, kShadowFS);
    skyProg = linkProgram(kSkyVS, kSkyFS);
    if (!sceneProg || !shadowProg || !skyProg) {
        if (err)
            *err = "lighting shader failed to compile";
        return false;
    }
    uViewProj = glGetUniformLocation(sceneProg, "uViewProj");
    uLightVP = glGetUniformLocation(sceneProg, "uLightVP");
    uViewPos = glGetUniformLocation(sceneProg, "uViewPos");
    uLightDir = glGetUniformLocation(sceneProg, "uLightDir");
    uLightColor = glGetUniformLocation(sceneProg, "uLightColor");
    uSkyColor = glGetUniformLocation(sceneProg, "uSkyColor");
    uGroundColor = glGetUniformLocation(sceneProg, "uGroundColor");
    uFogColor = glGetUniformLocation(sceneProg, "uFogColor");
    uFogDensity = glGetUniformLocation(sceneProg, "uFogDensity");
    uShadow = glGetUniformLocation(sceneProg, "uShadow");
    uTexture = glGetUniformLocation(sceneProg, "uTexture");
    uShadowTexel = glGetUniformLocation(sceneProg, "uShadowTexel");
    uShadowStrength = glGetUniformLocation(sceneProg, "uShadowStrength");
    uSkyInvVP = glGetUniformLocation(skyProg, "uInvViewProj");
    uSkyCam = glGetUniformLocation(skyProg, "uCamPos");
    uSkySun = glGetUniformLocation(skyProg, "uSunDir");

    glGenBuffers(1, &instanceVbo);
    asset_cookie = cookie;
    const MeshData data[kShapeCount] = {sphereMesh(), boxMesh(),
                                        cylinderXMesh(), wedgeMesh(),
                                        cornerWedgeMesh()};
    for (int i = 0; i < kShapeCount; ++i)
        meshes[i] = uploadMesh(data[i], instanceVbo, true);
    glGenVertexArrays(1, &emptyVao);

    // Shadow map: depth-only 2048², clamped-to-border white so off-map reads
    // count as "lit" instead of "in shadow".
    glGenTextures(1, &shadowTex);
    glBindTexture(GL_TEXTURE_2D, shadowTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, kShadowSize,
                 kShadowSize, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const float border[4] = {1, 1, 1, 1};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
    glGenFramebuffers(1, &shadowFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                           shadowTex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        if (err)
            *err = "shadow framebuffer incomplete";
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    return resize(width, height);
}

bool Renderer::resize(int w, int h) {
    if (w <= 0 || h <= 0)
        return true;
    if (w == fbW && h == fbH && msFbo)
        return true;
    fbW = w;
    fbH = h;

    const GLint samples = msaa ? 4 : 0;
    if (!msFbo) {
        glGenFramebuffers(1, &msFbo);
        glGenRenderbuffers(1, &msColor);
        glGenRenderbuffers(1, &msDepth);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, msFbo);
    glBindRenderbuffer(GL_RENDERBUFFER, msColor);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, msColor);
    glBindRenderbuffer(GL_RENDERBUFFER, msDepth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples,
                                     GL_DEPTH_COMPONENT24, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, msDepth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        // Fall back to a plain single-sample target instead of failing the
        // whole window on drivers with a low sample limit.
        msaa = false;
        glBindRenderbuffer(GL_RENDERBUFFER, msColor);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, 0, GL_RGBA8, w, h);
        glBindRenderbuffer(GL_RENDERBUFFER, msDepth);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, 0,
                                         GL_DEPTH_COMPONENT24, w, h);
    }

    if (!resolveFbo) {
        glGenFramebuffers(1, &resolveFbo);
        glGenTextures(1, &resolveTex);
    }
    glBindTexture(GL_TEXTURE_2D, resolveTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, resolveFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           resolveTex, 0);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
                    GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return ok;
}

void Renderer::drawGui(const std::vector<GuiDrawCommand>& commands) {
    if (!commands.empty())
        gui.draw(commands, fbW, fbH);
}

void Renderer::shutdown() {
    gui.shutdown();
    for (GpuMesh& m : meshes) {
        if (m.vao)
            glDeleteVertexArrays(1, &m.vao);
        if (m.vbo)
            glDeleteBuffers(1, &m.vbo);
        if (m.ibo)
            glDeleteBuffers(1, &m.ibo);
    }
    for (auto& entry : assets) {
        GpuAsset& asset = entry.second;
        if (asset.mesh.vao)
            glDeleteVertexArrays(1, &asset.mesh.vao);
        if (asset.mesh.vbo)
            glDeleteBuffers(1, &asset.mesh.vbo);
        if (asset.mesh.ibo)
            glDeleteBuffers(1, &asset.mesh.ibo);
        if (asset.texture)
            glDeleteTextures(1, &asset.texture);
    }
    assets.clear();
    if (instanceVbo)
        glDeleteBuffers(1, &instanceVbo);
    if (emptyVao)
        glDeleteVertexArrays(1, &emptyVao);
    if (sceneProg)
        glDeleteProgram(sceneProg);
    if (shadowProg)
        glDeleteProgram(shadowProg);
    if (skyProg)
        glDeleteProgram(skyProg);
    if (shadowFbo)
        glDeleteFramebuffers(1, &shadowFbo);
    if (shadowTex)
        glDeleteTextures(1, &shadowTex);
    if (msFbo)
        glDeleteFramebuffers(1, &msFbo);
    if (resolveFbo)
        glDeleteFramebuffers(1, &resolveFbo);
    if (resolveTex)
        glDeleteTextures(1, &resolveTex);
    if (msColor)
        glDeleteRenderbuffers(1, &msColor);
    if (msDepth)
        glDeleteRenderbuffers(1, &msDepth);
    if (gl)
        SDL_GL_DestroyContext(gl);
    if (window)
        SDL_DestroyWindow(window);
}

void Renderer::draw(const std::vector<ViewBox>& boxes, const CameraFocus& focus,
                    V3 camPos, V3 camTarget, bool haveWorld, float fovY) {
    // ---- camera matrices ----
    const float aspect = float(fbW) / float(std::max(fbH, 1));
    const M4 view = lookAt(camPos, camTarget, V3{0, 1, 0});
    const M4 proj = perspective(fovY, aspect, 0.5f, 20000.0f);
    const M4 viewProj = mul(proj, view);
    M4 invViewProj;
    const bool haveInv = invert(viewProj, invViewProj);

    const V3 lightDir =
        normalize(V3{-0.45f, 0.82f, 0.35f}); // late-afternoon key light
    const V3 lightColor{1.10f, 1.04f, 0.92f};
    const V3 skyColor{0.46f, 0.57f, 0.72f};
    const V3 groundColor{0.20f, 0.18f, 0.16f};
    const V3 fogColor{0.72f, 0.82f, 0.94f};
    const float fogDensity = 0.0011f;

    // ---- cull + bucket ----
    // Six frustum planes from the combined matrix (Gribb/Hartmann). The
    // matrix is column-major, so row r is (m[r], m[4+r], m[8+r], m[12+r]);
    // indexing by columns here silently culls the world at close range.
    Plane planes[6];
    for (int i = 0; i < 3; ++i) {
        planes[i * 2 + 0] = {viewProj.m[3] + viewProj.m[i],
                             viewProj.m[7] + viewProj.m[4 + i],
                             viewProj.m[11] + viewProj.m[8 + i],
                             viewProj.m[15] + viewProj.m[12 + i]};
        planes[i * 2 + 1] = {viewProj.m[3] - viewProj.m[i],
                             viewProj.m[7] - viewProj.m[4 + i],
                             viewProj.m[11] - viewProj.m[8 + i],
                             viewProj.m[15] - viewProj.m[12 + i]};
    }
    for (Plane& p : planes) {
        const float l = std::sqrt(p.nx * p.nx + p.ny * p.ny + p.nz * p.nz);
        if (l > 1e-8f) {
            p.nx /= l;
            p.ny /= l;
            p.nz /= l;
            p.d /= l;
        }
    }
    auto visible = [&](const ViewBox& b, float radius) {
        for (const Plane& p : planes) {
            if (p.nx * float(b.px) + p.ny * float(b.py) + p.nz * float(b.pz) +
                    p.d <
                -radius)
                return false;
        }
        return true;
    };

    struct Item {
        const ViewBox* b;
        float dist2;
    };
    std::vector<Item> opaque, alpha;
    opaque.reserve(boxes.size());
    alpha.reserve(64);
    for (const ViewBox& b : boxes) {
        float radius =
            0.5f * std::sqrt(float(b.sx * b.sx + b.sy * b.sy + b.sz * b.sz));
        if (b.mesh_mode == 2) {
            // SpecialMesh Size is collision-only; cull against the rendered
            // Scale so large file meshes do not pop out at the frustum edge.
            const float mesh_radius = 0.5f * std::sqrt(
                float(b.msx * b.msx + b.msy * b.msy + b.msz * b.msz));
            radius = std::max(radius, mesh_radius);
        }
        if (!visible(b, radius))
            continue;
        const float dx = float(b.px) - camPos.x;
        const float dy = float(b.py) - camPos.y;
        const float dz = float(b.pz) - camPos.z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (b.is_transparent())
            alpha.push_back({&b, d2});
        else
            opaque.push_back({&b, d2});
    }
    std::sort(alpha.begin(), alpha.end(),
              [](const Item& a, const Item& b) { return a.dist2 > b.dist2; });

    for (auto& b : batches)
        b.clear();

    // MeshPart/SpecialMesh instances are grouped by their (mesh, texture)
    // asset pair: each group has its own VAO and sampler, while the instance
    // VBO still carries model matrices for the whole group.
    std::map<std::string, std::vector<Instance>> meshOpaque, meshAlpha;
    std::map<std::string, GpuAsset*> meshAssets;

    auto addMesh = [&](const Item& it, bool transparent) -> bool {
        GpuAsset* asset = asset_for(*it.b);
        if (!asset)
            return false;
        const std::string key = meshKey(*it.b);
        meshAssets[key] = asset;
        Instance in = makeInstance(*it.b);
        in.params[2] = asset->texture ? 1.0f : 0.0f;
        in.params[3] = it.b->mesh_mode == 2 ? 1.0f : 0.0f;
        (transparent ? meshAlpha : meshOpaque)[key].push_back(in);
        return true;
    };

    auto shapeOf = [](const ViewBox& b) {
        return (b.shape >= 0 && b.shape < kShapeCount) ? b.shape : kBlock;
    };
    for (const Item& it : opaque) {
        if (!it.b->mesh_uri.empty() && addMesh(it, false))
            continue;
        batches[shapeOf(*it.b)].push_back(makeInstance(*it.b));
    }
    // Transparent order matters, so fill shape buckets in sorted order: the
    // global sort survives across shapes as well as within one.
    for (const Item& it : alpha) {
        if (!it.b->mesh_uri.empty() && addMesh(it, true))
            continue;
        batches[shapeOf(*it.b)].push_back(makeInstance(*it.b));
    }
    // Opaque entries were appended first, so each shape's slice is
    // [0, opaqueCount) opaque and [opaqueCount, +alphaCount) sorted alpha.
    size_t opaqueCount[kShapeCount] = {0, 0, 0, 0, 0};
    for (const Item& it : opaque)
        opaqueCount[shapeOf(*it.b)]++;
    size_t alphaCount[kShapeCount] = {0, 0, 0, 0, 0};
    for (const Item& it : alpha)
        alphaCount[shapeOf(*it.b)]++;

    // ---- shadow pass ----
    const float shadowRadius = std::clamp(
        float(focus.radius) * 1.6f + 12.0f, 24.0f, 320.0f);
    const V3 shadowCenter = camTarget;
    const V3 lightEye = shadowCenter - lightDir * (shadowRadius * 2.0f);
    const M4 lightView = lookAt(lightEye, shadowCenter, V3{0, 1, 0});
    const M4 lightProj =
        ortho(-shadowRadius, shadowRadius, -shadowRadius, shadowRadius, 1.0f,
              shadowRadius * 4.0f);
    const M4 lightVP = mul(lightProj, lightView);

    glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo);
    glViewport(0, 0, kShadowSize, kShadowSize);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    glDisable(GL_BLEND);
    glUseProgram(shadowProg);
    glUniformMatrix4fv(glGetUniformLocation(shadowProg, "uLightVP"), 1,
                       GL_FALSE, lightVP.m);
    for (int s = 0; s < kShapeCount; ++s) {
        const size_t n = opaqueCount[s];
        if (!n)
            continue;
        glBindVertexArray(meshes[s].vao);
        glBindBuffer(GL_ARRAY_BUFFER, instanceVbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(n * sizeof(Instance)),
                     batches[s].data(), GL_STREAM_DRAW);
        glDrawElementsInstanced(GL_TRIANGLES, meshes[s].count, GL_UNSIGNED_INT,
                                nullptr, GLsizei(n));
    }
    for (const auto& group : meshOpaque) {
        GpuAsset* asset = meshAssets[group.first];
        const size_t n = group.second.size();
        if (!asset || !n)
            continue;
        glBindVertexArray(asset->mesh.vao);
        glBindBuffer(GL_ARRAY_BUFFER, instanceVbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(n * sizeof(Instance)),
                     group.second.data(), GL_STREAM_DRAW);
        glDrawElementsInstanced(GL_TRIANGLES, asset->mesh.count,
                                GL_UNSIGNED_INT, nullptr, GLsizei(n));
    }
    glBindVertexArray(0);

    // ---- main pass ----
    glBindFramebuffer(GL_FRAMEBUFFER, msFbo);
    glViewport(0, 0, fbW, fbH);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.72f, 0.84f, 0.95f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_CULL_FACE);

    if (haveInv) {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glUseProgram(skyProg);
        glUniformMatrix4fv(uSkyInvVP, 1, GL_FALSE, invViewProj.m);
        glUniform3f(uSkyCam, camPos.x, camPos.y, camPos.z);
        glUniform3f(uSkySun, lightDir.x, lightDir.y, lightDir.z);
        glBindVertexArray(emptyVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
    }

    glUseProgram(sceneProg);
    glUniformMatrix4fv(uViewProj, 1, GL_FALSE, viewProj.m);
    glUniformMatrix4fv(uLightVP, 1, GL_FALSE, lightVP.m);
    glUniform3f(uViewPos, camPos.x, camPos.y, camPos.z);
    glUniform3f(uLightDir, lightDir.x, lightDir.y, lightDir.z);
    glUniform3f(uLightColor, lightColor.x, lightColor.y, lightColor.z);
    glUniform3f(uSkyColor, skyColor.x, skyColor.y, skyColor.z);
    glUniform3f(uGroundColor, groundColor.x, groundColor.y, groundColor.z);
    glUniform3f(uFogColor, fogColor.x, fogColor.y, fogColor.z);
    glUniform1f(uFogDensity, fogDensity);
    glUniform1f(uShadowTexel, 1.0f / float(kShadowSize));
    glUniform1f(uShadowStrength, haveWorld ? 1.0f : 0.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, shadowTex);
    glUniform1i(uShadow, 0);
    glUniform1i(uTexture, 1);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    auto drawRange = [&](int s, size_t offset, size_t count) {
        if (!count)
            return;
        glBindVertexArray(meshes[s].vao);
        glBindBuffer(GL_ARRAY_BUFFER, instanceVbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(count * sizeof(Instance)),
                     batches[s].data() + offset, GL_STREAM_DRAW);
        glDrawElementsInstanced(GL_TRIANGLES, meshes[s].count, GL_UNSIGNED_INT,
                                nullptr, GLsizei(count));
    };
    for (int s = 0; s < kShapeCount; ++s)
        drawRange(s, 0, opaqueCount[s]);

    glActiveTexture(GL_TEXTURE1);
    for (const auto& group : meshOpaque) {
        GpuAsset* asset = meshAssets[group.first];
        const size_t n = group.second.size();
        if (!asset || !n)
            continue;
        glBindTexture(GL_TEXTURE_2D, asset->texture);
        glBindVertexArray(asset->mesh.vao);
        glBindBuffer(GL_ARRAY_BUFFER, instanceVbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(n * sizeof(Instance)),
                     group.second.data(), GL_STREAM_DRAW);
        glDrawElementsInstanced(GL_TRIANGLES, asset->mesh.count,
                                GL_UNSIGNED_INT, nullptr, GLsizei(n));
    }

    // Transparent: back-to-front, depth writes off. Each shape's slice is
    // already ordered because the global sort filled the buckets.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    for (int s = 0; s < kShapeCount; ++s)
        drawRange(s, opaqueCount[s], alphaCount[s]);
    for (const auto& group : meshAlpha) {
        GpuAsset* asset = meshAssets[group.first];
        const size_t n = group.second.size();
        if (!asset || !n)
            continue;
        glBindTexture(GL_TEXTURE_2D, asset->texture);
        glBindVertexArray(asset->mesh.vao);
        glBindBuffer(GL_ARRAY_BUFFER, instanceVbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(n * sizeof(Instance)),
                     group.second.data(), GL_STREAM_DRAW);
        glDrawElementsInstanced(GL_TRIANGLES, asset->mesh.count,
                                GL_UNSIGNED_INT, nullptr, GLsizei(n));
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(0);
}

bool Renderer::capture(const std::string& path, std::string* err) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, msFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolveFbo);
    glBlitFramebuffer(0, 0, fbW, fbH, 0, 0, fbW, fbH, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, resolveFbo);
    std::vector<unsigned char> rgba(size_t(fbW) * size_t(fbH) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fbW, fbH, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (rgba.empty()) {
        if (err)
            *err = "screen capture failed (no frame data)";
        return false;
    }
    // GL rows are bottom-up; PNG wants top-down.
    std::vector<unsigned char> flipped(rgba.size());
    const size_t stride = size_t(fbW) * 4;
    for (int y = 0; y < fbH; ++y)
        std::memcpy(flipped.data() + size_t(y) * stride,
                    rgba.data() + size_t(fbH - 1 - y) * stride, stride);
    if (!writePng(path, fbW, fbH, flipped)) {
        if (err)
            *err = std::string("screenshot save failed: ") + path;
        return false;
    }
    return true;
}

// ---- drivers ---------------------------------------------------------------

int runViewerImpl(const std::function<std::vector<ViewBox>()>& boxFn,
                  const std::function<bool(double)>& tick,
                  const std::string& shot_path, std::string* err,
                  const std::string& cookie,
                  const std::function<bool(ViewCamera*)>& cameraFn,
                  const std::function<const rbx::Instance*()>& guiFn) {
    // A rejoin runs the viewer again in the same process. GTK/libdecor keeps
    // process-global state that SDL_Quit tears down without rebuilding it; a
    // later SDL_CreateWindow then segfaults in g_intern_static_string (seen
    // through libdecor-gtk). Keep SDL initialized for the process lifetime and
    // recreate only the window/GL context per session.
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_Init(SDL_INIT_VIDEO)) {
        if (err)
            *err = std::string("SDL init failed (no display? use "
                               "--headless): ") + SDL_GetError();
        return 2;
    }

    rbx::input::reset();
    // SDL owns the platform clipboard; the runtime only sees a neutral text
    // getter/setter so headless builds never link a windowing dependency.
    rbx::input::set_clipboard_bridge(
        [] {
            std::string out;
            if (SDL_HasClipboardText()) {
                char* text = SDL_GetClipboardText();
                if (text) {
                    out = text;
                    SDL_free(text);
                }
            }
            return out;
        },
        [](const std::string& text) { SDL_SetClipboardText(text.c_str()); });

    Renderer r;
    if (!r.init(1280, 720, err, cookie)) {
        r.shutdown();
        return 2;
    }

    float yaw = kPi * 0.25f;
    float pitch = 0.42f;
    float zoom = 1.0f;
    bool framed = false;
    bool was_avatar = false;
    bool had_world = false;
    V3 camPos{}, camTarget{};

    int frames = 0;
    int warmup = shot_path.empty() ? -1 : 120;
    if (const char* env = std::getenv("RBX_SHOT_WARMUP"))
        warmup = std::max(1, std::atoi(env));
    // --shot normally captures two seconds after the world appears, which is
    // before the RCC spawns the local avatar. RBX_SHOT_AVATAR=1 holds the
    // capture until the local character is linked and framed.
    const bool shot_wants_avatar = std::getenv("RBX_SHOT_AVATAR") != nullptr;
    bool shot_saved = false;
    bool running = true;
    Uint64 lastTick = SDL_GetTicks();
    Uint64 lastTitle = 0;
    float mouseLastX = 0, mouseLastY = 0;
    bool mouseTracked = false;
    float pointerX = 0, pointerY = 0;
    SDL_MouseButtonFlags pointerButtons = 0;
    SDL_GetMouseState(&pointerX, &pointerY);
    SDL_Cursor* cursor_arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    SDL_Cursor* cursor_hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    SDL_Cursor* cursor_ibeam = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
    rbx::gui::Cursor last_cursor = rbx::gui::Cursor::Arrow;
    bool finger_down = false;
    float finger_x = 0.0f, finger_y = 0.0f;
    SDL_StartTextInput(r.window);
    // A deterministic driver (RBX_GUI_TEST) owns the input queue; the desktop
    // pointer must not fake a release between its synthetic down and up.
    const bool synthetic_input = rbx::input::test_mode();

    auto publish_pointer = [&]() {
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(r.window, &pw, &ph);
        rbx::input::publish_mouse(
            pointerX, pointerY,
            (pointerButtons & SDL_BUTTON_MASK(SDL_BUTTON_LEFT)) != 0,
            (pointerButtons & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)) != 0,
            (pointerButtons & SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE)) != 0, pw,
            ph);
    };

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT)
                running = false;
            else if (ev.type == SDL_EVENT_KEY_DOWN &&
                     ev.key.key == SDLK_ESCAPE)
                running = false;
            else if (ev.type == SDL_EVENT_KEY_DOWN ||
                     ev.type == SDL_EVENT_KEY_UP) {
                const int key = roblox_keycode(ev.key.key);
                if (key != 0 && !synthetic_input)
                    rbx::input::publish_key(key,
                                            ev.type == SDL_EVENT_KEY_DOWN,
                                            ev.key.repeat);
            } else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                pointerX = ev.motion.x;
                pointerY = ev.motion.y;
                if (!synthetic_input)
                    publish_pointer();
            } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                       ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                // Publish the transition itself: a press and release between
                // two frames must not be lost to the per-frame state sample.
                const SDL_MouseButtonFlags mask =
                    SDL_BUTTON_MASK(Uint8(ev.button.button));
                if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                    pointerButtons |= mask;
                else
                    pointerButtons &= ~mask;
                pointerX = ev.button.x;
                pointerY = ev.button.y;
                if (!synthetic_input)
                    publish_pointer();
            } else if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
                if (!synthetic_input) {
                    rbx::input::publish_wheel(pointerX, pointerY, ev.wheel.x,
                                              ev.wheel.y);
                    // A wheel over a ScreenGui belongs to the GUI; the camera
                    // only zooms when no GUI object captured the pointer.
                    if (!rbx::gui::pointer_captured())
                        zoom = std::clamp(zoom - float(ev.wheel.y) * 0.12f,
                                          0.45f, 2.8f);
                }
            } else if (ev.type == SDL_EVENT_TEXT_INPUT) {
                if (!synthetic_input)
                    rbx::input::publish_text(ev.text.text ? ev.text.text : "");
            } else if (ev.type == SDL_EVENT_FINGER_DOWN) {
                int fw = 0, fh = 0;
                SDL_GetWindowSizeInPixels(r.window, &fw, &fh);
                finger_down = true;
                finger_x = ev.tfinger.x * fw;
                finger_y = ev.tfinger.y * fh;
            } else if (ev.type == SDL_EVENT_FINGER_UP) {
                if (finger_down) {
                    int fw = 0, fh = 0;
                    SDL_GetWindowSizeInPixels(r.window, &fw, &fh);
                    const float x = ev.tfinger.x * fw;
                    const float y = ev.tfinger.y * fh;
                    if (std::abs(x - finger_x) < 8.0f &&
                        std::abs(y - finger_y) < 8.0f && !synthetic_input)
                        rbx::input::publish_touch_tap(finger_x, finger_y);
                }
                finger_down = false;
            } else if (ev.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                if (!synthetic_input)
                    rbx::input::clear_keys();
            }
        }
        if (!running)
            break;

        int winW = 0, winH = 0;
        SDL_GetWindowSizeInPixels(r.window, &winW, &winH);
        if (!r.resize(winW, winH)) {
            if (err)
                *err = "framebuffer resize failed";
            break;
        }

        if (!synthetic_input) {
            pointerButtons = SDL_GetMouseState(&pointerX, &pointerY);
            rbx::input::publish_mouse(
                pointerX, pointerY,
                (pointerButtons & SDL_BUTTON_MASK(SDL_BUTTON_LEFT)) != 0,
                (pointerButtons & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)) != 0,
                (pointerButtons & SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE)) != 0,
                winW, winH);
        }

        const Uint64 now = SDL_GetTicks();
        const double dt = double(now - lastTick) / 1000.0;
        lastTick = now;
        if (tick && !tick(dt))
            break;

        const std::vector<ViewBox> boxes = boxFn();
        const CameraFocus focus = focusCamera(boxes);
        const bool have_world = !boxes.empty();
        ViewCamera viewCam;
        const bool use_view_cam =
            cameraFn && cameraFn(&viewCam) && viewCam.valid;
        if (std::getenv("RBX_VIEW_DEBUG") && frames < 2)
            std::printf("view: boxes=%zu avatar=%d focus=(%.2f,%.2f,%.2f) "
                        "r=%.2f\n",
                        boxes.size(), int(focus.has_avatar), focus.cx,
                        focus.cy, focus.cz, focus.radius);
        if (std::getenv("RBX_VIEW_DEBUG") && frames == 0) {
            int shown = 0;
            for (const auto& b : boxes) {
                if (!b.local_avatar || shown++ >= 24)
                    continue;
                std::printf("view: avatar %s p=(%.2f,%.2f,%.2f) "
                            "s=(%.2f,%.2f,%.2f) color=(%.2f,%.2f,%.2f) "
                            "tr=%.2f shape=%d mat=%d\n",
                            b.name.c_str(), b.px, b.py, b.pz, b.sx, b.sy,
                            b.sz, b.r, b.g, b.b, b.transparency, b.shape,
                            b.material);
            }
        }
        if (have_world) {
            // Re-frame when the first world arrives, and when the local
            // avatar appears after an overview (the official client snaps
            // behind the character on spawn).
            if (!framed || (focus.has_avatar && !was_avatar)) {
                yaw = focus.has_avatar ? std::atan2(float(focus.fx),
                                                    float(focus.fz))
                                       : kPi * 0.25f;
                pitch = focus.has_avatar ? 0.28f : 0.48f;
                was_avatar = focus.has_avatar;
                framed = true;
            }
        }

        // Mouse orbit / wheel zoom. Right or middle drag, exactly the
        // controls a viewer expects; left click stays free for GUI input.
        // A ScreenGui hover/focus owns the pointer, so look-drag cannot start
        // underneath a button or text field.
        if (have_world && !rbx::gui::pointer_captured()) {
            const bool dragging =
                (pointerButtons & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)) ||
                (pointerButtons & SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE));
            if (dragging && mouseTracked) {
                yaw -= (pointerX - mouseLastX) * 0.006f;
                pitch = std::clamp(
                    pitch + (pointerY - mouseLastY) * 0.005f, -0.10f, 1.15f);
            }
            mouseLastX = pointerX;
            mouseLastY = pointerY;
            mouseTracked = true;
        }

        V3 target{float(focus.cx), float(focus.cy), float(focus.cz)};
        if (focus.has_avatar)
            target.y += 1.7f;
        if (use_view_cam) {
            // The local player owns the camera once a Character exists.  Skip
            // the orbit/framing math entirely so the two cameras never fight
            // for the same frame.
            camPos = V3{float(viewCam.px), float(viewCam.py),
                        float(viewCam.pz)};
            camTarget = V3{float(viewCam.px + viewCam.lx),
                           float(viewCam.py + viewCam.ly),
                           float(viewCam.pz + viewCam.lz)};
            had_world = true;
            framed = true;
        }
        // Third-person distance is body-scale, never map-scale: one giant
        // hat/accessory must not push the camera a thousand studs away.
        float base_dist = focus.has_avatar
                              ? std::clamp(float(focus.radius) * 2.7f, 9.0f,
                                           18.0f)
                              : std::max(12.0f, float(focus.radius) * 2.2f);
        if (!focus.has_avatar && framed && was_avatar)
            base_dist = std::max(base_dist, 12.0f);
        const float dist = base_dist * zoom;
        const V3 forward{std::sin(yaw), 0.0f, std::cos(yaw)};
        const float horiz = dist * std::cos(pitch);
        const float vert = dist * std::sin(pitch);
        const V3 want{target.x - forward.x * horiz, target.y + vert,
                      target.z - forward.z * horiz};

        if (!had_world) {
            camPos = want;
            camTarget = target;
        }
        const float smooth = 1.0f - std::exp(-float(dt) * 8.0f);
        camPos = lerp(camPos, want, smooth);
        camTarget = lerp(camTarget, target, smooth);
        had_world = have_world;

        r.draw(boxes, focus, camPos, camTarget, have_world,
               use_view_cam ? float(viewCam.fov_y) : 70.0f);
        std::vector<GuiDrawCommand> gui_cmds;
        if (guiFn) {
            const rbx::Instance* gui_root = guiFn();
            rbxclient::setGuiLayoutViewport(winW, winH);
            gui_cmds = collectGui(gui_root, winW, winH, &rbx::gui::view());
        }
        r.drawGui(gui_cmds);

        const rbx::gui::Cursor want_cursor = rbx::gui::cursor();
        if (want_cursor != last_cursor) {
            last_cursor = want_cursor;
            SDL_Cursor* use = want_cursor == rbx::gui::Cursor::Hand
                                  ? cursor_hand
                              : want_cursor == rbx::gui::Cursor::IBeam
                                  ? cursor_ibeam
                                  : cursor_arrow;
            if (use)
                SDL_SetCursor(use);
        }

        if (std::getenv("RBX_VIEW_HUD") && now - lastTitle > 500) {
            char title[160];
            std::snprintf(title, sizeof(title), "Boblox — %zu parts — %.0f fps",
                          boxes.size(),
                          dt > 1e-6 ? 1.0 / dt : 0.0);
            SDL_SetWindowTitle(r.window, title);
            lastTitle = now;
        }

        // The warmup counts frames of replicated world, not frames since the
        // window opened: a live join needs seconds before the snapshot lands,
        // and capturing during that gap saves a sky-only PNG.
        if (warmup >= 0 && framed && have_world &&
            (!shot_wants_avatar || focus.has_avatar) && ++frames >= warmup) {
            if (std::getenv("RBX_VIEW_DEBUG"))
                std::printf("view: shot boxes=%zu avatar=%d\n", boxes.size(),
                            int(focus.has_avatar));
            shot_saved = r.capture(shot_path, err);
            break;
        }

        // Resolve the multisample target into the window.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, r.msFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, r.fbW, r.fbH, 0, 0, r.fbW, r.fbH,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        SDL_GL_SwapWindow(r.window);
    }

    SDL_DestroyCursor(cursor_arrow);
    SDL_DestroyCursor(cursor_hand);
    SDL_DestroyCursor(cursor_ibeam);
    r.shutdown();
    // SDL_Quit is deliberately not called here: see the init comment above.
    if (warmup >= 0 && !shot_saved) {
        if (err && err->empty())
            *err = framed ? "screenshot was not saved"
                          : "screenshot needs a replicated world";
        return 2;
    }
    return 0;
}

} // namespace

int runViewer(const std::vector<ViewBox>& boxes, const std::string& shot_path,
              std::string* err) {
    if (boxes.empty()) {
        if (err)
            *err = "nothing to render (empty world)";
        return 2;
    }
    const std::vector<ViewBox> stable = boxes;
    return runViewerImpl([stable] { return stable; }, nullptr, shot_path, err,
                         {}, {}, {});
}

int runViewerLive(const std::function<std::vector<ViewBox>()>& boxes,
                  const std::function<bool(double)>& tick,
                  const std::string& shot_path, std::string* err,
                  const std::function<bool(ViewCamera*)>& camera,
                  const std::function<const rbx::Instance*()>& guiRoot,
                  const std::string& cookie) {
    return runViewerImpl(boxes, tick, shot_path, err, cookie, camera, guiRoot);
}

} // namespace rbxclient
