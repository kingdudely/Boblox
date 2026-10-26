// view_raylib.cpp — raylib presentation backend for runViewer()/Live().
//
// The point of this file is to look like the game, not like a debug viewer:
//   * third-person follow camera on the local avatar (overview before it
//     replicates, or when a capture has no character)
//   * directional sun + hemispheric sky/ground ambient + specular highlights
//     instead of flat unlit boxes
//   * material response for Neon/Glass/Metal/ForceField, alpha blending and
//     a back-to-front transparent pass
//   * sky gradient, distance haze, and correct part silhouettes (wedge and
//     X-axis cylinders are real meshes, not scaled cubes)
//
// Built only when SDL3 dev is present at configure time (raylib PLATFORM
// SDL). Screenshot path uses LoadImageFromScreen + ExportImage, NOT
// TakeScreenshot: raylib prepends its internal exe-dir basePath to
// TakeScreenshot paths, which breaks absolute --shot paths (verified live).
#include "view.h"

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace rbxclient {

namespace {

// ---- meshes ----------------------------------------------------------------

Mesh uploadMesh(std::vector<float> vertices, std::vector<float> normals,
                std::vector<float> uvs) {
    Mesh m = {0};
    m.vertexCount = int(vertices.size() / 3);
    m.triangleCount = m.vertexCount / 3;
    if (m.vertexCount == 0)
        return m;
    m.vertices = (float*)MemAlloc(unsigned(vertices.size() * sizeof(float)));
    m.normals = (float*)MemAlloc(unsigned(normals.size() * sizeof(float)));
    m.texcoords = (float*)MemAlloc(unsigned(uvs.size() * sizeof(float)));
    std::memcpy(m.vertices, vertices.data(), vertices.size() * sizeof(float));
    std::memcpy(m.normals, normals.data(), normals.size() * sizeof(float));
    std::memcpy(m.texcoords, uvs.data(), uvs.size() * sizeof(float));
    UploadMesh(&m, false);
    return m;
}

// A right triangular prism: full rectangular base, vertical back, slope down
// toward -Z. This is the silhouette of Enum.PartType.Wedge (and a close
// stand-in for CornerWedge).
Mesh genWedgeMesh() {
    std::vector<float> v, n, uv;
    auto face = [&](const float* p, const float* normal) {
        for (int i = 0; i < 3; ++i) {
            v.push_back(p[i * 3 + 0]);
            v.push_back(p[i * 3 + 1]);
            v.push_back(p[i * 3 + 2]);
            n.push_back(normal[0]);
            n.push_back(normal[1]);
            n.push_back(normal[2]);
            uv.push_back(i == 1 ? 1.0f : 0.0f);
            uv.push_back(i == 2 ? 1.0f : 0.0f);
        }
    };
    const float bottom[] = {-0.5f, -0.5f, -0.5f, 0.5f, -0.5f,  0.5f,
                            0.5f,  -0.5f, 0.5f,  -0.5f, -0.5f, 0.5f};
    const float nDown[] = {0.0f, -1.0f, 0.0f};
    face(&bottom[0], nDown);
    face(&bottom[1 * 3], nDown);
    const float back[] = {-0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f,
                          0.5f,  0.5f,  0.5f, -0.5f, 0.5f,  0.5f};
    const float nBack[] = {0.0f, 0.0f, 1.0f};
    face(&back[0], nBack);
    face(&back[1 * 3], nBack);
    const float left[] = {-0.5f, -0.5f, -0.5f, -0.5f, -0.5f, 0.5f,
                          -0.5f, 0.5f,  0.5f};
    const float nLeft[] = {-1.0f, 0.0f, 0.0f};
    face(&left[0], nLeft);
    const float right[] = {0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f,
                           0.5f, -0.5f, 0.5f};
    const float nRight[] = {1.0f, 0.0f, 0.0f};
    face(&right[0], nRight);
    const float slope[] = {-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f,
                           -0.5f, 0.5f,  0.5f, 0.5f, -0.5f, -0.5f};
    const float nSlope[] = {0.0f, 0.70710678f, -0.70710678f};
    face(&slope[0], nSlope);
    const float slope2[] = {-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f,
                            0.5f,  -0.5f, -0.5f};
    face(&slope2[0], nSlope);
    return uploadMesh(std::move(v), std::move(n), std::move(uv));
}

// Roblox cylinders are X-axis parts; raylib's GenMeshCylinder is Y-axis and
// would visibly stand pipes on end. Build the X-axis mesh directly so Size
// maps to (length, diameter, diameter) exactly like a Part.
Mesh genCylinderXMesh(int slices = 24) {
    std::vector<float> v, n, uv;
    auto vert = [&](float x, float a, float b, float nx, float ny, float nz,
                    float u, float w) {
        v.push_back(x);
        v.push_back(a);
        v.push_back(b);
        n.push_back(nx);
        n.push_back(ny);
        n.push_back(nz);
        uv.push_back(u);
        uv.push_back(w);
    };
    for (int i = 0; i < slices; ++i) {
        const float a0 = float(2.0 * PI * i / slices);
        const float a1 = float(2.0 * PI * (i + 1) / slices);
        const float y0 = std::cos(a0) * 0.5f, z0 = std::sin(a0) * 0.5f;
        const float y1 = std::cos(a1) * 0.5f, z1 = std::sin(a1) * 0.5f;
        // side quad (two triangles), normals point radially out
        vert(-0.5f, y0, z0, 0, std::cos(a0), std::sin(a0), 0, 0);
        vert(0.5f, y0, z0, 0, std::cos(a0), std::sin(a0), 1, 0);
        vert(0.5f, y1, z1, 0, std::cos(a1), std::sin(a1), 1, 1);
        vert(-0.5f, y0, z0, 0, std::cos(a0), std::sin(a0), 0, 0);
        vert(0.5f, y1, z1, 0, std::cos(a1), std::sin(a1), 1, 1);
        vert(-0.5f, y1, z1, 0, std::cos(a1), std::sin(a1), 0, 1);
        // cap -X
        vert(-0.5f, 0, 0, -1, 0, 0, 0.5f, 0.5f);
        vert(-0.5f, y1, z1, -1, 0, 0, 0, 0);
        vert(-0.5f, y0, z0, -1, 0, 0, 1, 0);
        // cap +X
        vert(0.5f, 0, 0, 1, 0, 0, 0.5f, 0.5f);
        vert(0.5f, y0, z0, 1, 0, 0, 0, 0);
        vert(0.5f, y1, z1, 1, 0, 0, 1, 0);
    }
    return uploadMesh(std::move(v), std::move(n), std::move(uv));
}

// ---- shader ----------------------------------------------------------------

const char* kVertexShader = R"GLSL(
#version 330
in vec3 vertexPosition;
in vec3 vertexNormal;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;
out vec3 fragPosition;
out vec3 fragNormal;
out vec2 fragTexCoord;
out vec4 fragColor;
void main() {
    fragPosition = vec3(matModel * vec4(vertexPosition, 1.0));
    fragNormal = normalize(mat3(matModel) * vertexNormal);
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)GLSL";

const char* kFragmentShader = R"GLSL(
#version 330
in vec3 fragPosition;
in vec3 fragNormal;
in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec3 viewPos;
uniform vec3 lightDir;
uniform vec3 lightColor;
uniform vec3 skyColor;
uniform vec3 groundColor;
uniform vec3 fogColor;
uniform float fogDensity;
uniform float emissive;
uniform float specularStrength;
void main() {
    vec4 texel = texture(texture0, fragTexCoord);
    vec3 base = texel.rgb * colDiffuse.rgb * fragColor.rgb;
    vec3 n = normalize(fragNormal);
    vec3 l = normalize(lightDir);
    float ndl = max(dot(n, l), 0.0);
    // Half-Lambert fill: Roblox's sky/bounce light means the shadow side is
    // never black, which is exactly what made the old unlit silhouettes read
    // as a debug viewer instead of the game.
    float wrap = ndl * 0.5 + 0.5;
    float hemi = 0.5 + 0.5 * n.y;
    vec3 viewDir = normalize(viewPos - fragPosition);
    vec3 ambient = mix(groundColor, skyColor, hemi) * 1.05;
    vec3 diffuse = lightColor * (ndl * 0.95 + wrap * 0.22);
    vec3 halfDir = normalize(l + viewDir);
    float spec = pow(max(dot(n, halfDir), 0.0), 72.0) * specularStrength;
    float rim = pow(1.0 - max(dot(n, viewDir), 0.0), 3.0) * 0.10;
    vec3 lit = base * (ambient + diffuse) + lightColor * spec + skyColor * rim;
    lit = mix(lit, base * 1.12, clamp(emissive, 0.0, 1.0));
    // Gentle filmic shoulder so direct sun does not clip white plastic.
    lit = lit / (lit + vec3(0.55)) * 1.55;
    float dist = length(viewPos - fragPosition);
    float fog = 1.0 - exp(-fogDensity * fogDensity * dist * dist);
    vec3 outColor = mix(lit, fogColor, clamp(fog, 0.0, 0.88));
    finalColor = vec4(outColor, colDiffuse.a * fragColor.a);
}
)GLSL";

// ---- per-material response -------------------------------------------------

struct Look {
    float emissive = 0.0f;
    float specular = 0.18f;
    unsigned char alpha = 255;
};

Look lookFor(const ViewBox& b) {
    Look l;
    switch (b.material) {
    case 288: // Neon
        l.emissive = 0.92f;
        l.specular = 0.0f;
        break;
    case 1568: // Glass
        l.alpha = (unsigned char)std::clamp(52.0 + (1.0 - b.transparency) * 48.0,
                                           35.0, 150.0);
        l.specular = 0.9f;
        break;
    case 1584: // ForceField
        l.alpha = (unsigned char)std::clamp(70.0 + (1.0 - b.transparency) * 50.0,
                                           45.0, 150.0);
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
        l.alpha = (unsigned char)std::clamp(120.0 + (1.0 - b.transparency) * 60.0,
                                            60.0, 190.0);
        l.specular = 0.8f;
        break;
    default:
        l.specular = 0.12f + float(b.reflectance) * 0.7f;
        break;
    }
    if (b.transparency > 0.005) {
        const double a = (1.0 - b.transparency) * 255.0;
        l.alpha = (unsigned char)std::min<double>(l.alpha, std::max(0.0, a));
    }
    return l;
}

Matrix boxMatrix(const ViewBox& b) {
    // CFrame rows (r00..r22, GetComponents order) into raylib's
    // column-major Matrix: m0/m4/m8 = row 0, etc.
    Matrix rot = MatrixIdentity();
    rot.m0 = (float)b.r00;
    rot.m4 = (float)b.r01;
    rot.m8 = (float)b.r02;
    rot.m1 = (float)b.r10;
    rot.m5 = (float)b.r11;
    rot.m9 = (float)b.r12;
    rot.m2 = (float)b.r20;
    rot.m6 = (float)b.r21;
    rot.m10 = (float)b.r22;
    const Matrix scl = MatrixScale((float)b.sx, (float)b.sy, (float)b.sz);
    const Matrix tr = MatrixTranslate((float)b.px, (float)b.py, (float)b.pz);
    return MatrixMultiply(MatrixMultiply(scl, rot), tr);
}

// ---- window ----------------------------------------------------------------

int runViewerImpl(const std::function<std::vector<ViewBox>()>& boxFn,
                  const std::function<bool(double)>& tick,
                  const std::string& shot_path, std::string* err) {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_MSAA_4X_HINT);
    InitWindow(1280, 720, "Boblox");
    if (!IsWindowReady()) {
        if (err)
            *err = "raylib window not ready (no display? use --headless)";
        return 2;
    }
    SetTargetFPS(60);

    Shader shader = LoadShaderFromMemory(kVertexShader, kFragmentShader);
    if (shader.id == 0) {
        if (err)
            *err = "lighting shader failed to compile";
        CloseWindow();
        return 2;
    }
    const int uViewPos = GetShaderLocation(shader, "viewPos");
    const int uLightDir = GetShaderLocation(shader, "lightDir");
    const int uLightColor = GetShaderLocation(shader, "lightColor");
    const int uSkyColor = GetShaderLocation(shader, "skyColor");
    const int uGroundColor = GetShaderLocation(shader, "groundColor");
    const int uFogColor = GetShaderLocation(shader, "fogColor");
    const int uFogDensity = GetShaderLocation(shader, "fogDensity");
    const int uEmissive = GetShaderLocation(shader, "emissive");
    const int uSpecular = GetShaderLocation(shader, "specularStrength");

    // Late-afternoon key light from the upper left, warm; ambient is cool
    // sky over warm bounce. Values are in linear-ish sRGB; raylib's default
    // framebuffer is not tone mapped, so keep the highlights restrained.
    const Vector3 lightDir = Vector3Normalize((Vector3){-0.45f, 0.82f, 0.35f});
    const Vector3 lightColor = (Vector3){1.10f, 1.04f, 0.92f};
    const Vector3 skyColor = (Vector3){0.46f, 0.57f, 0.72f};
    const Vector3 groundColor = (Vector3){0.20f, 0.18f, 0.16f};
    const Vector3 fogColor = (Vector3){0.72f, 0.82f, 0.94f};
    const float fogDensity = 0.0011f;
    SetShaderValue(shader, uLightDir, &lightDir, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, uLightColor, &lightColor, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, uSkyColor, &skyColor, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, uGroundColor, &groundColor, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, uFogColor, &fogColor, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, uFogDensity, &fogDensity, SHADER_UNIFORM_FLOAT);

    Material material = LoadMaterialDefault();
    material.shader = shader;

    Mesh meshes[5] = {
        GenMeshSphere(0.5f, 24, 24),  // Ball
        GenMeshCube(1.0f, 1.0f, 1.0f), // Block
        genCylinderXMesh(),            // Cylinder
        genWedgeMesh(),                // Wedge
        genWedgeMesh(),                // CornerWedge (approximation)
    };
    UploadMesh(&meshes[0], false);
    UploadMesh(&meshes[1], false);

    Camera3D camera = {0};
    camera.fovy = 70.0f;
    camera.projection = CAMERA_PERSPECTIVE;
    camera.up = (Vector3){0.0f, 1.0f, 0.0f};
    float yaw = float(PI * 0.25);
    float pitch = 0.42f;
    float zoom = 1.0f;
    bool framed = false;
    bool was_avatar = false;
    bool had_world = false;

    int frames = 0;
    const int warmup = shot_path.empty() ? -1 : 120;
    bool shot_saved = false;

    while (!WindowShouldClose()) {
        const double dt = double(GetFrameTime());
        if (tick && !tick(dt))
            break;

        const std::vector<ViewBox> boxes = boxFn();
        const CameraFocus focus = focusCamera(boxes);
        const bool have_world = !boxes.empty();
        if (std::getenv("RBX_VIEW_DEBUG") && frames < 2)
            std::printf("view: boxes=%zu avatar=%d focus=(%.2f,%.2f,%.2f) "
                        "r=%.2f cam=(%.2f,%.2f,%.2f)\n",
                        boxes.size(), int(focus.has_avatar), focus.cx,
                        focus.cy, focus.cz, focus.radius, camera.position.x,
                        camera.position.y, camera.position.z);
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
                yaw = focus.has_avatar
                          ? std::atan2(float(focus.fx), float(focus.fz))
                          : float(PI * 0.25);
                pitch = focus.has_avatar ? 0.28f : 0.48f;
                was_avatar = focus.has_avatar;
                framed = true;
            }
        }

        // Mouse orbit / wheel zoom. Right or middle drag, exactly the
        // controls a viewer expects; left click stays free for future input.
        if (have_world) {
            if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) ||
                IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
                const Vector2 d = GetMouseDelta();
                yaw -= d.x * 0.006f;
                pitch = std::clamp(pitch + d.y * 0.005f, -0.10f, 1.15f);
            }
            zoom = std::clamp(zoom - GetMouseWheelMove() * 0.12f, 0.45f, 2.8f);
        }

        Vector3 target = {(float)focus.cx, (float)focus.cy, (float)focus.cz};
        if (focus.has_avatar)
            target.y += 1.7f;
        // Third-person distance is body-scale, never map-scale: one giant
        // hat/accessory must not push the camera a thousand studs away.
        float base_dist = focus.has_avatar
                              ? std::clamp(float(focus.radius) * 2.7f,
                                           9.0f, 18.0f)
                              : std::max(12.0f, float(focus.radius) * 2.2f);
        if (!focus.has_avatar && framed && was_avatar)
            base_dist = std::max(base_dist, 12.0f);
        const float dist = base_dist * zoom;
        const Vector3 forward = {std::sin(yaw), 0.0f, std::cos(yaw)};
        const float horiz = dist * std::cos(pitch);
        const float vert = dist * std::sin(pitch);
        const Vector3 want = {target.x - forward.x * horiz, target.y + vert,
                              target.z - forward.z * horiz};

        if (!had_world)
            camera.position = want;
        const float smooth = 1.0f - std::exp(-float(dt) * 8.0f);
        camera.position = Vector3Lerp(camera.position, want, smooth);
        camera.target = Vector3Lerp(camera.target, target, smooth);
        had_world = have_world;

        const Vector3 viewPos = camera.position;
        SetShaderValue(shader, uViewPos, &viewPos, SHADER_UNIFORM_VEC3);

        BeginDrawing();
        // Roblox's default sky is a blue gradient with a hazy horizon far
        // brighter than the zenith. A flat ClearBackground is the single
        // biggest reason the old viewer read as a debug scene.
        const int sw = GetScreenWidth(), sh = GetScreenHeight();
        DrawRectangleGradientV(0, 0, sw, sh, (Color){78, 143, 214, 255},
                               (Color){206, 228, 247, 255});
        // Soft sun bloom high on the right.
        DrawCircleGradient(
            (Vector2){sw * 0.78f, sh * 0.16f}, float(sh) * 0.28f,
            Fade((Color){255, 249, 224, 255}, 0.42f), BLANK);

        rlSetClipPlanes(0.5, 20000.0);
        BeginMode3D(camera);
        BeginBlendMode(BLEND_ALPHA);

        // Opaque first (depth write on), then transparent back-to-front with
        // depth writes off. Sorting thousands of parts once per frame is
        // cheap compared with the draw calls it saves state changes for.
        std::vector<const ViewBox*> opaque;
        std::vector<const ViewBox*> transparent;
        opaque.reserve(boxes.size());
        transparent.reserve(64);
        for (const auto& b : boxes) {
            if (b.is_transparent())
                transparent.push_back(&b);
            else
                opaque.push_back(&b);
        }
        std::sort(transparent.begin(), transparent.end(),
                  [&](const ViewBox* a, const ViewBox* b) {
                      auto d2 = [&](const ViewBox* v) {
                          const float dx = float(v->px) - viewPos.x;
                          const float dy = float(v->py) - viewPos.y;
                          const float dz = float(v->pz) - viewPos.z;
                          return dx * dx + dy * dy + dz * dz;
                      };
                      return d2(a) > d2(b);
                  });

        auto draw = [&](const ViewBox& b) {
            Look look = lookFor(b);
            const int shape = (b.shape >= 0 && b.shape <= 4) ? b.shape : 1;
            material.maps[MATERIAL_MAP_DIFFUSE].color = (Color){
                (unsigned char)(std::clamp(b.r, 0.0, 1.0) * 255.0),
                (unsigned char)(std::clamp(b.g, 0.0, 1.0) * 255.0),
                (unsigned char)(std::clamp(b.b, 0.0, 1.0) * 255.0), look.alpha};
            SetShaderValue(shader, uEmissive, &look.emissive,
                           SHADER_UNIFORM_FLOAT);
            SetShaderValue(shader, uSpecular, &look.specular,
                           SHADER_UNIFORM_FLOAT);
            DrawMesh(meshes[shape], material, boxMatrix(b));
        };
        for (const ViewBox* b : opaque)
            draw(*b);
        if (!transparent.empty()) {
            rlDisableDepthMask();
            for (const ViewBox* b : transparent)
                draw(*b);
            rlEnableDepthMask();
        }

        EndBlendMode();
        EndMode3D();

        // No debug HUD in the normal view: the point is a game-like frame.
        // Opt in when diagnosing a capture.
        if (std::getenv("RBX_VIEW_HUD")) {
            DrawText(boxes.empty()
                         ? "waiting for replication"
                         : TextFormat("%i parts  %.0f fps", (int)boxes.size(),
                                      GetFPS()),
                     12, 12, 18, Fade(BLACK, 0.65f));
        }
        EndDrawing();

        if (warmup >= 0 && framed && ++frames >= warmup) {
            Image img = LoadImageFromScreen();
            if (img.data == nullptr) {
                if (err)
                    *err = "screen capture failed (no frame data)";
                UnloadImage(img);
                break;
            }
            if (!ExportImage(img, shot_path.c_str())) {
                if (err)
                    *err = std::string("screenshot save failed: ") + shot_path;
                UnloadImage(img);
                break;
            }
            UnloadImage(img);
            shot_saved = true;
            break;
        }
    }

    for (Mesh& m : meshes)
        UnloadMesh(m);
    UnloadShader(shader);
    CloseWindow();
    if (warmup >= 0 && !shot_saved) {
        if (err)
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
    return runViewerImpl([stable] { return stable; }, nullptr, shot_path, err);
}

int runViewerLive(const std::function<std::vector<ViewBox>()>& boxes,
                  const std::function<bool(double)>& tick,
                  const std::string& shot_path, std::string* err) {
    return runViewerImpl(boxes, tick, shot_path, err);
}

} // namespace rbxclient
