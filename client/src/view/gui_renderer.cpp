// gui_renderer.cpp — see gui_renderer.h.
#include "view/gui_renderer.h"

#include "common/asset_path.h"
#include "common/cookie.h"
#include "gui/gui_font_metrics.h"

#define GL_GLEXT_PROTOTYPES 1
#include <GL/glcorearb.h>

#define NANOVG_GL3_IMPLEMENTATION
#include "nanovg.h"
#include "nanovg_gl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

#include <curl/curl.h>

namespace rbxclient {

namespace {

constexpr size_t kMaxGuiAssetBytes = 32u * 1024u * 1024u;

bool load_file(const std::string& path, std::vector<unsigned char>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    out->assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
    return !out->empty();
}

struct FetchBuffer {
    std::vector<unsigned char> bytes;
    bool overflow = false;
};

size_t curl_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<FetchBuffer*>(userdata);
    const size_t n = size * nmemb;
    if (n > kMaxGuiAssetBytes || out->bytes.size() > kMaxGuiAssetBytes - n) {
        out->overflow = true;
        return 0;
    }
    out->bytes.insert(out->bytes.end(),
                      reinterpret_cast<unsigned char*>(ptr),
                      reinterpret_cast<unsigned char*>(ptr) + n);
    return n;
}

bool digits_only(const std::string& value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return c >= '0' && c <= '9';
           });
}

std::string query_value(const std::string& query, const std::string& key) {
    size_t at = 0;
    while (at <= query.size()) {
        const size_t end = query.find('&', at);
        const std::string part = query.substr(
            at, end == std::string::npos ? std::string::npos : end - at);
        const size_t eq = part.find('=');
        if (eq != std::string::npos && part.substr(0, eq) == key)
            return part.substr(eq + 1);
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    return {};
}

// The native GUI accepts asset ids and the two URI forms below.  Asset
// delivery is fetched without blocking NanoVG; rbxthumb uses the public
// thumbnail image endpoints so the same asynchronous path handles both.
bool remote_url(const std::string& uri, std::string* url) {
    if (uri.rfind("rbxassetid://", 0) == 0) {
        const std::string id = uri.substr(std::strlen("rbxassetid://"));
        if (!digits_only(id))
            return false;
        *url = "https://assetdelivery.roblox.com/v1/asset/?id=" + id;
        return true;
    }
    if (uri.rfind("rbxthumb://", 0) != 0)
        return false;
    const std::string query = uri.substr(std::strlen("rbxthumb://"));
    const std::string type = query_value(query, "type");
    std::string id = query_value(query, "id");
    if (id.empty())
        id = query_value(query, "assetId");
    if (!digits_only(id))
        return false;
    const std::string w = query_value(query, "w");
    const std::string h = query_value(query, "h");
    const std::string width = digits_only(w) ? w : "150";
    const std::string height = digits_only(h) ? h : "150";
    if (type == "Asset") {
        *url = "https://thumbnails.roblox.com/v1/assets?assetIds=" + id +
               "&size=" + width + "x" + height + "&format=Png";
        return true;
    }
    if (type == "Avatar" || type == "AvatarBust" ||
        type == "AvatarFullBody" || type == "AvatarHeadShot") {
        const char* endpoint =
            type == "AvatarHeadShot"
                ? "https://thumbnails.roblox.com/v1/users/avatar-headshot?userIds="
            : type == "AvatarBust"
                ? "https://thumbnails.roblox.com/v1/users/avatar-bust?userIds="
                : "https://thumbnails.roblox.com/v1/users/avatar?userIds=";
        *url = std::string(endpoint) + id + "&size=" + width + "x" + height +
               "&format=Png&isCircular=false";
        return true;
    }
    return false;
}

std::string extract_image_url(const std::vector<unsigned char>& body) {
    const std::string text(body.begin(), body.end());
    // Thumbnails API JSON.  This is deliberately a narrow field extraction,
    // not a general JSON parser: only the imageUrl value is needed.
    size_t at = text.find("\"imageUrl\"");
    if (at != std::string::npos) {
        at = text.find(':', at);
        at = at == std::string::npos ? at : text.find('"', at);
        if (at != std::string::npos) {
            const size_t end = text.find('"', at + 1);
            if (end != std::string::npos)
                return text.substr(at + 1, end - at - 1);
        }
    }
    return {};
}

bool is_roblox_asset_host(const std::string& url) {
    return url.rfind("https://assetdelivery.roblox.com/", 0) == 0;
}

bool fetch_once(const std::string& url, const std::string& cookie,
                FetchBuffer* body, long* status, std::string* redirect,
                std::string* why) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (why) *why = "curl init failed";
        return false;
    }
    struct curl_slist* headers = nullptr;
    if (!cookie.empty() && is_roblox_asset_host(url))
        headers = curl_slist_append(
            headers, ("Cookie: " + cookie_header(cookie)).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Boblox/1.0");
    const CURLcode rc = curl_easy_perform(curl);
    char* location = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
    curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
    if (location) *redirect = location;
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        if (why)
            *why = std::string("asset request failed: ") +
                   curl_easy_strerror(rc) +
                   (body->overflow ? " (image too large)" : "");
        return false;
    }
    return true;
}

