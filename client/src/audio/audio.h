// audio.h — headful sound backend lifecycle.
#pragma once

#include <string>

namespace rbxclient {
namespace audio {

// Open the default playback device and install the runtime backend. The
// cookie is used only by the asset-delivery HTTP client and is never logged.
bool start(const std::string& cookie, std::string* err);
void stop();

} // namespace audio
} // namespace rbxclient
