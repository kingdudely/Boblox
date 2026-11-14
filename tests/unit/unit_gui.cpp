// unit_gui.cpp — ScreenGui layout, hit-testing and real input/event routing.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "engine.h"
#include "common/cookie.h"
#include "gui/gui_layout.h"
#include "instance/class_registry.h"
#include "instance/gui_input.h"
#include "instance/host_input.h"
#include "instance/instance.h"
#include "instance/signal.h"

#include "lua.h"
#include "luacode.h"

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

namespace {

rbx::Ref<rbx::Instance> make(const char* cls, const char* name) {
    const rbx::ClassInfo* info = rbx::find_class(cls);
    REQUIRE(info != nullptr);
    rbx::Ref<rbx::Instance> inst = rbx::instantiate(info);
    REQUIRE(inst.get() != nullptr);
    inst->set_name(name);
    return inst;
}

void setv(rbx::Instance* inst, const char* prop, rbx::Variant value) {
    std::string err;
    REQUIRE(inst->set_prop(prop, value, &err));
}

rbx::UDim2 udim(double sx, double ox, double sy, double oy) {
    rbx::UDim2 u;
    u.x.scale = sx;
    u.x.offset = ox;
    u.y.scale = sy;
    u.y.offset = oy;
    return u;
}

std::vector<char> compile(const std::string& src) {
    lua_CompileOptions o;
    std::memset(&o, 0, sizeof(o));
    size_t out_size = 0;
    char* out = luau_compile(src.c_str(), src.size(), &o, &out_size);
    REQUIRE(out != nullptr);
    std::vector<char> bc(out, out + out_size);
    std::free(out);
    return bc;
}

struct Env {
    rbxch::engine::Environment* e = nullptr;
    Env() {
        rbxch::Options o;
        o.profile = rbxch::Profile::Engine;
        e = rbxch::engine::create(o);
        REQUIRE(e != nullptr);
    }
    ~Env() { rbxch::engine::destroy(e); }

    void exec(const std::string& src) {
        const std::vector<char> bc = compile(src);
        const rbxch::Result r =
            rbxch::engine::execute(*e, bc.data(), bc.size());
        INFO("script: " << src << "\nerror: " << r.error);
        REQUIRE(r.status == rbxch::Status::Ok);
    }
    void step(double dt = 1.0 / 60.0) { rbxch::engine::step(*e, dt); }
    lua_State* L() { return rbxch::engine::state(*e); }
};

std::string gstr(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    const char* s = lua_tostring(L, -1);
    std::string out = s ? s : "";
    lua_pop(L, 1);
    return out;
}

double gnum(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    const double n = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return n;
}

bool gbool(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    const bool b = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return b;
}

void expose_root(lua_State* L, rbx::Instance* root) {
    push_variant(L, rbx::Variant(root));
    lua_setglobal(L, "testRoot");
}

} // namespace

TEST_CASE("ScreenGui lays out scale/offset and respects the inset") {
    rbx::register_builtin_classes();
    auto root = make("Model", "DataModel");
    auto screen = make("ScreenGui", "Hud");
    auto frame = make("Frame", "Panel");
    auto label = make("TextLabel", "Title");
    std::string err;
    REQUIRE(screen->reparent(root.get(), &err));
    REQUIRE(frame->reparent(screen.get(), &err));
    REQUIRE(label->reparent(frame.get(), &err));

    setv(frame.get(), "Position", rbx::Variant(udim(0.25, 0, 0.25, 0)));
    setv(frame.get(), "Size", rbx::Variant(udim(0.5, 0, 0.5, 0)));
    setv(frame.get(), "AnchorPoint", rbx::Variant(rbx::Vector2{0.0, 0.0}));
    setv(frame.get(), "BackgroundColor3",
         rbx::Variant(rbx::Color3{1.0, 0.0, 0.0}));
    setv(label.get(), "Size", rbx::Variant(udim(1.0, 0, 1.0, 0)));
    setv(label.get(), "Text", rbx::Variant(std::string("Hello")));
    setv(label.get(), "TextSize", rbx::Variant(20.0));
    setv(label.get(), "TextColor3",
         rbx::Variant(rbx::Color3{1.0, 1.0, 1.0}));

    const auto cmds = rbxclient::collectGui(root.get(), 640, 360);
    REQUIRE(cmds.size() == 3); // frame rect, label rect, label text
    CHECK(cmds[0].kind == rbxclient::GuiKind::Rect);
    CHECK(cmds[0].x == doctest::Approx(160.0));
    CHECK(cmds[0].y == doctest::Approx(117.0)); // 25% of 324 + 36 inset
    CHECK(cmds[0].w == doctest::Approx(320.0));
    CHECK(cmds[0].h == doctest::Approx(162.0));
    CHECK(cmds[1].kind == rbxclient::GuiKind::Rect);
    CHECK(cmds[2].kind == rbxclient::GuiKind::Text);
    CHECK(cmds[2].text == "Hello");
    CHECK(cmds[2].text_size == doctest::Approx(20.0));
}

TEST_CASE("AnchorPoint subtracts from the position") {
    rbx::register_builtin_classes();
    auto screen = make("ScreenGui", "Hud");
    auto frame = make("Frame", "Centered");
    std::string err;
    REQUIRE(frame->reparent(screen.get(), &err));
    setv(frame.get(), "Position", rbx::Variant(udim(0.5, 0, 0.5, 0)));
    setv(frame.get(), "Size", rbx::Variant(udim(0.0, 100, 0.0, 40)));
    setv(frame.get(), "AnchorPoint", rbx::Variant(rbx::Vector2{0.5, 0.5}));

    const auto cmds = rbxclient::collectGui(screen.get(), 800, 600);
    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0].x == doctest::Approx(400.0 - 50.0));
    CHECK(cmds[0].y == doctest::Approx(36.0 + (600.0 - 36.0) * 0.5 - 20.0));
}