// Follow a bounded redirect chain by hand.  The cookie is added by fetch_once
// only for assetdelivery, so CDN hops never receive the session.
bool fetch_follow(const std::string& start, const std::string& cookie,
                  FetchBuffer* body, std::string* why) {
    std::string url = start;
    for (int hop = 0; hop < 6; ++hop) {
        body->bytes.clear();
        body->overflow = false;
        long status = 0;
        std::string redirect;
        if (!fetch_once(url, cookie, body, &status, &redirect, why))
            return false;
        if (status >= 300 && status < 400) {
            if (redirect.empty()) {
                if (why) *why = "asset redirect had no location";
                return false;
            }
            url = redirect;
            continue;
        }
        if (status < 200 || status >= 300) {
            if (why)
                *why = "asset request returned HTTP " + std::to_string(status);
            return false;
        }
        return true;
    }
    if (why) *why = "asset redirect chain was too long";
    return false;
}

bool fetch_remote(const std::string& uri, const std::string& cookie,
                  std::vector<unsigned char>* bytes, std::string* why) {
    std::string url;
    if (!remote_url(uri, &url)) {
        if (why) *why = "unsupported image URI '" + uri + "'";
        return false;
    }
    FetchBuffer body;
    if (!fetch_follow(url, cookie, &body, why))
        return false;
    if (body.bytes.empty()) {
        if (why) *why = "asset response was empty";
        return false;
    }
    // Thumbnail APIs return JSON whose imageUrl is the actual image.  Follow
    // that second hop through the same resolver (CDN hops still get no
    // cookie).  A non-image asset (for example a Decal RBXMX) stays textual
    // and fails the NanoVG decode, so IsLoaded remains false like native.
    const bool textual = body.bytes.size() > 1 &&
                         (body.bytes[0] == '{' || body.bytes[0] == '<');
    std::string nested =
        textual ? extract_image_url(body.bytes) : std::string();
    if (!nested.empty()) {
        FetchBuffer image;
        if (!fetch_follow(nested, cookie, &image, why))
            return false;
        if (image.bytes.empty()) {
            if (why) *why = "image response was empty";
            return false;
        }
        body.bytes = std::move(image.bytes);
    }
    *bytes = std::move(body.bytes);
    return true;
}

} // namespace

struct GuiRenderer::AssetRequest {
    std::mutex mutex;
    bool done = false;
    std::vector<unsigned char> bytes;
    std::string error;
};

