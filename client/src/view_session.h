// view_session.h — captured chan1 stream -> DataModel world -> viewer.
//
// Transport-independent world builder (bytes in, rendered world out): the
// seam a future Studio frontend drives without touching QUIC/session.
// Lives in rbx_view (needs joindata apply + view collect/render).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rbxclient {

using LogFn = std::function<void(const std::string&)>;

// Build the world from a captured chan1 stream and render the frozen
// snapshot through runViewer (headful). Returns 0 on clean viewer exit,
// 2 + log on failure.
int viewSession(const std::vector<uint8_t>& chan1, const std::string& shot,
                const LogFn& log);

} // namespace rbxclient