TEST_CASE("UIListLayout lays out children and exposes absolute geometry") {
    rbx::register_builtin_classes();
    auto screen = make("ScreenGui", "Hud");
    auto frame = make("Frame", "List");
    auto a = make("TextLabel", "A");
    auto b = make("TextLabel", "B");
    auto c = make("TextLabel", "C");
    auto layout = make("UIListLayout", "Layout");
    std::string err;
    REQUIRE(frame->reparent(screen.get(), &err));
    REQUIRE(a->reparent(frame.get(), &err));
    REQUIRE(b->reparent(frame.get(), &err));
    REQUIRE(c->reparent(frame.get(), &err));
    REQUIRE(layout->reparent(frame.get(), &err));
    setv(screen.get(), "IgnoreGuiInset", rbx::Variant(true));
    setv(frame.get(), "Size", rbx::Variant(udim(0, 400, 0, 300)));
    setv(a.get(), "Size", rbx::Variant(udim(0, 100, 0, 40)));
    setv(b.get(), "Size", rbx::Variant(udim(0, 50, 0, 30)));
    setv(c.get(), "Size", rbx::Variant(udim(0, 200, 0, 20)));
    setv(a.get(), "LayoutOrder", rbx::Variant(int64_t(2)));
    setv(b.get(), "LayoutOrder", rbx::Variant(int64_t(1)));
    setv(c.get(), "LayoutOrder", rbx::Variant(int64_t(3)));
    setv(layout.get(), "FillDirection",
         rbx::Variant(rbx::EnumItem{"FillDirection", 1}));
    setv(layout.get(), "Padding", rbx::Variant(rbx::UDim{0.0, 10.0}));
    setv(layout.get(), "HorizontalAlignment",
         rbx::Variant(rbx::EnumItem{"HorizontalAlignment", 1}));
    setv(layout.get(), "VerticalAlignment",
         rbx::Variant(rbx::EnumItem{"VerticalAlignment", 1}));
    setv(layout.get(), "SortOrder",
         rbx::Variant(rbx::EnumItem{"SortOrder", 2}));

    const auto cmds = rbxclient::collectGui(screen.get(), 800, 600);
    REQUIRE(!cmds.empty());
    auto abs = [&](const rbx::Instance* inst) {
        rbx::Variant v;
        REQUIRE(inst->get_prop("AbsolutePosition", v));
        return std::get<rbx::Vector2>(v);
    };
    auto size = [&](const rbx::Instance* inst) {
        rbx::Variant v;
        REQUIRE(inst->get_prop("AbsoluteSize", v));
        return std::get<rbx::Vector2>(v);
    };
    rbx::Variant content;
    REQUIRE(layout->get_prop("AbsoluteContentSize", content));
    const auto content_size = std::get<rbx::Vector2>(content);
    CHECK(content_size.x == doctest::Approx(200.0));
    CHECK(content_size.y == doctest::Approx(110.0));
    CHECK(abs(b.get()).y == doctest::Approx(0.0));
    CHECK(abs(a.get()).y == doctest::Approx(40.0));
    CHECK(abs(c.get()).y == doctest::Approx(90.0));
    CHECK(size(c.get()).x == doctest::Approx(200.0));
    CHECK(size(c.get()).y == doctest::Approx(20.0));
}

TEST_CASE("UIGridLayout uses native cell geometry and exposes its counters") {
    rbx::register_builtin_classes();
    auto screen = make("ScreenGui", "Hud");
    auto frame = make("Frame", "Grid");
    auto grid = make("UIGridLayout", "GridLayout");
    std::string err;
    REQUIRE(frame->reparent(screen.get(), &err));
    REQUIRE(grid->reparent(frame.get(), &err));
    setv(screen.get(), "IgnoreGuiInset", rbx::Variant(true));
    setv(frame.get(), "Size", rbx::Variant(udim(0, 400, 0, 300)));
    setv(grid.get(), "CellSize",
         rbx::Variant(udim(0, 100, 0, 50)));
    setv(grid.get(), "CellPadding",
         rbx::Variant(udim(0, 10, 0, 10)));
    setv(grid.get(), "FillDirectionMaxCells", rbx::Variant(int64_t(10)));
    std::vector<rbx::Ref<rbx::Instance>> kids;
    for (int i = 1; i <= 5; ++i) {
        auto child = make("TextLabel", ("G" + std::to_string(i)).c_str());
        REQUIRE(child->reparent(frame.get(), &err));
        setv(child.get(), "LayoutOrder", rbx::Variant(int64_t(i)));
        kids.push_back(child);
    }
    rbxclient::collectGui(screen.get(), 800, 600);
    auto abs = [&](const rbx::Instance* inst) {
        rbx::Variant v;
        REQUIRE(inst->get_prop("AbsolutePosition", v));
        return std::get<rbx::Vector2>(v);
    };
    auto size = [&](const rbx::Instance* inst) {
        rbx::Variant v;
        REQUIRE(inst->get_prop("AbsoluteSize", v));
        return std::get<rbx::Vector2>(v);
    };
    CHECK(abs(kids[0].get()).x == doctest::Approx(0.0));
    CHECK(abs(kids[1].get()).x == doctest::Approx(110.0));
    CHECK(abs(kids[2].get()).x == doctest::Approx(220.0));
    CHECK(abs(kids[3].get()).y == doctest::Approx(60.0));
    CHECK(abs(kids[4].get()).x == doctest::Approx(110.0));
    CHECK(size(kids[4].get()).x == doctest::Approx(100.0));
    CHECK(size(kids[4].get()).y == doctest::Approx(50.0));
    rbx::Variant cell_count, cell_size, content;
    REQUIRE(grid->get_prop("AbsoluteCellCount", cell_count));
    REQUIRE(grid->get_prop("AbsoluteCellSize", cell_size));
    REQUIRE(grid->get_prop("AbsoluteContentSize", content));
    CHECK(std::get<rbx::Vector2>(cell_count).x == doctest::Approx(3.0));
    CHECK(std::get<rbx::Vector2>(cell_count).y == doctest::Approx(2.0));
    CHECK(std::get<rbx::Vector2>(cell_size).y == doctest::Approx(50.0));
    CHECK(std::get<rbx::Vector2>(content).x == doctest::Approx(320.0));
    CHECK(std::get<rbx::Vector2>(content).y == doctest::Approx(110.0));
}

