// cookie.h — one place that turns a stored .ROBLOSECURITY value into the
// Cookie header Roblox expects. Cookie files hold the raw value (no name); a
// caller may also pass an already-named header. Keeping the normalization
// here prevents assetdelivery/thumbnail requests from silently returning 401.
#pragma once

#include <string>

namespace rbxclient {

inline std::string cookie_header(const std::string& cookie) {
    if (cookie.empty())
        return {};
    const size_t semi = cookie.find(';');
    const std::string first =
        semi == std::string::npos ? cookie : cookie.substr(0, semi);
    // Raw values can contain '=' (base64 padding), so only treat the exact
    // named prefix as already-normalized.
    if (first.rfind(".ROBLOSECURITY=", 0) == 0)
        return cookie;
    return ".ROBLOSECURITY=" + cookie;
}

} // namespace rbxclient