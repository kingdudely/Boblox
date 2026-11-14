// web_http.h — authenticated HTTP transport shared by the runtime's web APIs.
//
// The live client installs its .ROBLOSECURITY cookie once at startup; the
// Player social methods use the same transport as HttpService.  Unit tests
// replace the transport so they never touch the network.
#pragma once

#include <functional>
#include <string>

namespace rbx::web {

struct HttpResponse {
    bool ok = false;
    long status = 0;
    std::string body;
    std::string headers;
    std::string error;
};

using HttpTransport = std::function<bool(
    const std::string& method, const std::string& url, const std::string& body,
    const std::string& headers, HttpResponse* out)>;

// Accept either a raw .ROBLOSECURITY value or a full `Cookie:` value.  The
// setter trims trailing CR/LF because cookie files are usually newline-ended.
void set_session_cookie(std::string cookie);
void clear_session_cookie();
std::string session_cookie_header();
std::string cookie_header(const std::string& raw);

// A test may install a transport; an empty function restores libcurl.
void set_http_transport(HttpTransport transport);
void reset_http_transport();

bool http_request(const std::string& method, const std::string& url,
                  const std::string& body, const std::string& headers,
                  HttpResponse* out);

} // namespace rbx::web