TEST_CASE("UIListLayout flex and wrapped line alignment follow native") {
    rbx::register_builtin_classes();
    auto screen = make("ScreenGui", "Hud");
    auto vf = make("Frame", "VF");
    auto hf = make("Frame", "HF");
    auto lines = make("Frame", "LN");
    std::string err;
    REQUIRE(vf->reparent(screen.get(), &err));
    REQUIRE(hf->reparent(screen.get(), &err));
    REQUIRE(lines->reparent(screen.get(), &err));
    setv(screen.get(), "IgnoreGuiInset", rbx::Variant(true));
    setv(vf.get(), "Size", rbx::Variant(udim(0, 400, 0, 300)));
    setv(hf.get(), "Size", rbx::Variant(udim(0, 400, 0, 200)));
    setv(lines.get(), "Size", rbx::Variant(udim(0, 400, 0, 200)));
    setv(lines.get(), "Position", rbx::Variant(udim(0, 0, 0, 320)));

    std::vector<rbx::Ref<rbx::Instance>> vkids;
    const int vh[] = {20, 30, 40};
    for (int i = 0; i < 3; ++i) {
        auto child = make("TextLabel", ("V" + std::to_string(i + 1)).c_str());
        REQUIRE(child->reparent(vf.get(), &err));
        setv(child.get(), "Size", rbx::Variant(udim(0, 50 + i * 50, 0, vh[i])));
        vkids.push_back(child);
    }
    auto vl = make("UIListLayout", "VL");
    REQUIRE(vl->reparent(vf.get(), &err));
    setv(vl.get(), "FillDirection", rbx::Variant(rbx::EnumItem{"FillDirection", 1}));
    setv(vl.get(), "Padding", rbx::Variant(rbx::UDim{0.0, 10.0}));
    setv(vl.get(), "VerticalFlex",
         rbx::Variant(rbx::EnumItem{"UIFlexAlignment", 1}));

    std::vector<rbx::Ref<rbx::Instance>> hkids;
    for (int i = 0; i < 3; ++i) {
        auto child = make("TextLabel", ("H" + std::to_string(i + 1)).c_str());
        REQUIRE(child->reparent(hf.get(), &err));
        setv(child.get(), "Size", rbx::Variant(udim(0, 50 + i * 50, 0, 20)));
        hkids.push_back(child);
    }
    auto hl = make("UIListLayout", "HL");
    REQUIRE(hl->reparent(hf.get(), &err));
    setv(hl.get(), "FillDirection",
         rbx::Variant(rbx::EnumItem{"FillDirection", 0}));
    setv(hl.get(), "HorizontalFlex",
         rbx::Variant(rbx::EnumItem{"UIFlexAlignment", 1}));

    std::vector<rbx::Ref<rbx::Instance>> lkids;
    const int lh[] = {40, 30, 20, 50};
    const int lw[] = {150, 150, 150, 100};
    for (int i = 0; i < 4; ++i) {
        auto child = make("TextLabel", ("L" + std::to_string(i + 1)).c_str());
        REQUIRE(child->reparent(lines.get(), &err));
        setv(child.get(), "Size", rbx::Variant(udim(0, lw[i], 0, lh[i])));
        lkids.push_back(child);
    }
    auto ll = make("UIListLayout", "LL");
    REQUIRE(ll->reparent(lines.get(), &err));
    setv(ll.get(), "FillDirection",
         rbx::Variant(rbx::EnumItem{"FillDirection", 0}));
    setv(ll.get(), "Padding", rbx::Variant(rbx::UDim{0.0, 10.0}));
    setv(ll.get(), "Wraps", rbx::Variant(true));
    setv(ll.get(), "ItemLineAlignment",
         rbx::Variant(rbx::EnumItem{"ItemLineAlignment", 2}));

    rbxclient::collectGui(screen.get(), 800, 600);
    auto pos = [&](const rbx::Instance* inst) {
        rbx::Variant v;
        REQUIRE(inst->get_prop("AbsolutePosition", v));
        return std::get<rbx::Vector2>(v);
    };
    auto size = [&](const rbx::Instance* inst) {
        rbx::Variant v;
        REQUIRE(inst->get_prop("AbsoluteSize", v));
        return std::get<rbx::Vector2>(v);
    };
    const auto vf_origin = pos(vf.get());
    CHECK(pos(vkids[0].get()).y - vf_origin.y == doctest::Approx(0.0));
    CHECK(pos(vkids[1].get()).y - vf_origin.y == doctest::Approx(93.3333333333));
    CHECK(pos(vkids[2].get()).y - vf_origin.y == doctest::Approx(196.6666666667));
    CHECK(size(vkids[0].get()).y == doctest::Approx(83.3333333333));
    CHECK(size(vkids[2].get()).y == doctest::Approx(103.3333333333));
    const auto hf_origin = pos(hf.get());
    CHECK(pos(hkids[0].get()).x - hf_origin.x == doctest::Approx(0.0));
    CHECK(pos(hkids[1].get()).x - hf_origin.x ==
          doctest::Approx(83.3333333333));
    CHECK(size(hkids[2].get()).x == doctest::Approx(183.3333333333));
    CHECK(size(hkids[0].get()).y == doctest::Approx(20.0));
    const auto lines_origin = pos(lines.get());
    CHECK(pos(lkids[1].get()).y - lines_origin.y == doctest::Approx(5.0));
    CHECK(pos(lkids[2].get()).y - lines_origin.y == doctest::Approx(65.0));
}

