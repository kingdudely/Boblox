// bridge.cpp — the one definition of the executor <-> transport seam.
#include "send/bridge.h"

namespace rbx {
namespace send {

namespace {
const Api* g_api = nullptr;
}

void install(const Api* api) { g_api = api; }
const Api* installed() { return g_api; }

} // namespace send
} // namespace rbx
