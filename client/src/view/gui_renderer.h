// gui_renderer.h — NanoVG presentation of GuiDrawCommand lists.
//
// Kept out of gui_layout: layout is pure and unit-tested; this file is the
// only place that knows about NanoVG, fonts and textures.
#pragma once

#include "gui/gui_layout.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct NVGcontext;

namespace rbxclient {

class GuiRenderer {
  public:
    GuiRenderer();
    ~GuiRenderer();
    GuiRenderer(const GuiRenderer&) = delete;
    GuiRenderer& operator=(const GuiRenderer&) = delete;

    // `cookie` is the .ROBLOSECURITY value used only for assetdelivery
    // requests; it is never placed in the image cache key or logs.
    bool init(std::string* err, const std::string& cookie = {});
    void shutdown();
    void draw(const std::vector<GuiDrawCommand>& commands, int width,
              int height);
    bool ready() const { return vg_ != nullptr; }
    // Same font/atlas measurement used for drawing; installed into the shared
    // layout so TextBounds/TextFits agree with the rendered glyphs.
    rbx::Vector2 measure(const std::string& text, double size, bool bold);

  private:
    struct AssetRequest;
    int image(const std::string& path);
    void pump_assets();
    void start_asset(const std::string& path);
    NVGcontext* vg_ = nullptr;
    int font_regular_ = -1;
    int font_bold_ = -1;
    std::vector<unsigned char> regular_data_;
    std::vector<unsigned char> bold_data_;
    std::unordered_map<std::string, int> images_;
    std::string cookie_;
    std::unordered_map<std::string, std::shared_ptr<AssetRequest>> pending_;
};

} // namespace rbxclient