namespace {

// The native tree ships Arimo (Legacy/Arial) and SourceSansPro (SourceSans).
// Fall back to any DejaVu present on the host so a missing asset never
// disables the whole GUI.
std::string font_path(const char* asset, bool bold) {
    const std::string p =
        rbxasset_file(std::string("rbxasset://") + asset);
    if (!p.empty())
        return p;
    const char* system_paths[] = {
        bold ? "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
             : "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    for (const char* path : system_paths)
        if (std::ifstream(path).good())
            return path;
    return {};
}

NVGcolor nvg_color(const rbx::Color3& c, double alpha) {
    return nvgRGBAf(float(std::clamp(c.r, 0.0, 1.0)),
                    float(std::clamp(c.g, 0.0, 1.0)),
                    float(std::clamp(c.b, 0.0, 1.0)),
                    float(std::clamp(alpha, 0.0, 1.0)));
}

void apply_transform(NVGcontext* vg, const GuiDrawCommand& c) {
    if (std::abs(c.rotation) > 1e-6) {
        const float cx = float(c.x + c.w * 0.5);
        const float cy = float(c.y + c.h * 0.5);
        nvgTranslate(vg, cx, cy);
        nvgRotate(vg, float(c.rotation));
        nvgTranslate(vg, -cx, -cy);
    }
}

void apply_clip(NVGcontext* vg, const GuiDrawCommand& c) {
    if (c.clip_w > 0.0 && c.clip_h > 0.0)
        nvgIntersectScissor(vg, float(c.clip_x), float(c.clip_y),
                            float(c.clip_w), float(c.clip_h));
}

} // namespace

GuiRenderer::GuiRenderer() = default;

GuiRenderer::~GuiRenderer() { shutdown(); }

bool GuiRenderer::init(std::string* err, const std::string& cookie) {
    if (vg_)
        return true;
    cookie_ = cookie;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        if (err)
            *err = "curl global init failed";
        return false;
    }
    vg_ = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!vg_) {
        if (err)
            *err = "NanoVG GL3 init failed";
        return false;
    }

    auto load_face = [&](const char* asset, bool bold,
                         std::vector<unsigned char>* data, const char* name,
                         int* out) {
        const std::string path = font_path(asset, bold);
        if (path.empty())
            return;
        if (load_file(path, data))
            *out = nvgCreateFontMem(vg_, name, data->data(), int(data->size()),
                                    0);
        if (*out < 0)
            *out = nvgCreateFont(vg_, name, path.c_str());
    };
    load_face("fonts/SourceSansPro-Regular.ttf", false, &regular_data_,
              "sans", &font_regular_);
    load_face("fonts/SourceSansPro-Bold.ttf", true, &bold_data_,
              "sans-bold", &font_bold_);
    load_face("fonts/Arimo-Regular.ttf", false, &arimo_regular_data_,
              "arimo", &font_arimo_regular_);
    load_face("fonts/Arimo-Bold.ttf", true, &arimo_bold_data_,
              "arimo-bold", &font_arimo_bold_);
    if (font_bold_ < 0)
        font_bold_ = font_regular_;
    if (font_arimo_regular_ < 0)
        font_arimo_regular_ = font_regular_;
    if (font_arimo_bold_ < 0)
        font_arimo_bold_ = font_bold_;
    setGuiImageLoadedQuery([this](const std::string& uri) {
        const auto it = images_.find(uri);
        if (it != images_.end())
            return it->second > 0;
        if (uri.rfind("rbxassetid://", 0) == 0 ||
            uri.rfind("rbxthumb://", 0) == 0)
            return false; // the asynchronous fetch has not completed
        std::string file = rbxasset_file(uri);
        if (file.empty())
            file = uri;
        return std::ifstream(file).good();
    });
    return true;
}

int GuiRenderer::faceFont(int face) const {
    switch (font_metrics::normalizedFace(face)) {
    case rbxclient::font_metrics::kArial:
        return font_arimo_regular_;
    case rbxclient::font_metrics::kArialBold:
        return font_arimo_bold_;
    case rbxclient::font_metrics::kSourceSans:
        return font_regular_;
    case rbxclient::font_metrics::kSourceSansBold:
        return font_bold_;
    case rbxclient::font_metrics::kLegacy:
    default:
        return font_arimo_regular_;
    }
}

void GuiRenderer::shutdown() {
    setGuiImageLoadedQuery({});
    // Requests own their result; a detached fetch may finish after the GL
    // context is gone, so it never touches NanoVG.  Dropping the request
    // table here lets that thread exit without a dangling renderer.
    pending_.clear();
    images_.clear();
    regular_data_.clear();
    bold_data_.clear();
    arimo_regular_data_.clear();
    arimo_bold_data_.clear();
    if (vg_) {
        nvgDeleteGL3(vg_);
        vg_ = nullptr;
    }
    font_regular_ = font_bold_ = -1;
    font_arimo_regular_ = font_arimo_bold_ = -1;
}

void GuiRenderer::start_asset(const std::string& path) {
    if (!vg_ || pending_.count(path) || images_.count(path))
        return;
    auto request = std::make_shared<AssetRequest>();
    pending_[path] = request;
    const std::string cookie = cookie_;
    std::thread([request, path, cookie] {
        std::vector<unsigned char> bytes;
        std::string error;
        const bool ok = fetch_remote(path, cookie, &bytes, &error);
        std::lock_guard<std::mutex> lock(request->mutex);
        request->done = true;
        request->bytes = std::move(bytes);
        request->error = std::move(error);
        if (!ok && request->error.empty())
            request->error = "image fetch failed";
    }).detach();
}

