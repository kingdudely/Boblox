// asset_path.h — resolve rbxasset:// URIs against the native content tree.
//
// The Android client ships its content under assets/content; the extracted
// tree at ~/rr/assets has the same layout.  Resolution is deliberately
// conservative: a URI must be relative, contain no ".." component, and its
// final candidate must be a regular file inside the root.  Scripts can put
// arbitrary strings in Content properties, so this is also a path-traversal
// guard, not just a convenience.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

namespace rbxclient {

inline std::filesystem::path asset_root() {
    if (const char* env = std::getenv("RBX_ASSET_ROOT"))
        if (*env)
            return std::filesystem::path(env);
    if (const char* home = std::getenv("HOME")) {
        const std::filesystem::path rr =
            std::filesystem::path(home) / "rr" / "assets";
        if (std::filesystem::is_directory(rr))
            return rr;
    }
    return std::filesystem::path("assets");
}

// Strip "rbxasset://".  False for other schemes or an unsafe path.
inline bool rbxasset_relative(const std::string& uri, std::string* rel) {
    static const std::string prefix = "rbxasset://";
    if (uri.rfind(prefix, 0) != 0)
        return false;
    std::string path = uri.substr(prefix.size());
    while (!path.empty() && (path.front() == '/' || path.front() == '\\'))
        path.erase(path.begin());
    if (path.empty())
        return false;
    // Reject absolute paths and any traversal component.
    if (path.front() == '/' || path.find(':') != std::string::npos)
        return false;
    size_t pos = 0;
    while (pos <= path.size()) {
        const size_t slash = path.find_first_of("/\\", pos);
        const std::string part =
            path.substr(pos, slash == std::string::npos ? std::string::npos
                                                       : slash - pos);
        if (part == "..")
            return false;
        if (slash == std::string::npos)
            break;
        pos = slash + 1;
    }
    *rel = path;
    return true;
}

// Resolve an rbxasset:// URI to an existing file.  The native tree stores
// built-ins directly under content/ (and some under ExtraContent/), so both
// spellings are tried.
inline bool resolve_rbxasset(const std::string& uri,
                             std::filesystem::path* out) {
    std::string rel;
    if (!rbxasset_relative(uri, &rel))
        return false;
    const std::filesystem::path root = asset_root();
    const std::filesystem::path candidates[] = {
        root / rel,
        root / "content" / rel,
        root / "ExtraContent" / rel,
    };
    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec) && !ec) {
            if (out)
                *out = p;
            return true;
        }
    }
    return false;
}

// A compact helper for callers that only want the path.
inline std::string rbxasset_file(const std::string& uri) {
    std::filesystem::path p;
    return resolve_rbxasset(uri, &p) ? p.string() : std::string();
}

} // namespace rbxclient
