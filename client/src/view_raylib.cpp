// view_raylib.cpp — raylib window backend for runViewer() (see view.h).
//
// Built only when SDL3 dev is present at configure time (raylib PLATFORM
// SDL). Screenshot path uses LoadImageFromScreen + ExportImage, NOT
// TakeScreenshot: raylib prepends its internal exe-dir basePath to
// TakeScreenshot paths, which breaks absolute --shot paths (verified
// live). Fully portable, no OS headers.
#include "view.h"

#include "raylib.h"
#include "raymath.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace rbxclient {

int runViewer(const std::vector<ViewBox>& boxes, const std::string& shot_path,
              std::string* err) {
    if (boxes.empty()) {
        if (err)
            *err = "nothing to render (empty world)";
        return 2;
    }
    // Spawn-focus framing (pure): orbit the joining player's neighborhood,
    // not the whole-map centroid (which shrinks everything to specks).
    const CameraFocus focus = focusCamera(boxes);
    const double cx = focus.cx, cy = focus.cy, cz = focus.cz;
    const double radius = focus.radius;

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
