// bridge.h — the executor <-> transport seam for upstream sends.
//
// exec.cpp lives in rbx_solve, the RakNet transport in rbx_raknet, and the
// two must not depend on each other (rbx_raknet already links rbx_solve).
// The app installs this small function table once the session exists; the
// executor calls through it or reports that no session is live.
#pragma once

#include <cstddef>
#include <cstdint>

namespace rbx {
namespace send {

struct Api {
    void* ctx = nullptr;
    // Sustained ControlState (mask 0 clears). True when the session can send.
    bool (*hold)(void* ctx, int mask, int values) = nullptr;
    bool (*release)(void* ctx) = nullptr;
    // One-shot ControlState, applied to the next frame only.
    bool (*control)(void* ctx, int mask, int values) = nullptr;
    // One raw application message. ordered=true rides the reliable-ordered
    // stream (0x60/0x70), false goes out unreliable (0x00) like input.
    bool (*raw)(void* ctx, const void* data, size_t len, bool ordered) = nullptr;
    // True once the data AEAD keys are installed (post-Reply2).
    bool (*ready)(void* ctx) = nullptr;
};

// Defined once in bridge.cpp. An inline function with a static local is not
// safe here: rbx_solve is a static library, and each object that emits the
// inline gets its own local copy under this build, so exec.cpp would read a
// different slot than the app writes.
void install(const Api* api);
const Api* installed();

} // namespace send
} // namespace rbx
