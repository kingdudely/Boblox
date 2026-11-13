// audio_stub.cpp — no SDL3 at configure time.
#include "audio/audio.h"

namespace rbxclient {
namespace audio {

bool start(const std::string&, std::string* err) {
    if (err)
        *err = "audio backend not built (SDL3 development files missing)";
    return false;
}

void stop() {}

} // namespace audio
} // namespace rbxclient