TEST_CASE("UIPadding shrinks the layout canvas before resolving child sizes") {
    rbx::register_builtin_classes();
    auto screen = make("ScreenGui", "Hud");
    auto frame = make("Frame", "Padded");
    auto a = make("TextLabel", "P1");
    auto b = make("TextLabel", "P2");
    auto padding = make("UIPadding", "Padding");
    auto layout = make("UIListLayout", "Layout");
    std::string err;
    REQUIRE(frame->reparent(screen.get(), &err));
    REQUIRE(a->reparent(frame.get(), &err));
    REQUIRE(b->reparent(frame.get(), &err));
    REQUIRE(padding->reparent(frame.get(), &err));
    REQUIRE(layout->reparent(frame.get(), &err));
    setv(screen.get(), "IgnoreGuiInset", rbx::Variant(true));
    setv(frame.get(), "Size", rbx::Variant(udim(0, 400, 0, 200)));
    setv(a.get(), "Size", rbx::Variant(udim(0.5, 0, 0, 30)));
    setv(b.get(), "Size", rbx::Variant(udim(1, 0, 0, 20)));
    setv(padding.get(), "PaddingLeft", rbx::Variant(rbx::UDim{0.0, 10.0}));
    setv(padding.get(), "PaddingRight", rbx::Variant(rbx::UDim{0.0, 10.0}));
    setv(padding.get(), "PaddingTop", rbx::Variant(rbx::UDim{0.0, 20.0}));
    setv(padding.get(), "PaddingBottom", rbx::Variant(rbx::UDim{0.0, 20.0}));
    setv(layout.get(), "Padding", rbx::Variant(rbx::UDim{0.0, 5.0}));
    rbxclient::collectGui(screen.get(), 800, 600);
    auto abs = [&](const rbx::Instance* inst) {
        rbx::Variant v;
        REQUIRE(inst->get_prop("AbsolutePosition", v));
        return std::get<rbx::Vector2>(v);
    };
    auto size = [&](const rbx::Instance* inst) {
        rbx::Variant v;
        REQUIRE(inst->get_prop("AbsoluteSize", v));
        return std::get<rbx::Vector2>(v);
    };
    CHECK(abs(a.get()).x == doctest::Approx(10.0));
    CHECK(abs(a.get()).y == doctest::Approx(20.0));
    CHECK(size(a.get()).x == doctest::Approx(190.0));
    CHECK(abs(b.get()).y == doctest::Approx(55.0));
    CHECK(size(b.get()).x == doctest::Approx(380.0));
    rbx::Variant content;
    REQUIRE(layout->get_prop("AbsoluteContentSize", content));
    CHECK(std::get<rbx::Vector2>(content).x == doctest::Approx(380.0));
    CHECK(std::get<rbx::Vector2>(content).y == doctest::Approx(55.0));
}

TEST_CASE("UIListLayout wrapping uses native line-start alignment") {
    rbx::register_builtin_classes();
    auto screen = make("ScreenGui", "Hud");
    auto frame = make("Frame", "Wrap");
    auto layout = make("UIListLayout", "Layout");
    std::string err;
    REQUIRE(frame->reparent(screen.get(), &err));
    REQUIRE(layout->reparent(frame.get(), &err));
    setv(screen.get(), "IgnoreGuiInset", rbx::Variant(true));
    setv(frame.get(), "Size", rbx::Variant(udim(0, 400, 0, 200)));
    setv(layout.get(), "FillDirection",
         rbx::Variant(rbx::EnumItem{"FillDirection", 0}));
    setv(layout.get(), "Padding", rbx::Variant(rbx::UDim{0.0, 10.0}));
    setv(layout.get(), "Wraps", rbx::Variant(true));
    setv(layout.get(), "SortOrder",
         rbx::Variant(rbx::EnumItem{"SortOrder", 2}));
    struct Spec { const char* name; int w, h; };
    const Spec specs[] = {{"W1", 150, 40}, {"W2", 150, 30},
                          {"W3", 150, 20}, {"W4", 100, 50}};
    std::vector<rbx::Ref<rbx::Instance>> kids;
    for (int i = 0; i < 4; ++i) {
        auto child = make("TextLabel", specs[i].name);
        REQUIRE(child->reparent(frame.get(), &err));
        setv(child.get(), "Size",
             rbx::Variant(udim(0, specs[i].w, 0, specs[i].h)));
        setv(child.get(), "LayoutOrder", rbx::Variant(int64_t(i + 1)));
        kids.push_back(child);
    }
    rbxclient::collectGui(screen.get(), 800, 600);
    auto abs = [&](const rbx::Instance* inst) {
        rbx::Variant v;
        REQUIRE(inst->get_prop("AbsolutePosition", v));
        return std::get<rbx::Vector2>(v);
    };
    CHECK(abs(kids[0].get()).y == doctest::Approx(0.0));
    CHECK(abs(kids[1].get()).y == doctest::Approx(0.0));
    CHECK(abs(kids[2].get()).y == doctest::Approx(50.0));
    CHECK(abs(kids[3].get()).y == doctest::Approx(50.0));
    rbx::Variant content;
    REQUIRE(layout->get_prop("AbsoluteContentSize", content));
    CHECK(std::get<rbx::Vector2>(content).x == doctest::Approx(310.0));
    CHECK(std::get<rbx::Vector2>(content).y == doctest::Approx(100.0));
}