void GuiRenderer::pump_assets() {
    if (!vg_ || pending_.empty())
        return;
    for (auto it = pending_.begin(); it != pending_.end();) {
        std::vector<unsigned char> bytes;
        std::string error;
        {
            std::lock_guard<std::mutex> lock(it->second->mutex);
            if (!it->second->done) {
                ++it;
                continue;
            }
            bytes = std::move(it->second->bytes);
            error = std::move(it->second->error);
        }
        if (error.empty() && !bytes.empty()) {
            const int id = nvgCreateImageMem(vg_, 0, bytes.data(),
                                             int(bytes.size()));
            if (id > 0) {
                images_[it->first] = id;
            } else {
                images_[it->first] = -1;
                std::fprintf(stderr, "gui: cannot decode image %s\n",
                             it->first.c_str());
            }
        } else {
            images_[it->first] = -1;
            std::fprintf(stderr, "gui: %s: %s\n", it->first.c_str(),
                         error.c_str());
        }
        it = pending_.erase(it);
    }
}

int GuiRenderer::image(const std::string& path) {
    if (!vg_ || path.empty())
        return 0;
    const auto it = images_.find(path);
    if (it != images_.end())
        return it->second > 0 ? it->second : 0;
    if (path.rfind("rbxassetid://", 0) == 0 ||
        path.rfind("rbxthumb://", 0) == 0) {
        // Native image loading is asynchronous.  Kick off the fetch here and
        // draw the label's background until pump_assets() has a texture.
        start_asset(path);
        return 0;
    }
    std::string file = rbxasset_file(path);
    if (file.empty())
        file = path; // a direct local path remains supported
    if (file.empty())
        return 0;
    const int id = nvgCreateImage(vg_, file.c_str(), 0);
    images_[path] = id;
    return id;
}

