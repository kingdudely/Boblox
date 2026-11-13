// audio_host.cpp — see audio_host.h.
#include "instance/audio_host.h"

namespace rbx {
namespace audio {
namespace {
Backend* g_backend = nullptr;
}

void set_backend(Backend* backend) { g_backend = backend; }

Backend* backend() { return g_backend; }

} // namespace audio
} // namespace rbx