TEST_CASE("UIListLayout applies a script custom sorter") {
    rbx::register_builtin_classes();
    Env env; // destroyed after the C++ instance refs below
    auto screen = make("ScreenGui", "Hud");
    auto frame = make("Frame", "List");
    std::string err;
    REQUIRE(frame->reparent(screen.get(), &err));
    setv(screen.get(), "IgnoreGuiInset", rbx::Variant(true));
    setv(frame.get(), "Size", rbx::Variant(udim(0, 400, 0, 300)));
    auto c = make("TextLabel", "C");
    auto a = make("TextLabel", "A");
    auto b = make("TextLabel", "B");
    for (auto* child : {c.get(), a.get(), b.get()}) {
        REQUIRE(child->reparent(frame.get(), &err));
        setv(child, "Size", rbx::Variant(udim(0, 50, 0, 20)));
    }
    auto layout = make("UIListLayout", "Layout");
    REQUIRE(layout->reparent(frame.get(), &err));
    setv(layout.get(), "SortOrder",
         rbx::Variant(rbx::EnumItem{"SortOrder", 1}));

    expose_root(env.L(), screen.get());
    env.exec(
        "local l = testRoot:FindFirstChild('List'):FindFirstChild('Layout')\n"
        "l:SetCustomSortFunction(function(x, y) return x.Name > y.Name end)\n");
    rbxclient::collectGui(screen.get(), 800, 600);
    auto y = [&](const rbx::Instance* inst) {
        rbx::Variant v;
        REQUIRE(inst->get_prop("AbsolutePosition", v));
        return std::get<rbx::Vector2>(v).y;
    };
    CHECK(y(c.get()) == doctest::Approx(0.0));
    CHECK(y(b.get()) == doctest::Approx(20.0));
    CHECK(y(a.get()) == doctest::Approx(40.0));
}

TEST_CASE("only PlayerGui screens render, not StarterGui templates") {
    rbx::register_builtin_classes();
    auto data_model = make("Model", "DataModel");
    auto starter = make("StarterGui", "StarterGui");
    auto template_screen = make("ScreenGui", "Template");
    auto player = make("Model", "Player");
    auto player_gui = make("PlayerGui", "PlayerGui");
    auto live_screen = make("ScreenGui", "Live");
    auto live_frame = make("Frame", "OnlyLive");
    std::string err;
    REQUIRE(starter->reparent(data_model.get(), &err));
    REQUIRE(template_screen->reparent(starter.get(), &err));
    REQUIRE(player->reparent(data_model.get(), &err));
    REQUIRE(player_gui->reparent(player.get(), &err));
    REQUIRE(live_screen->reparent(player_gui.get(), &err));
    REQUIRE(live_frame->reparent(live_screen.get(), &err));
    setv(live_frame.get(), "Size", rbx::Variant(udim(0, 20, 0, 20)));
    setv(live_frame.get(), "BackgroundColor3",
         rbx::Variant(rbx::Color3{0.0, 0.0, 1.0}));

    const auto cmds = rbxclient::collectGui(data_model.get(), 640, 360);
    REQUIRE(cmds.size() == 1);
    CHECK(cmds[0].kind == rbxclient::GuiKind::Rect);
    CHECK(cmds[0].w == doctest::Approx(20.0));
}

TEST_CASE("GUI presentation defaults match the native client") {
    rbx::register_builtin_classes();
    Env env; // engine registration installs the GUI defaults
    auto screen = make("ScreenGui", "Hud");
    auto frame = make("Frame", "Panel");
    auto button = make("TextButton", "Button");
    auto list = make("UIListLayout", "List");
    auto grid = make("UIGridLayout", "Grid");
    rbx::Variant v;
    REQUIRE(screen->get_prop("Enabled", v));
    CHECK(std::get<bool>(v));
    REQUIRE(frame->get_prop("Visible", v));
    CHECK(std::get<bool>(v));
    REQUIRE(frame->get_prop("Interactable", v));
    CHECK(std::get<bool>(v));
    REQUIRE(frame->get_prop("Active", v));
    CHECK(!std::get<bool>(v));
    REQUIRE(button->get_prop("Active", v));
    CHECK(std::get<bool>(v));
    REQUIRE(list->get_prop("FillDirection", v));
    CHECK(std::get<rbx::EnumItem>(v).value == 1); // Vertical
    REQUIRE(list->get_prop("HorizontalAlignment", v));
    CHECK(std::get<rbx::EnumItem>(v).value == 1); // Left
    REQUIRE(list->get_prop("VerticalAlignment", v));
    CHECK(std::get<rbx::EnumItem>(v).value == 1); // Top
    REQUIRE(list->get_prop("Padding", v));
    CHECK(std::get<rbx::UDim>(v).offset == doctest::Approx(0.0));
    REQUIRE(grid->get_prop("CellSize", v));
    CHECK(std::get<rbx::UDim2>(v).x.offset == doctest::Approx(100.0));
    CHECK(std::get<rbx::UDim2>(v).y.offset == doctest::Approx(100.0));
    REQUIRE(grid->get_prop("CellPadding", v));
    CHECK(std::get<rbx::UDim2>(v).x.offset == doctest::Approx(5.0));
    REQUIRE(grid->get_prop("HorizontalAlignment", v));
    CHECK(std::get<rbx::EnumItem>(v).value == 1); // Left
}

TEST_CASE("ImageLabel keeps its asset URI and IsLoaded follows the cache") {
    rbx::register_builtin_classes();
    auto screen = make("ScreenGui", "Hud");
    auto image = make("ImageLabel", "Logo");
    std::string err;
    REQUIRE(image->reparent(screen.get(), &err));
    setv(image.get(), "Size", rbx::Variant(udim(0, 64, 0, 64)));
    setv(image.get(), "Image",
         rbx::Variant(std::string("rbxassetid://284402752")));
    setv(image.get(), "ImageColor3",
         rbx::Variant(rbx::Color3{0.25, 0.5, 0.75}));

    const auto cmds = rbxclient::collectGui(screen.get(), 640, 360);
    const auto image_cmd = std::find_if(
        cmds.begin(), cmds.end(), [](const auto& c) {
            return c.kind == rbxclient::GuiKind::Image;
        });
    REQUIRE(image_cmd != cmds.end());
    CHECK(image_cmd->image_path == "rbxassetid://284402752");
    CHECK(image_cmd->image_color.r == doctest::Approx(0.25));
    CHECK(image_cmd->image_color.g == doctest::Approx(0.5));
    CHECK(image_cmd->image_color.b == doctest::Approx(0.75));

    rbx::Variant loaded;
    REQUIRE(image->get_prop("IsLoaded", loaded));
    CHECK_FALSE(std::get<bool>(loaded));
    rbxclient::setGuiImageLoadedQuery(
        [](const std::string& uri) { return uri == "rbxassetid://284402752"; });
    REQUIRE(image->get_prop("IsLoaded", loaded));
    CHECK(std::get<bool>(loaded));
    rbxclient::setGuiImageLoadedQuery({});
}

