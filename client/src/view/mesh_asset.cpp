#include "view/mesh_asset.h"

#include "common/asset_path.h"
#include "common/cookie.h"

#include <curl/curl.h>
#include <zlib.h>

#include <draco/compression/decode.h>
#include <draco/mesh/mesh.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

namespace rbxclient {

namespace {

constexpr char kMagicV2[] = "version 2.00\n";
constexpr char kMagicV5[] = "version 5.00\n";
constexpr char kMagicV6[] = "version 6.00\n";
constexpr char kMagicV7[] = "version 7.00\n";
constexpr size_t kMagicBytes = sizeof(kMagicV2) - 1; // both versions: 13
constexpr size_t kMaxVertexBytes = 40;

uint16_t read_u16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

uint32_t read_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

float read_f32(const uint8_t* p) {
    uint32_t bits = read_u32(p);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void fail(std::string* error, const char* text) {
    if (error)
        *error = text;
}

bool digits_only(const std::string& text) {
    if (text.empty())
        return false;
    for (char c : text)
        if (c < '0' || c > '9')
            return false;
    return true;
}

bool asset_id_url(const std::string& uri, std::string* url) {
    constexpr char prefix[] = "rbxassetid://";
    if (uri.rfind(prefix, 0) != 0)
        return false;
    const std::string id = uri.substr(sizeof(prefix) - 1);
    if (!digits_only(id))
        return false;
    *url = "https://assetdelivery.roblox.com/v1/asset/?id=" + id;
    return true;
}

// Join scripts and replicated Content values use all three spellings for
// the same asset.  Normalise the website form to the delivery endpoint so a
// MeshPart does not depend on which one the server happened to send.
bool website_asset_url(const std::string& uri, std::string* url) {
    const size_t id_at = uri.find("id=");
    if (uri.rfind("https://www.roblox.com/asset/", 0) != 0 &&
        uri.rfind("http://www.roblox.com/asset/", 0) != 0)
        return false;
    if (id_at == std::string::npos)
        return false;
    const std::string id = uri.substr(id_at + 3);
    if (!digits_only(id))
        return false;
    *url = "https://assetdelivery.roblox.com/v1/asset/?id=" + id;
    return true;
}

bool roblox_asset_host(const std::string& url) {
    return url.rfind("https://assetdelivery.roblox.com/", 0) == 0;
}

bool gunzip(const std::vector<uint8_t>& in, std::vector<uint8_t>* out,
            std::string* error) {
    out->clear();
    if (in.size() < 2 || in[0] != 0x1f || in[1] != 0x8b) {
        *out = in;
        return true;
    }
    z_stream stream{};
    if (inflateInit2(&stream, 15 + 16) != Z_OK) {
        fail(error, "asset: zlib init failed");
        return false;
    }
    stream.next_in = const_cast<Bytef*>(in.data());
    stream.avail_in = uInt(in.size());
    out->resize(std::max<size_t>(in.size() * 4, 4096));
    int rc = Z_OK;
    while (rc == Z_OK) {
        if (stream.total_out == out->size())
            out->resize(out->size() * 2);
        stream.next_out = out->data() + stream.total_out;
        stream.avail_out = uInt(out->size() - stream.total_out);
        rc = inflate(&stream, Z_NO_FLUSH);
        if (rc == Z_OK && stream.avail_in == 0 && stream.avail_out > 0)
            rc = Z_BUF_ERROR;
    }
    out->resize(stream.total_out);
    inflateEnd(&stream);
    if (rc != Z_STREAM_END) {
        fail(error, "asset: gzip stream is truncated");
        out->clear();
        return false;
    }
    return true;
}

struct FetchBuffer {
    std::vector<uint8_t> bytes;
    bool overflow = false;
    static constexpr size_t kLimit = 64 * 1024 * 1024;
};

size_t curl_write(char* data, size_t size, size_t count, void* user) {
    FetchBuffer* body = static_cast<FetchBuffer*>(user);
    const size_t n = size * count;
    if (body->bytes.size() + n > FetchBuffer::kLimit) {
        body->overflow = true;
        return 0;
    }
    body->bytes.insert(body->bytes.end(), (uint8_t*)data, (uint8_t*)data + n);
    return n;
}

bool fetch_once(const std::string& url, const std::string& cookie,
                FetchBuffer* body, long* status, std::string* redirect,
                std::string* error) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        fail(error, "asset request: curl init failed");
        return false;
    }
    struct curl_slist* headers = nullptr;
    if (!cookie.empty() && roblox_asset_host(url))
        headers = curl_slist_append(
            headers, ("Cookie: " + cookie_header(cookie)).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
    char* location = nullptr;
    curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
    if (location)
        *redirect = location;
    if (headers)
        curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        if (error)
            *error = std::string("asset request failed: ") +
                     curl_easy_strerror(rc) +
                     (body->overflow ? " (asset too large)" : "");
        return false;
    }
    return true;
}

// Shared vertex/face reader for the v2 and v5 containers.  Both store
// position at +0, normal at +12 and UV at +24 in 36/40-byte records; v5 may
// insert per-vertex skin envelopes before the face table.
bool read_mesh_payload(const uint8_t* data, size_t size, size_t start,
                       uint32_t vertex_count, uint32_t face_count,
                       size_t vertex_stride, size_t skin_bytes, MeshAsset* out,
                       std::string* error) {
    if (vertex_stride < 32 || vertex_stride > kMaxVertexBytes)
        return false;
    if (vertex_count == 0 || face_count == 0 ||
        vertex_count > std::numeric_limits<uint32_t>::max() / vertex_stride)
        return false;
    const size_t vertex_bytes = size_t(vertex_count) * vertex_stride;
    const size_t index_bytes = size_t(face_count) * 3 * sizeof(uint32_t);
    if (start > size || vertex_bytes > size - start ||
        skin_bytes > size - start - vertex_bytes ||
        index_bytes > size - start - vertex_bytes - skin_bytes) {
        fail(error, "mesh: truncated vertex or index data");
        return false;
    }

    MeshAsset mesh;
    mesh.vertices.reserve(size_t(vertex_count) * 8);
    mesh.indices.reserve(size_t(face_count) * 3);
    mesh.min[0] = mesh.min[1] = mesh.min[2] =
        std::numeric_limits<float>::max();
    mesh.max[0] = mesh.max[1] = mesh.max[2] =
        std::numeric_limits<float>::lowest();
    const uint8_t* vertex = data + start;
    for (uint32_t i = 0; i < vertex_count; ++i, vertex += vertex_stride) {
        for (int c = 0; c < 3; ++c) {
            const float p = read_f32(vertex + c * 4);
            mesh.min[c] = std::min(mesh.min[c], p);
            mesh.max[c] = std::max(mesh.max[c], p);
            mesh.vertices.push_back(p);
        }
        // Position, normal and UV are the fields the renderer needs; the
        // remaining floats are tangent/color/padding.
        for (int c = 3; c < 8; ++c)
            mesh.vertices.push_back(read_f32(vertex + c * 4));
    }
    const uint8_t* indices =
        data + start + vertex_bytes + skin_bytes;
    for (uint32_t i = 0; i < face_count * 3; ++i) {
        const uint32_t index = read_u32(indices + size_t(i) * 4);
        if (index >= vertex_count) {
            fail(error, "mesh: index is out of range");
            return false;
        }
        mesh.indices.push_back(index);
    }
    mesh.valid = true;
    *out = std::move(mesh);
    return true;
}

bool decode_draco_mesh(const uint8_t* data, size_t size, MeshAsset* out,
                       std::string* error) {
    draco::DecoderBuffer buffer;
    buffer.Init(reinterpret_cast<const char*>(data), size);
    draco::Decoder decoder;
    auto decoded = decoder.DecodeMeshFromBuffer(&buffer);
    if (!decoded.ok()) {
        fail(error, "mesh: Draco decode failed");
        return false;
    }
    std::unique_ptr<draco::Mesh> mesh = std::move(decoded).value();
    if (!mesh || mesh->num_faces() == 0 || mesh->num_points() == 0) {
        fail(error, "mesh: Draco mesh has no geometry");
        return false;
    }

    const draco::PointAttribute* position =
        mesh->GetNamedAttribute(draco::GeometryAttribute::POSITION);
    const draco::PointAttribute* normal =
        mesh->GetNamedAttribute(draco::GeometryAttribute::NORMAL);
    const draco::PointAttribute* uv =
        mesh->GetNamedAttribute(draco::GeometryAttribute::TEX_COORD);
    if (!position || !normal || !uv) {
        // Roblox's Draco stream omits attribute names.  Position is the
        // first 3-component float attribute; the next 3-component one is the
        // normal and the 2-component one is UV.
        for (int i = 0; i < mesh->num_attributes(); ++i) {
            const draco::PointAttribute* a = mesh->attribute(i);
            if (!a || a->data_type() != draco::DT_FLOAT32)
                continue;
            if (a == position || a == normal || a == uv)
                continue;
            if (!position && a->num_components() == 3) {
                position = a;
            } else if (a->num_components() == 3 && !normal) {
                normal = a;
            } else if (a->num_components() == 2 && !uv) {
                uv = a;
            }
        }
    }
    if (!position) {
        fail(error, "mesh: Draco mesh has no position attribute");
        return false;
    }

    MeshAsset result;
    result.indices.reserve(size_t(mesh->num_faces()) * 3);
    result.vertices.reserve(size_t(mesh->num_faces()) * 3 * 8);
    result.min[0] = result.min[1] = result.min[2] =
        std::numeric_limits<float>::max();
    result.max[0] = result.max[1] = result.max[2] =
        std::numeric_limits<float>::lowest();
    uint32_t next = 0;
    for (draco::FaceIndex f(0); f < mesh->num_faces(); ++f) {
        const draco::Mesh::Face& face = mesh->face(f);
        for (int corner = 0; corner < 3; ++corner) {
            const draco::PointIndex point = face[corner];
            float p[3] = {0.0f, 0.0f, 0.0f};
            float n[3] = {0.0f, 0.0f, 1.0f};
            float t[2] = {0.0f, 0.0f};
            position->GetMappedValue(point, p);
            if (normal)
                normal->GetMappedValue(point, n);
            if (uv)
                uv->GetMappedValue(point, t);
            for (int c = 0; c < 3; ++c) {
                result.min[c] = std::min(result.min[c], p[c]);
                result.max[c] = std::max(result.max[c], p[c]);
            }
            result.vertices.insert(result.vertices.end(),
                                   {p[0], p[1], p[2], n[0], n[1], n[2],
                                    t[0], t[1]});
            result.indices.push_back(next++);
        }
    }
    result.valid = true;
    *out = std::move(result);
    return true;
}

} // namespace

