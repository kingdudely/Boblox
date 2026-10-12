// view.h — snapshot-walk to raylib boxes (mesh-soup phase).
//
// Split for testability: collectBoxes() is pure (no GL) and walks any
// applied world; runViewer() owns the raylib window + orbit camera.
// Headless runs never touch GL (play_main branches before calling).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rbx {
class Instance;
}

namespace rbxclient {

// One renderable box: a CFrame position + rotation rows + Size extents.
struct ViewBox {
    double px = 0.0, py = 0.0, pz = 0.0;
    double r00 = 1.0, r01 = 0.0, r02 = 0.0;
    double r10 = 0.0, r11 = 1.0, r12 = 0.0;
    double r20 = 0.0, r21 = 0.0, r22 = 1.0;
    double sx = 4.0, sy = 1.0, sz = 2.0;
    std::string name;
};

// Depth-first walk; every descendant whose class chain declares BOTH
// CFrame and Size contributes a box (missing props fall back to the
// registry defaults, exactly like script reads).
std::vector<ViewBox> collectBoxes(const rbx::Instance* root);

// Orbit-camera framing (pure, GL-free): centroid + radius of the
// spawn-area boxes (named exactly "SpawnLocation"), falling back to all
// boxes when none exist. Spawn-focus keeps the joining player's
// neighborhood framed instead of shrinking the whole map to specks.
// Robust throughout: non-finite positions never frame; the center is the
// coordinate median; the radius cuts before the first 10x distance jump
// (live physics ships finite-but-absurd outliers — 167k studs observed —
// that would otherwise empty the frame). Radius covers kept centers plus
// the largest kept half-extent, floored at 1.0.
struct CameraFocus {
    double cx = 0.0, cy = 0.0, cz = 0.0;
    double radius = 1.0;
};
CameraFocus focusCamera(const std::vector<ViewBox>& boxes);

// Open a raylib window (SDL backend) and orbit the boxes until the window
// closes. shot_path non-empty: save a screenshot after warmup frames, then
// exit (for review under Xvfb; never golden — GL output varies by driver).
// Returns 0 on clean exit, 2 + *err on failure (no display, GL init fail).
// Without SDL3 dev at configure time this compiles to a stub that always
// returns 2 (same CLI error path in play_main).
int runViewer(const std::vector<ViewBox>& boxes, const std::string& shot_path,
              std::string* err);

} // namespace rbxclient