TEST_CASE("cookie header accepts raw and named .ROBLOSECURITY values") {
    CHECK(rbxclient::cookie_header("ABC==") == ".ROBLOSECURITY=ABC==");
    CHECK(rbxclient::cookie_header(".ROBLOSECURITY=ABC==") ==
          ".ROBLOSECURITY=ABC==");
    CHECK(rbxclient::cookie_header(".ROBLOSECURITY=ABC==; other=1") ==
          ".ROBLOSECURITY=ABC==; other=1");
}

TEST_CASE("GUI uses LocalPlayer's PlayerGui, not another player's") {
    rbx::register_builtin_classes();
    auto data_model = make("Model", "DataModel");
    auto players = make("Players", "Players");
    auto local = make("Player", "Local");
    auto other = make("Player", "Other");
    auto local_gui = make("PlayerGui", "PlayerGui");
    auto other_gui = make("PlayerGui", "PlayerGui");
    auto local_screen = make("ScreenGui", "LocalScreen");
    auto other_screen = make("ScreenGui", "OtherScreen");
    auto local_frame = make("Frame", "LocalFrame");
    auto other_frame = make("Frame", "OtherFrame");
    std::string err;
    REQUIRE(players->reparent(data_model.get(), &err));
    REQUIRE(local->reparent(players.get(), &err));
    REQUIRE(other->reparent(players.get(), &err));
    REQUIRE(local_gui->reparent(local.get(), &err));
    REQUIRE(other_gui->reparent(other.get(), &err));
    REQUIRE(local_screen->reparent(local_gui.get(), &err));
    REQUIRE(other_screen->reparent(other_gui.get(), &err));
    REQUIRE(local_frame->reparent(local_screen.get(), &err));
    REQUIRE(other_frame->reparent(other_screen.get(), &err));
    setv(local.get(), "UserId", rbx::Variant(int64_t(4242)));
    setv(other.get(), "UserId", rbx::Variant(int64_t(9999)));
    for (auto* frame : {local_frame.get(), other_frame.get()}) {
        setv(frame, "Size", rbx::Variant(udim(0, 20, 0, 20)));
        setv(frame, "BackgroundColor3",
             rbx::Variant(rbx::Color3{0.0, 0.0, 1.0}));
    }
    rbx::set_local_user_id(4242);
    const auto cmds = rbxclient::collectGui(data_model.get(), 640, 360);
    CHECK(cmds.size() == 1);
    if (!cmds.empty())
        CHECK(cmds[0].source == local_frame.get());
    rbx::set_local_user_id(0);
}

TEST_CASE("invisible objects and disabled screens draw nothing") {
    rbx::register_builtin_classes();
    auto screen = make("ScreenGui", "Hud");
    auto frame = make("Frame", "Hidden");
    std::string err;
    REQUIRE(frame->reparent(screen.get(), &err));
    setv(frame.get(), "Size", rbx::Variant(udim(0, 100, 0, 100)));
    setv(frame.get(), "BackgroundColor3",
         rbx::Variant(rbx::Color3{0.0, 1.0, 0.0}));
    setv(frame.get(), "Visible", rbx::Variant(false));
    CHECK(rbxclient::collectGui(screen.get(), 800, 600).empty());

    setv(frame.get(), "Visible", rbx::Variant(true));
    setv(screen.get(), "Enabled", rbx::Variant(false));
    CHECK(rbxclient::collectGui(screen.get(), 800, 600).empty());
}

TEST_CASE("hit-testing honours ZIndex, visibility and pass-through") {
    rbx::register_builtin_classes();
    auto screen = make("ScreenGui", "Hud");
    setv(screen.get(), "IgnoreGuiInset", rbx::Variant(true));
    auto lower = make("TextButton", "Lower");
    auto upper = make("TextButton", "Upper");
    auto label = make("TextLabel", "Decorative");
    std::string err;
    REQUIRE(lower->reparent(screen.get(), &err));
    REQUIRE(upper->reparent(screen.get(), &err));
    REQUIRE(label->reparent(screen.get(), &err));
    for (auto* b : {lower.get(), upper.get()}) {
        setv(b, "Position", rbx::Variant(udim(0, 50, 0, 50)));
        setv(b, "Size", rbx::Variant(udim(0, 100, 0, 100)));
    }
    setv(upper.get(), "ZIndex", rbx::Variant(int64_t(5)));
    setv(label.get(), "Position", rbx::Variant(udim(0, 50, 0, 50)));
    setv(label.get(), "Size", rbx::Variant(udim(0, 100, 0, 100)));
    setv(label.get(), "ZIndex", rbx::Variant(int64_t(10)));

    // The label is above both buttons but not Active: it must not swallow.
    CHECK(rbxclient::hitTestGui(screen.get(), 800, 600, 100, 100) ==
          upper.get());
    setv(upper.get(), "Active", rbx::Variant(false));
    CHECK(rbxclient::hitTestGui(screen.get(), 800, 600, 100, 100) ==
          lower.get());
    setv(upper.get(), "Active", rbx::Variant(true));
    setv(upper.get(), "Visible", rbx::Variant(false));
    CHECK(rbxclient::hitTestGui(screen.get(), 800, 600, 100, 100) ==
          lower.get());
}