bool parse_mesh(const uint8_t* data, size_t size, MeshAsset* out,
                std::string* error) {
    if (out)
        *out = MeshAsset{};
    if (!data || !out) {
        fail(error, "mesh: empty input");
        return false;
    }
    if (size < kMagicBytes || std::memcmp(data, kMagicV2, kMagicBytes) == 0) {
        if (size < kMagicBytes + 12) {
            fail(error, "mesh: truncated v2 header");
            return false;
        }
        const uint8_t* head = data + kMagicBytes;
        const uint16_t header_size = read_u16(head);
        const size_t vertex_stride = head[2];
        const size_t face_stride = head[3];
        if (header_size != 12 || face_stride != 12) {
            fail(error, "mesh: unsupported v2 header");
            return false;
        }
        return read_mesh_payload(data, size, kMagicBytes + header_size,
                                 read_u32(head + 4), read_u32(head + 8),
                                 vertex_stride, 0, out, error);
    }
    if (std::memcmp(data, kMagicV5, kMagicBytes) != 0) {
        if (std::memcmp(data, kMagicV6, kMagicBytes) == 0) {
            if (size < kMagicBytes + 33 ||
                std::memcmp(data + kMagicBytes, "COREMESH", 8) != 0 ||
                read_u32(data + kMagicBytes + 8) != 1) {
                fail(error, "mesh: unsupported v6 chunk");
                return false;
            }
            const uint32_t vertex_count =
                read_u32(data + kMagicBytes + 16);
            const size_t faces_at = kMagicBytes + 20 +
                                    size_t(vertex_count) * 40;
            if (faces_at + 4 > size) {
                fail(error, "mesh: truncated v6 face count");
                return false;
            }
            return read_mesh_payload(data, size, kMagicBytes + 20,
                                     vertex_count,
                                     read_u32(data + faces_at), 40, 4, out,
                                     error);
        }
        if (std::memcmp(data, kMagicV7, kMagicBytes) == 0) {
            if (size < kMagicBytes + 33 ||
                std::memcmp(data + kMagicBytes, "COREMESH", 8) != 0 ||
                read_u32(data + kMagicBytes + 8) != 2) {
                fail(error, "mesh: unsupported v7 chunk");
                return false;
            }
            const uint32_t blob_size =
                read_u32(data + kMagicBytes + 16);
            if (blob_size > size - kMagicBytes - 20) {
                fail(error, "mesh: truncated v7 Draco blob");
                return false;
            }
            return decode_draco_mesh(data + kMagicBytes + 20, blob_size, out,
                                     error);
        }
        fail(error, "mesh: not a version 2.00/5.00/6.00 container");
        return false;
    }
    if (size < kMagicBytes + 32) {
        fail(error, "mesh: truncated v5 header");
        return false;
    }
    const uint8_t* head = data + kMagicBytes;
    const uint16_t header_size = read_u16(head);
    const uint16_t num_bones = read_u16(head + 14);
    if (header_size != 32) {
        fail(error, "mesh: unsupported v5 header");
        return false;
    }
    const uint32_t vertex_count = read_u32(head + 4);
    const uint32_t face_count = read_u32(head + 8);
    const size_t skin_bytes =
        num_bones ? size_t(vertex_count) * 8 : 0;
    return read_mesh_payload(data, size, kMagicBytes + header_size,
                             vertex_count, face_count, 40, skin_bytes, out,
                             error);
}

