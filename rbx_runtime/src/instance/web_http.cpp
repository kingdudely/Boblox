#include "instance/web_http.h"

#include <curl/curl.h>

#include <utility>

namespace rbx::web {
namespace {

std::string g_cookie;
HttpTransport g_transport;

size_t write_body(char* p, size_t sz, size_t n, void* ud) {
    auto* out = static_cast<std::string*>(ud);
    out->append(p, sz * n);
    return sz * n;
}

bool curl_request(const std::string& method, const std::string& url,
                  const std::string& body, const std::string& headers,
                  HttpResponse* out) {
    CURL* c = curl_easy_init();
    if (!c) {
        out->error = "curl_easy_init failed";
        return false;
    }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Roblox/WinInet");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out->body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, write_body);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &out->headers);
    struct curl_slist* hs = nullptr;
    if (!headers.empty()) {
        std::string line;
        for (size_t i = 0; i <= headers.size(); ++i) {
            if (i == headers.size() || headers[i] == '\n') {
                if (!line.empty()) hs = curl_slist_append(hs, line.c_str());
                line.clear();
            } else if (headers[i] != '\r') {
                line.push_back(headers[i]);
            }
        }
        if (hs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hs);
    }
    if (method == "POST" || method == "PUT" || method == "PATCH") {
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    } else if (method != "GET") {
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
    }
    const CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &out->status);
        out->ok = true;
    } else {
        out->error = curl_easy_strerror(rc);
    }
    if (hs) curl_slist_free_all(hs);
    curl_easy_cleanup(c);
    return out->ok;
}

} // namespace

void set_session_cookie(std::string cookie) {
    while (!cookie.empty() && (cookie.back() == '\n' || cookie.back() == '\r'))
        cookie.pop_back();
    g_cookie = std::move(cookie);
}

void clear_session_cookie() { g_cookie.clear(); }

std::string cookie_header(const std::string& raw) {
    if (raw.empty()) return "";
    size_t start = 0;
    if (raw.rfind("Cookie:", 0) == 0) {
        start = 7;
        while (start < raw.size() && raw[start] == ' ') ++start;
    }
    const std::string value = raw.substr(start);
    // A stored file normally contains only the raw token.  A caller may also
    // provide an already-formed cookie header; do not prefix that twice.
    // Raw tokens can contain '=' (base64 padding), so only the exact named
    // prefix counts as already-normalized.
    const size_t semi = value.find(';');
    const std::string first =
        semi == std::string::npos ? value : value.substr(0, semi);
    if (first.rfind(".ROBLOSECURITY=", 0) == 0) return value;
    return ".ROBLOSECURITY=" + value;
}

std::string session_cookie_header() { return cookie_header(g_cookie); }

void set_http_transport(HttpTransport transport) {
    g_transport = std::move(transport);
}

void reset_http_transport() { g_transport = HttpTransport{}; }

bool http_request(const std::string& method, const std::string& url,
                  const std::string& body, const std::string& headers,
                  HttpResponse* out) {
    if (!out) return false;
    *out = HttpResponse{};
    if (g_transport) return g_transport(method, url, body, headers, out);
    return curl_request(method, url, body, headers, out);
}

} // namespace rbx::web