TEST_CASE("hit-testing clips children and applies the ScreenGui inset") {
    rbx::register_builtin_classes();
    auto screen = make("ScreenGui", "Hud");
    auto outer = make("Frame", "Clip");
    auto child = make("TextButton", "Child");
    std::string err;
    REQUIRE(outer->reparent(screen.get(), &err));
    REQUIRE(child->reparent(outer.get(), &err));
    setv(outer.get(), "Position", rbx::Variant(udim(0, 10, 0, 10)));
    setv(outer.get(), "Size", rbx::Variant(udim(0, 100, 0, 100)));
    setv(outer.get(), "ClipsDescendants", rbx::Variant(true));
    setv(child.get(), "Position", rbx::Variant(udim(0, -50, 0, -50)));
    setv(child.get(), "Size", rbx::Variant(udim(0, 300, 0, 300)));
    // Inset moves the screen canvas down 36 px unless ignored.
    CHECK(rbxclient::hitTestGui(screen.get(), 800, 600, 50, 50) ==
          child.get());
    CHECK(rbxclient::hitTestGui(screen.get(), 800, 600, 250, 250) ==
          nullptr);
    setv(screen.get(), "IgnoreGuiInset", rbx::Variant(true));
    CHECK(rbxclient::hitTestGui(screen.get(), 800, 600, 50, 50) ==
          child.get());
    setv(child.get(), "Visible", rbx::Variant(false));
    CHECK(rbxclient::hitTestGui(screen.get(), 800, 600, 50, 50) == nullptr);
}

TEST_CASE("rotation-aware hit-testing uses the inverse object transform") {
    rbx::register_builtin_classes();
    auto screen = make("ScreenGui", "Hud");
    auto button = make("TextButton", "Rotated");
    std::string err;
    REQUIRE(button->reparent(screen.get(), &err));
    setv(screen.get(), "IgnoreGuiInset", rbx::Variant(true));
    setv(button.get(), "Position", rbx::Variant(udim(0.5, 0, 0.5, 0)));
    setv(button.get(), "AnchorPoint", rbx::Variant(rbx::Vector2{0.5, 0.5}));
    setv(button.get(), "Size", rbx::Variant(udim(0, 100, 0, 20)));
    setv(button.get(), "Rotation", rbx::Variant(90.0));
    CHECK(rbxclient::hitTestGui(screen.get(), 800, 600, 400, 340) ==
          button.get());
    CHECK(rbxclient::hitTestGui(screen.get(), 800, 600, 440, 300) == nullptr);
}

