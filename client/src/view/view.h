// view.h — snapshot-walk to renderable parts (SDL3 + OpenGL presentation).
//
// Split for testability: collectBoxes() is pure (no GL) and walks any
// applied world; runViewer() owns the SDL3 window + third-person camera.
// Headless runs never touch GL (play_main branches before calling).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rbx {
class Instance;
}

namespace rbxclient {

// One renderable part: a CFrame position + rotation rows + Size extents.
// Appearance fields mirror what BasePart actually replicates; material is
// Enum.Material (256 = Plastic) and drives the shader's response.
struct ViewBox {
    double px = 0.0, py = 0.0, pz = 0.0;
    double r00 = 1.0, r01 = 0.0, r02 = 0.0;
    double r10 = 0.0, r11 = 1.0, r12 = 0.0;
    double r20 = 0.0, r21 = 0.0, r22 = 1.0;
    double sx = 4.0, sy = 1.0, sz = 2.0;
    double r = 0.639, g = 0.639, b = 0.639;
    double transparency = 0.0; // clamped 0..1
    double reflectance = 0.0;  // clamped 0..1
    int shape = 1;             // Enum.PartType: Ball=0, Block=1, Cylinder=2, Wedge=3, CornerWedge=4
    int material = 256;        // Enum.Material (Plastic)
    bool local_avatar = false; // descendant of the local Player's Character
    std::string name;

    // A transparent/glass part needs the back-to-front pass.
    bool is_transparent() const {
        return transparency > 0.005 || material == 1568 /* Glass */ ||
               material == 1584 /* ForceField */;
    }
};

// Depth-first walk; every descendant whose class chain declares BOTH
// CFrame and Size contributes a part (missing props fall back to the
// registry defaults, exactly like script reads).
std::vector<ViewBox> collectBoxes(const rbx::Instance* root);

// Third-person / overview framing (pure, GL-free).
//
// Priority 1: the local player's replicated Character (the way the official
// client starts behind the avatar). Priority 2: SpawnLocation pads, so a
// joining player's neighborhood is framed instead of the whole map.
// Robust throughout: non-finite positions never frame; the center is the
// coordinate median; the radius is the 90th-percentile reach (distance +
// half-extent), so far-flung skybox/decor parts (167k studs observed live)
// cannot shrink the playable area to specks. Floored at 1.0.
struct CameraFocus {
    double cx = 0.0, cy = 0.0, cz = 0.0;
    double radius = 1.0;
    bool has_avatar = false;
    // Avatar facing direction (Roblox LookVector = -Z CFrame column).
    double fx = 0.0, fy = 0.0, fz = -1.0;
};
CameraFocus focusCamera(const std::vector<ViewBox>& boxes);

// Open an SDL3 window and follow the avatar / orbit the world until the
// window closes. shot_path non-empty: save a screenshot after warmup frames,
// then exit (for review; never golden — GL output varies by driver). Returns
// 0 on clean exit, 2 + *err on failure (no display, GL init fail). Without
// SDL3 dev at configure time this compiles to a stub that always returns 2
// (same CLI error path).
int runViewer(const std::vector<ViewBox>& boxes, const std::string& shot_path,
              std::string* err);

// Camera pose supplied by the client-owned local player.  The viewer falls
// back to its orbit camera while this returns false (before spawn).
struct ViewCamera {
    bool valid = false;
    double px = 0.0, py = 0.0, pz = 0.0;
    double lx = 0.0, ly = 0.0, lz = -1.0; // look direction
    double fov_y = 70.0;
};

// Live window: parts are re-collected every frame (empty = keep waiting for
// the first snapshot), and tick(dt) runs the transport + Engine scheduler
// from inside the render loop. Return false from tick to end the window.
int runViewerLive(const std::function<std::vector<ViewBox>()>& boxes,
                  const std::function<bool(double)>& tick,
                  const std::string& shot_path, std::string* err,
                  const std::function<bool(ViewCamera*)>& camera = {},
                  const std::function<const rbx::Instance*()>& guiRoot = {});

} // namespace rbxclient
