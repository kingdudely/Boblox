// audio_host.h — the embedder supplies real playback.
//
// challenge_core must not link SDL, curl or any platform audio API. The
// headful client installs a Backend that resolves assets and mixes sounds;
// headless/strict builds leave it null, so Sound:Play cannot claim success.
#pragma once

#include <string>

namespace rbx {

class Instance;

namespace audio {

struct Backend {
    virtual ~Backend() = default;

    // Start playing uri for sound. Returns false and fills why when the asset
    // cannot be resolved, decoded, or accepted by the device.
    virtual bool play(Instance* sound, const std::string& uri, bool looped,
                      double volume, std::string* why) = 0;
    virtual void pause(Instance* sound) = 0;
    virtual void resume(Instance* sound) = 0;
    virtual void stop(Instance* sound) = 0;
    // Drop any voice state for an instance that is being destroyed, so a
    // later poll cannot dereference a dangling Instance*.
    virtual void forget(Instance* sound) = 0;
    virtual void set_volume(Instance* sound, double volume) = 0;
    virtual bool is_playing(Instance* sound) = 0;
    // Advance completion flags for finished one-shot voices. Called once per
    // engine frame; implementations may leave it empty when they have no
    // asynchronous state.
    virtual void poll() = 0;
};

void set_backend(Backend* backend);
Backend* backend();

} // namespace audio
} // namespace rbx