TEST_CASE("GUI input produces exact click, release-outside and hover order") {
    rbx::register_builtin_classes();
    rbx::input::reset();
    rbx::gui::reset();
    Env env;
    auto root = make("Model", "Root");
    rbxch::engine::attach(*env.e, root.get());
    expose_root(env.L(), root.get());
    env.exec(R"(
        screen = Instance.new("ScreenGui")
        screen.IgnoreGuiInset = true
        screen.Parent = testRoot
        btn = Instance.new("TextButton")
        btn.Position = UDim2.new(0, 20, 0, 20)
        btn.Size = UDim2.new(0, 120, 0, 40)
        btn.Parent = screen
        events = {}
        local function log(s) events[#events + 1] = s end
        btn.InputBegan:Connect(function() log("btn.InputBegan") end)
        btn.InputEnded:Connect(function() log("btn.InputEnded") end)
        btn.MouseButton1Down:Connect(function() log("btn.MouseButton1Down") end)
        btn.MouseButton1Up:Connect(function() log("btn.MouseButton1Up") end)
        btn.MouseButton1Click:Connect(function() log("btn.MouseButton1Click") end)
        btn.Activated:Connect(function() log("btn.Activated") end)
        btn.MouseEnter:Connect(function() log("btn.MouseEnter") end)
        btn.MouseLeave:Connect(function() log("btn.MouseLeave") end)
    )");

    rbx::input::publish_mouse(80, 40, false, false, false, 800, 600);
    env.step();
    env.exec("events = {}");
    rbx::input::publish_mouse(80, 40, true, false, false, 800, 600);
    env.step();
    rbx::input::publish_mouse(80, 40, false, false, false, 800, 600);
    env.step();
    env.exec("eventLog = table.concat(events, ',')");
    CHECK(gstr(env.L(), "eventLog") ==
          "btn.InputBegan,btn.MouseButton1Down,btn.InputEnded,"
          "btn.MouseButton1Up,btn.MouseButton1Click,btn.Activated");

    // Press on the button, drag off, release: up/end only, no click/activated.
    env.exec("events = {}");
    rbx::input::publish_mouse(80, 40, true, false, false, 800, 600);
    env.step();
    rbx::input::publish_mouse(400, 300, true, false, false, 800, 600);
    env.step();
    rbx::input::publish_mouse(400, 300, false, false, false, 800, 600);
    env.step();
    env.exec("eventLog = table.concat(events, ',')");
    CHECK(gstr(env.L(), "eventLog") ==
          "btn.InputBegan,btn.MouseButton1Down,btn.MouseLeave,"
          "btn.InputEnded,btn.MouseButton1Up");

    // Hover is edge-triggered exactly once per crossing.
    env.exec("events = {}");
    rbx::input::publish_mouse(80, 40, false, false, false, 800, 600);
    env.step();
    rbx::input::publish_mouse(400, 300, false, false, false, 800, 600);
    env.step();
    rbx::input::publish_mouse(80, 40, false, false, false, 800, 600);
    env.step();
    env.exec("eventLog = table.concat(events, ',')");
    CHECK(gstr(env.L(), "eventLog") ==
          "btn.MouseEnter,btn.MouseLeave,btn.MouseEnter");
    rbx::input::reset();
}

TEST_CASE("TextBox focus, editing, selection and Enter submit") {
    rbx::register_builtin_classes();
    rbx::input::reset();
    rbx::gui::reset();
    Env env;
    auto root = make("Model", "Root");
    rbxch::engine::attach(*env.e, root.get());
    expose_root(env.L(), root.get());
    env.exec(R"(
        screen = Instance.new("ScreenGui")
        screen.IgnoreGuiInset = true
        screen.Parent = testRoot
        tb = Instance.new("TextBox")
        tb.Position = UDim2.new(0, 20, 0, 80)
        tb.Size = UDim2.new(0, 240, 0, 40)
        tb.TextEditable = true
        tb.Text = ""
        tb.Parent = screen
        events = {}
        local function log(s) events[#events + 1] = s end
        tb.Focused:Connect(function() log("tb.Focused") end)
        tb.FocusLost:Connect(function(enter) log("tb.FocusLost:" .. tostring(enter)) end)
        tb.ReturnPressed:Connect(function() log("tb.ReturnPressed") end)
        tb.SelectionGained:Connect(function() log("tb.SelectionGained") end)
        tb.SelectionLost:Connect(function() log("tb.SelectionLost") end)
    )");
    rbx::input::publish_mouse(100, 100, false, false, false, 800, 600);
    env.step();
    env.exec("events = {}");
    rbx::input::publish_mouse(100, 100, true, false, false, 800, 600);
    env.step();
    rbx::input::publish_mouse(100, 100, false, false, false, 800, 600);
    env.step();
    env.exec("eventLog = table.concat(events, ',') ; focusedNow = tb:IsFocused()");
    CHECK(gstr(env.L(), "eventLog") ==
          "tb.Focused,tb.SelectionGained"); // Mouse signals are not TextBox events
    CHECK(gbool(env.L(), "focusedNow"));
    CHECK(rbx::gui::focused_textbox() != nullptr);

    rbx::input::publish_text("Hello World");
    env.step();
    env.exec("typed = tb.Text ; typedCaret = tb.CursorPosition ; "
             "editable = tb.TextEditable ; focusedFlag = tb:IsFocused()");
    CHECK(gbool(env.L(), "editable"));
    CHECK(gbool(env.L(), "focusedFlag"));
    CHECK(gstr(env.L(), "typed") == "Hello World");
    CHECK(gnum(env.L(), "typedCaret") == 11.0);

    env.exec("events = {}");
    rbx::input::publish_key(306, true); // ctrl
    rbx::input::publish_key(65, true);  // A
    rbx::input::publish_key(65, false);
    rbx::input::publish_key(306, false);
    env.step();
    env.exec("selStart = tb.SelectionStart ; selCaret = tb.CursorPosition ; "
             "tb.SelectionStart = 0 ; manualSel = tb.SelectionStart");
    CHECK(gnum(env.L(), "manualSel") == 0.0);
    CHECK(gnum(env.L(), "selStart") == 0.0);
    CHECK(gnum(env.L(), "selCaret") == 11.0);

    rbx::input::publish_key(13, true); // Enter
    env.step();
    rbx::input::publish_key(13, false);
    env.step();
    env.exec("eventLog = table.concat(events, ',') ; finalFocus = tb:IsFocused() ; finalText = tb.Text");
    CHECK(gstr(env.L(), "eventLog") ==
          "tb.ReturnPressed,tb.FocusLost:true,tb.SelectionLost");
    CHECK(!gbool(env.L(), "finalFocus"));
    CHECK(gstr(env.L(), "finalText") == "Hello World");
    rbx::input::reset();
}

TEST_CASE("ScrollingFrame wheel clamps CanvasPosition and clamps again under drag") {
    rbx::register_builtin_classes();
    rbx::input::reset();
    rbx::gui::reset();
    Env env;
    auto root = make("Model", "Root");
    rbxch::engine::attach(*env.e, root.get());
    expose_root(env.L(), root.get());
    env.exec(R"(
        screen = Instance.new("ScreenGui")
        screen.IgnoreGuiInset = true
        screen.Parent = testRoot
        sf = Instance.new("ScrollingFrame")
        sf.Position = UDim2.new(0, 20, 0, 140)
        sf.Size = UDim2.new(0, 220, 0, 120)
        sf.CanvasSize = UDim2.new(0, 0, 0, 300)
        sf.CanvasPosition = Vector2.new(0, 0)
        sf.ScrollBarThickness = 12
        sf.ScrollBarImageVisible = true
        sf.ScrollingDirection = Enum.ScrollingDirection.XY
        sf.ScrollingEnabled = true
        sf.Parent = screen
        events = {}
        sf:GetPropertyChangedSignal("CanvasPosition"):Connect(function()
            events[#events + 1] = "sf.CanvasPosition:" .. tostring(sf.CanvasPosition.Y)
        end)
    )");
    rbx::input::publish_mouse(100, 180, false, false, false, 800, 600);
    env.step();
    for (int i = 0; i < 5; ++i)
        rbx::input::publish_wheel(100, 180, 0, -10);
    env.step();
    env.exec("scrollY = sf.CanvasPosition.Y");
    CHECK(gnum(env.L(), "scrollY") ==
          doctest::Approx(180.0)); // 300 - 120, not 400
    env.exec("wasClamped = sf.CanvasPosition.Y <= 180.0");
    CHECK(gbool(env.L(), "wasClamped"));

    // Dragging the vertical thumb past the end must also clamp to max.
    env.exec("events = {}");
    rbx::input::publish_mouse(234, 150, true, false, false, 800, 600);
    env.step();
    rbx::input::publish_mouse(234, 500, true, false, false, 800, 600);
    env.step();
    rbx::input::publish_mouse(234, 500, false, false, false, 800, 600);
    env.step();
    env.exec("dragY = sf.CanvasPosition.Y");
    CHECK(gnum(env.L(), "dragY") == doctest::Approx(180.0));
    rbx::input::reset();
}