bool load_mesh_file(const std::string& path, MeshAsset* out,
                    std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error)
            *error = "mesh: cannot open " + path;
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    return parse_mesh(bytes.data(), bytes.size(), out, error);
}

bool fetch_asset(const std::string& uri, const std::string& cookie,
                 std::vector<uint8_t>* bytes, std::string* error) {
    if (bytes)
        bytes->clear();
    if (!bytes) {
        fail(error, "asset: no output buffer");
        return false;
    }
    std::string file;
    if (uri.rfind("rbxasset://", 0) == 0)
        file = rbxasset_file(uri);
    else if (uri.find("://") == std::string::npos)
        file = uri;
    if (!file.empty()) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            fail(error, "asset: cannot open local file");
            return false;
        }
        std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
        return gunzip(raw, bytes, error) && !bytes->empty();
    }

    std::string url;
    if (website_asset_url(uri, &url)) {
        // normalised above
    } else if (uri.rfind("http://", 0) == 0 ||
               uri.rfind("https://", 0) == 0)
        url = uri;
    else if (!asset_id_url(uri, &url)) {
        if (error)
            *error = "asset: unsupported URI '" + uri + "'";
        return false;
    }
    std::string current = url;
    for (int hop = 0; hop < 6; ++hop) {
        FetchBuffer body;
        long status = 0;
        std::string redirect;
        if (!fetch_once(current, cookie, &body, &status, &redirect, error))
            return false;
        if (status >= 300 && status < 400) {
            if (redirect.empty()) {
                fail(error, "asset: redirect had no location");
                return false;
            }
            current = redirect;
            continue;
        }
        if (status < 200 || status >= 300) {
            if (error)
                *error = "asset: HTTP " + std::to_string(status);
            return false;
        }
        if (body.bytes.empty()) {
            fail(error, "asset: empty response");
            return false;
        }
        if (!gunzip(body.bytes, bytes, error))
            return false;
        return !bytes->empty();
    }
    fail(error, "asset: redirect chain was too long");
    return false;
}

} // namespace rbxclient
