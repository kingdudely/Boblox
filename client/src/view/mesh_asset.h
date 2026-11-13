// mesh_asset.h — native mesh (.mesh v2.00) parsing and asset resolution.
//
// The renderer only draws geometry the native client can draw: MeshPart and
// FileMesh/SpecialMesh files use Roblox's "version 2.00\n" container.  The
// parser is GL-free so it can be unit-tested without a window, and the
// fetcher owns the same assetdelivery handshake as the GUI image cache (the
// session cookie is never forwarded to the CDN).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rbxclient {

// Interleaved position(3) normal(3) UV(2).  Vertices stay in the file's
// native coordinates; render_mesh normalises them to a unit cube so a
// MeshPart's replicated Size is authoritative for the rendered extents.
struct MeshAsset {
    std::vector<float> vertices;
    std::vector<uint32_t> indices;
    float min[3] = {0.0f, 0.0f, 0.0f};
    float max[3] = {0.0f, 0.0f, 0.0f};
    bool valid = false;
};

// Parse the native container.  v2 has a 12-byte header after the version
// line, a declared vertex stride (36 or 40) and u32 faces; v5 has a 32-byte
// header, 40-byte vertices, optional skin envelopes and the same u32 face
// table; v6 carries an uncompressed COREMESH chunk; v7 carries a Draco
// COREMESH v2 bitstream.  Unknown layouts are rejected rather than guessed.
bool parse_mesh(const uint8_t* data, size_t size, MeshAsset* out,
                std::string* error = nullptr);

// Load a local mesh path (including a resolved rbxasset:// path).
bool load_mesh_file(const std::string& path, MeshAsset* out,
                    std::string* error = nullptr);

// Resolve an asset URI to bytes.  rbxasset:// is read from the native
// content tree; rbxassetid:// is fetched from assetdelivery with the
// caller's cookie.  Thumbnail URIs are not mesh/texture assets and are
// rejected.  Redirects are followed by hand so the cookie never leaves the
// Roblox asset host.
bool fetch_asset(const std::string& uri, const std::string& cookie,
                 std::vector<uint8_t>* bytes, std::string* error = nullptr);

} // namespace rbxclient