// remote.h — client->server RemoteEvent transport bridge.
//
// The Engine runtime knows how to encode a RemoteEvent call (the wire grammar
// is fixed and self-describing), but it must not know RakNet/QUIC. The host
// installs one sink before pumping scripts; FireServer serializes the call
// and hands the bytes to it. A host with no sink (tests, offline playback)
// raises a legible Lua error instead of silently dropping the call.
//
// Wire (native 2.739, verified against live TX captures):
//   [83][07][varint lo][u32 instance id][u16 0x11bb][u8 argc][args...][00]
// 0x11bb is the native's fixed "RemoteEvent invocation" id (not a schema
// event index): every RemoteEvent, reliable or not, uses it, and the target
// instance ref selects which remote the server fires.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct lua_State;

namespace rbx {

// user: the host pointer passed to set_remote_send.
// Returns false when the bytes could not be handed to the transport.
using RemoteSendFn = bool (*)(void* user, const uint8_t* data, size_t len,
                              bool unreliable);

// Install (or clear, with fn == nullptr) the process-wide sink. The live
// client installs it once; tests install a capture sink.
void set_remote_send(RemoteSendFn fn, void* user);

// Attach FireServer to RemoteEvent/UnreliableRemoteEvent. Called by
// register_instance_methods().
void install_remote_methods();

// Serialize one RemoteEvent invocation. Exposed for unit tests and for any
// host that wants to inspect/replay the bytes rather than send them.
// Returns false and fills *err on an unsupported Lua value.
bool encode_remote_event(lua_State* L, int instance_index, int first_arg,
                         std::vector<uint8_t>* out, std::string* err);

// Encode a CFrame payload (tag 27 body: position + rotation form). Exposed
// so the parity test can compare against the native's quantized capture.
void encode_cframe_body(const class CFrame& cf, std::vector<uint8_t>* out);

} // namespace rbx
