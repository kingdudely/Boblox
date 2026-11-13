#include "instance/contact_host.h"

#include "instance/instance.h"
#include "instance/signal.h"

namespace rbx {
namespace contact {

namespace {
Backend* g_backend = nullptr;
}

void set_backend(Backend* backend) { g_backend = backend; }
Backend* backend() { return g_backend; }

void dispatch(lua_State* L) {
    if (!g_backend || !L)
        return;
    const auto fire = [L](std::vector<Pair> pairs, const char* event) {
        for (const Pair& pair : pairs) {
            if (pair.a && !pair.a->destroyed())
                pair.a->event_signal(event)->fire(
                    L, {Variant(pair.b.get())});
            if (pair.b && !pair.b->destroyed())
                pair.b->event_signal(event)->fire(
                    L, {Variant(pair.a.get())});
        }
    };
    fire(g_backend->take_begun(), "Touched");
    fire(g_backend->take_ended(), "TouchEnded");
}

} // namespace contact
} // namespace rbx
