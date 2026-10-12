// view.cpp — snapshot-walk to raylib boxes (see view.h).
//
// Renderable rule: an instance contributes a box when its class chain
// declares CFrame AND Size (BasePart descendants). Color is a fixed
// neutral: snapshot Parts carry Color3uint8/BrickColor, both currently
// skipped at apply (undeclared/unresolvable) — renderer color mapping is
// a later phase, not faked here.
#include "view.h"

#include "instance/class_registry.h"
#include "instance/instance.h"
#include "instance/variant.h"

#include "raylib.h"
#include "raymath.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <variant>

namespace rbxclient {

std::vector<ViewBox> collectBoxes(const rbx::Instance* root) {
    std::vector<ViewBox> out;
    if (!root)
        return out;
    // Iterative DFS (worlds are thousands deep via services; no recursion).
    std::vector<const rbx::Instance*> stack{root};
    while (!stack.empty()) {
        const rbx::Instance* cur = stack.back();
        stack.pop_back();
        for (const auto& ch : cur->children())
            stack.push_back(ch.get());
        if (cur == root)
            continue;
        const rbx::PropInfo* cf = rbx::find_property(cur->class_info(), "CFrame");
        const rbx::PropInfo* sz = rbx::find_property(cur->class_info(), "Size");
        if (!cf || !sz)
            continue;
        if (cf->type != rbx::PropType::CFrame || sz->type != rbx::PropType::Vector3)
            continue;
        rbx::Variant vcf, vsz;
        if (!cur->get_prop("CFrame", vcf) || !cur->get_prop("Size", vsz))
            continue;
        const rbx::CFrame* c = std::get_if<rbx::CFrame>(&vcf);
        const rbx::Vector3* s = std::get_if<rbx::Vector3>(&vsz);
        if (!c || !s)
            continue;
        ViewBox b;
        b.px = c->x;
        b.py = c->y;
        b.pz = c->z;
        b.r00 = c->r00;
        b.r01 = c->r01;
        b.r02 = c->r02;
        b.r10 = c->r10;
        b.r11 = c->r11;
        b.r12 = c->r12;
        b.r20 = c->r20;
        b.r21 = c->r21;
        b.r22 = c->r22;
        b.sx = s->x;
        b.sy = s->y;
        b.sz = s->z;
        b.name = cur->name();
        out.push_back(b);
    }
    return out;
}

int runViewer(const std::vector<ViewBox>& boxes, const std::string& shot_path,
              std::string* err) {
    if (boxes.empty()) {
        if (err)
            *err = "nothing to render (empty world)";
        return 2;
    }
    // World center + radius for the orbit camera.
    double cx = 0, cy = 0, cz = 0;
    for (const auto& b : boxes) {
        cx += b.px;
        cy += b.py;
        cz += b.pz;
    }
    cx /= boxes.size();
    cy /= boxes.size();
    cz /= boxes.size();
    double radius = 0;
    for (const auto& b : boxes) {
        const double d = std::sqrt((b.px - cx) * (b.px - cx) +
                                   (b.py - cy) * (b.py - cy) +
                                   (b.pz - cz) * (b.pz - cz));
        if (d > radius)
            radius = d;
    }
    if (radius < 1.0)
        radius = 1.0;

    InitWindow(1280, 720, "boblox rbxplay --headful (snapshot mesh soup)");
    if (!IsWindowReady()) {
        if (err)
            *err = "raylib window not ready (no display?)";
        return 2;
    }
    SetTargetFPS(60);
    Camera3D camera = {0};
    camera.position =
        (Vector3){float(cx + radius * 1.2), float(cy + radius * 0.9),
                  float(cz + radius * 1.2)};
    camera.target = (Vector3){float(cx), float(cy), float(cz)};
    camera.up = (Vector3){0.0f, 1.0f, 0.0f};
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    // One shared unit cube; per-Part transform via matrix multiply.
    Mesh cube = GenMeshCube(1.0f, 1.0f, 1.0f);
    Model model = LoadModelFromMesh(cube);
    const Color tint = {160, 160, 160, 255};

    int frames = 0;
    const int warmup = shot_path.empty() ? -1 : 120;
    while (!WindowShouldClose()) {
        UpdateCamera(&camera, CAMERA_ORBITAL);
        BeginDrawing();
        ClearBackground(RAYWHITE);
        BeginMode3D(camera);
        for (const auto& b : boxes) {
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
            const Matrix scl =
                MatrixScale((float)b.sx, (float)b.sy, (float)b.sz);
            const Matrix tr =
                MatrixTranslate((float)b.px, (float)b.py, (float)b.pz);
            model.transform = MatrixMultiply(MatrixMultiply(scl, rot), tr);
            DrawModel(model, (Vector3){0, 0, 0}, 1.0f, tint);
        }
        EndMode3D();
        DrawText("boblox snapshot (ESC closes)", 10, 10, 20, DARKGRAY);
        EndDrawing();
        if (warmup >= 0 && ++frames >= warmup) {
            // NOTE: TakeScreenshot() prepends raylib's internal basePath
            // (exe dir), so absolute paths break. LoadImageFromScreen +
            // ExportImage is the same capture with no path mangling, and
            // is fully portable (no OS headers).
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
            break;
        }
    }
    UnloadModel(model); // (releases the cube mesh too)
    CloseWindow();
    return 0;
}

} // namespace rbxclient
