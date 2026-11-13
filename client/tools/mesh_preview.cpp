// mesh_preview.cpp — deterministic headful proof for the five PartType meshes.
// The live viewer frames a streamed map; this offline viewer contains only the
// native primitive boxes, so the Wedge/CornerWedge silhouettes and normals are
// large and readable in one --shot artifact.
#include "view/view.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const std::string shot = argc > 1 ? argv[1] : "mesh_preview.png";
    const int only = argc > 2 ? std::atoi(argv[2]) : -1;
    const double yaw_override =
        argc > 3 ? std::atof(argv[3]) * 3.14159265358979323846 / 180.0 : 0.0;
    const char* names[] = {"Ball", "Block", "Cylinder", "Wedge",
                           "CornerWedge"};
    const double colors[][3] = {
        {0.82, 0.31, 0.31}, {0.31, 0.59, 0.86}, {0.35, 0.75, 0.47},
        {0.94, 0.71, 0.24}, {0.78, 0.39, 0.86}};
    std::vector<rbxclient::ViewBox> boxes;
    for (int i = 0; i < 5; ++i) {
        if (only >= 0 && i != only)
            continue;
        rbxclient::ViewBox b;
        b.name = names[i];
        b.shape = i;
        b.sx = 10.0;
        b.sy = 7.0;
        b.sz = 10.0;
        b.px = (i - 2) * 8.0;
        b.py = 3.5;
        b.pz = 0.0;
        // Turn the asymmetric shapes so their distinguishing feature faces
        // the overview camera: CornerWedge's apex at +X/-Z and Wedge's slope
        // toward -Z are otherwise hidden behind their base faces.
        const double yaw = argc > 3 ? yaw_override
                                    : (i == 3 ? 3.14159265359 : 0.35);
        b.r00 = std::cos(yaw);
        b.r02 = std::sin(yaw);
        b.r20 = -std::sin(yaw);
        b.r22 = std::cos(yaw);
        b.r = colors[i][0];
        b.g = colors[i][1];
        b.b = colors[i][2];
        boxes.push_back(b);
    }
    std::string err;
    const int rc = rbxclient::runViewer(boxes, shot, &err);
    if (rc != 0)
        std::fprintf(stderr, "mesh_preview: %s\n", err.c_str());
    return rc;
}