void GuiRenderer::draw(const std::vector<GuiDrawCommand>& commands, int width,
                       int height) {
    if (!vg_ || commands.empty() || width <= 0 || height <= 0)
        return;
    pump_assets();
    // The caller binds the render target (the 3D pass's multisample FBO);
    // NanoVG must draw into that same target so --shot captures the GUI too.
    nvgBeginFrame(vg_, float(width), float(height), 1.0f);
    nvgSave(vg_);
    nvgResetScissor(vg_);

    for (const GuiDrawCommand& c : commands) {
        if (c.w <= 0.0 || c.h <= 0.0)
            continue;
        nvgSave(vg_);
        apply_clip(vg_, c);
        apply_transform(vg_, c);
        switch (c.kind) {
        case GuiKind::Rect: {
            nvgBeginPath(vg_);
            if (c.corner_radius > 0.0)
                nvgRoundedRect(vg_, float(c.x), float(c.y), float(c.w),
                               float(c.h), float(c.corner_radius));
            else
                nvgRect(vg_, float(c.x), float(c.y), float(c.w), float(c.h));
            if (c.background_alpha > 0.001) {
                nvgFillColor(vg_, nvg_color(c.background, c.background_alpha));
                nvgFill(vg_);
            }
            if (c.border_width > 0.0) {
                nvgStrokeWidth(vg_, float(c.border_width));
                nvgStrokeColor(vg_, nvg_color(c.border, c.border_alpha));
                nvgStroke(vg_);
            }
            // Native AutoButtonColor feedback.  The press tint is darker so
            // a held button is distinguishable from hover alone.
            if (c.pressed) {
                nvgBeginPath(vg_);
                nvgRect(vg_, float(c.x), float(c.y), float(c.w), float(c.h));
                nvgFillColor(vg_, nvgRGBAf(0.0f, 0.0f, 0.0f, 0.16f));
                nvgFill(vg_);
            } else if (c.hovered) {
                nvgBeginPath(vg_);
                nvgRect(vg_, float(c.x), float(c.y), float(c.w), float(c.h));
                nvgFillColor(vg_, nvgRGBAf(1.0f, 1.0f, 1.0f, 0.12f));
                nvgFill(vg_);
            }
            break;
        }
        case GuiKind::Text: {
            const int font = faceFont(c.font);
            if (font < 0)
                break;
            nvgFontFaceId(vg_, font);
            const double glyph_size =
                c.text_size * font_metrics::pixelScale(c.font);
            float size = float(glyph_size);
            if (c.scaled) {
                // Fit the box: height first, then shrink to width.
                size = float(std::max(6.0, c.h * 0.78));
                float bounds[4];
                nvgFontSize(vg_, size);
                nvgTextBounds(vg_, 0, 0, c.text.c_str(), nullptr, bounds);
                const float tw = bounds[2] - bounds[0];
                if (tw > 0.0f && tw > c.w * 0.96f)
                    size *= float(c.w * 0.96 / tw);
            }
            nvgFontSize(vg_, size);
            int align = NVG_ALIGN_LEFT;
            if (c.h_align == 1)
                align = NVG_ALIGN_CENTER;
            else if (c.h_align == 2)
                align = NVG_ALIGN_RIGHT;
            if (c.v_align == 0)
                align |= NVG_ALIGN_TOP;
            else if (c.v_align == 2)
                align |= NVG_ALIGN_BOTTOM;
            else
                align |= NVG_ALIGN_MIDDLE;
            nvgTextAlign(vg_, align);
            float x = float(c.x);
            if (c.h_align == 1)
                x = float(c.x + c.w * 0.5);
            else if (c.h_align == 2)
                x = float(c.x + c.w);
            float y = float(c.y + c.h * 0.5);
            if (c.v_align == 0)
                y = float(c.y);
            else if (c.v_align == 2)
                y = float(c.y + c.h);
            if (c.focused && !c.placeholder) {
                // Selection and caret are painted from the same glyph metrics
                // as the text, in the same alignment space.
                const std::string& text = c.text;
                const auto offsets = [&] {
                    std::vector<size_t> v;
                    for (size_t i = 0; i < text.size();) {
                        v.push_back(i);
                        ++i;
                        while (i < text.size() &&
                               (static_cast<unsigned char>(text[i]) & 0xC0) ==
                                   0x80)
                            ++i;
                    }
                    v.push_back(text.size());
                    return v;
                }();
                const int ncp = int(offsets.size()) - 1;
                const int caret = std::clamp(c.caret, 0, ncp);
                const int anchor = std::clamp(c.selection_start, 0, ncp);
                const int from = std::min(caret, anchor);
                const int to = std::max(caret, anchor);
                auto prefix_width = [&](int cp) {
                    const std::string part =
                        text.substr(0, offsets[size_t(cp)]);
                    return float(font_metrics::measure(part, c.text_size,
                                                      c.font)
                                     .x);
                };
                const float start_x =
                    c.h_align == 1   ? float(c.x) + float(c.w) * 0.5f -
                                           prefix_width(ncp) * 0.5f
                    : c.h_align == 2 ? float(c.x + c.w) - prefix_width(ncp)
                                     : float(c.x);
                const float top = float(c.y) + std::max(1.0f, float(size) * 0.08f);
                if (to > from) {
                    const float sx = start_x + prefix_width(from);
                    const float ex = start_x + prefix_width(to);
                    nvgBeginPath(vg_);
                    nvgRect(vg_, sx, top, ex - sx,
                            std::max(2.0f, float(size) * 1.15f));
                    nvgFillColor(vg_, nvgRGBAf(0.25f, 0.55f, 0.95f, 0.45f));
                    nvgFill(vg_);
                }
                if (c.caret_visible) {
                    const float cx = start_x + prefix_width(caret);
                    nvgBeginPath(vg_);
                    nvgRect(vg_, cx, top, std::max(1.0f, float(size) * 0.06f),
                            std::max(2.0f, float(size) * 1.15f));
                    nvgFillColor(vg_, nvg_color(c.text_color, 1.0));
                    nvgFill(vg_);
                }
                nvgBeginPath(vg_);
                nvgRect(vg_, float(c.x), float(c.y), float(c.w), float(c.h));
                nvgStrokeWidth(vg_, 1.5f);
                nvgStrokeColor(vg_, nvgRGBAf(0.35f, 0.68f, 1.0f, 0.95f));
                nvgStroke(vg_);
            }
            nvgFillColor(vg_, nvg_color(c.text_color, c.text_alpha));
            if (c.wrap)
                nvgTextBox(vg_, x, y, float(c.w), c.text.c_str(), nullptr);
            else
                nvgText(vg_, x, y, c.text.c_str(), nullptr);
            break;
        }
        case GuiKind::Image: {
            const int id = image(c.image_path);
            if (id <= 0)
                break;
            NVGpaint paint = nvgImagePattern(
                vg_, float(c.x), float(c.y), float(c.w), float(c.h), 0.0f, id,
                float(std::clamp(c.image_alpha, 0.0, 1.0)));
            paint.innerColor = paint.outerColor =
                nvg_color(c.image_color, std::clamp(c.image_alpha, 0.0, 1.0));
            nvgBeginPath(vg_);
            nvgRect(vg_, float(c.x), float(c.y), float(c.w), float(c.h));
            nvgFillPaint(vg_, paint);
            nvgFill(vg_);
            break;
        }
        }
        nvgRestore(vg_);
    }
    nvgRestore(vg_);
    nvgEndFrame(vg_);
}

} // namespace rbxclient
