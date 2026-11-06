// view_stub.cpp — runViewer() fallback when SDL3 dev is absent.
//
// CI (ubuntu-24.04) and minimal dev machines have no libsdl3-dev (it only
// ships in Ubuntu 25.10+), so the SDL3+OpenGL backend cannot configure
// there. The pure snapshot walk (view_collect.cpp) still builds and stays
// golden; only the GL window is stubbed. --headful then fails LOUDLY through
// the normal play_main error path (rc 2), never silently.
#include "view/view.h"

namespace rbxclient {

int runViewer(const std::vector<ViewBox>&, const std::string&,
              std::string* err) {
    if (err)
        *err = "headful viewer not built (no SDL3 dev at configure time; "
               "install SDL3 and re-run cmake for --headful/--shot)";
    return 2;
}

int runViewerLive(const std::function<std::vector<ViewBox>()>&,
                  const std::function<bool(double)>&, const std::string&,
                  std::string* err) {
    if (err)
        *err = "headful viewer not built (no SDL3 dev at configure time; "
               "install SDL3 and re-run cmake, or pass --headless)";
    return 2;
}

} // namespace rbxclient
