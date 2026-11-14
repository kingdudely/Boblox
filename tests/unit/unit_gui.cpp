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
#include <cmath>
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

TEST_CASE("UIScale and GUI size constraints follow native ordering") {
    rbx::register_builtin_classes();
    Env env;
    auto screen = make("ScreenGui", "Hud");
    std::string err;
    setv(screen.get(), "IgnoreGuiInset", rbx::Variant(true));
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

    auto frame = make("Frame", "Scaled");
    auto child_frame = make("Frame", "Child");
    auto scale = make("UIScale", "Scale");
    REQUIRE(frame->reparent(screen.get(), &err));
    REQUIRE(child_frame->reparent(frame.get(), &err));
    REQUIRE(scale->reparent(frame.get(), &err));
    setv(frame.get(), "Size", rbx::Variant(udim(0, 400, 0, 300)));
    setv(child_frame.get(), "Position", rbx::Variant(udim(0, 10, 0, 20)));
    setv(child_frame.get(), "Size", rbx::Variant(udim(0, 100, 0, 50)));
    setv(scale.get(), "Scale", rbx::Variant(2.0));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(frame.get()).x == doctest::Approx(800.0));
    CHECK(size(frame.get()).y == doctest::Approx(600.0));
    CHECK(abs(child_frame.get()).x == doctest::Approx(20.0));
    CHECK(abs(child_frame.get()).y == doctest::Approx(40.0));
    CHECK(size(child_frame.get()).x == doctest::Approx(200.0));
    CHECK(size(child_frame.get()).y == doctest::Approx(100.0));

    // Constraint clamps the pre-scale size, then the aspect fit runs, and the
    // object's own UIScale is the last step.
    CHECK(size(frame.get()).x == doctest::Approx(800.0));
    auto constraint = make("UISizeConstraint", "SizeConstraint");
    REQUIRE(constraint->reparent(frame.get(), &err));
    setv(constraint.get(), "MinSize", rbx::Variant(rbx::Vector2{300.0, 200.0}));
    setv(constraint.get(), "MaxSize", rbx::Variant(rbx::Vector2{350.0, 250.0}));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(frame.get()).x == doctest::Approx(700.0));
    CHECK(size(frame.get()).y == doctest::Approx(500.0));

    auto aspect = make("UIAspectRatioConstraint", "Aspect");
    REQUIRE(aspect->reparent(frame.get(), &err));
    setv(aspect.get(), "AspectRatio", rbx::Variant(2.0));
    setv(aspect.get(), "AspectType",
         rbx::Variant(rbx::EnumItem{"AspectType", 0}));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(frame.get()).x == doctest::Approx(700.0));
    CHECK(size(frame.get()).y == doctest::Approx(350.0));
    CHECK(size(child_frame.get()).x == doctest::Approx(200.0));
    CHECK(size(child_frame.get()).y == doctest::Approx(100.0));
}

TEST_CASE("UIScale composes through nested objects and padding") {
    rbx::register_builtin_classes();
    Env env;
    auto screen = make("ScreenGui", "Hud");
    std::string err;
    setv(screen.get(), "IgnoreGuiInset", rbx::Variant(true));
    auto outer = make("Frame", "Outer");
    auto middle = make("Frame", "Middle");
    auto inner = make("Frame", "Inner");
    auto outer_scale = make("UIScale", "OuterScale");
    auto middle_scale = make("UIScale", "MiddleScale");
    auto padding = make("UIPadding", "Padding");
    REQUIRE(outer->reparent(screen.get(), &err));
    REQUIRE(middle->reparent(outer.get(), &err));
    REQUIRE(inner->reparent(middle.get(), &err));
    REQUIRE(outer_scale->reparent(outer.get(), &err));
    REQUIRE(middle_scale->reparent(middle.get(), &err));
    setv(outer.get(), "Size", rbx::Variant(udim(0, 400, 0, 300)));
    setv(middle.get(), "Position", rbx::Variant(udim(0, 10, 0, 0)));
    setv(middle.get(), "Size", rbx::Variant(udim(0, 100, 0, 100)));
    setv(inner.get(), "Position", rbx::Variant(udim(0, 10, 0, 20)));
    setv(inner.get(), "Size", rbx::Variant(udim(0, 50, 0, 25)));
    setv(outer_scale.get(), "Scale", rbx::Variant(2.0));
    setv(middle_scale.get(), "Scale", rbx::Variant(0.5));
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
    CHECK(size(outer.get()).x == doctest::Approx(800.0));
    CHECK(size(middle.get()).x == doctest::Approx(100.0));
    CHECK(abs(middle.get()).x == doctest::Approx(20.0));
    CHECK(abs(inner.get()).x == doctest::Approx(30.0));
    CHECK(abs(inner.get()).y == doctest::Approx(20.0));
    CHECK(size(inner.get()).x == doctest::Approx(50.0));

    REQUIRE(padding->reparent(middle.get(), &err));
    setv(padding.get(), "PaddingLeft", rbx::Variant(rbx::UDim{0.0, 10.0}));
    setv(padding.get(), "PaddingTop", rbx::Variant(rbx::UDim{0.0, 10.0}));
    auto padded = make("Frame", "Padded");
    REQUIRE(padded->reparent(middle.get(), &err));
    setv(padded.get(), "Size", rbx::Variant(udim(0, 100, 0, 50)));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(abs(padded.get()).x - abs(middle.get()).x == doctest::Approx(10.0));
    CHECK(abs(padded.get()).y - abs(middle.get()).y == doctest::Approx(10.0));
}

TEST_CASE("UISizeConstraint and UIAspectRatioConstraint expose native defaults") {
    rbx::register_builtin_classes();
    Env env;
    auto scale = make("UIScale", "Scale");
    auto constraint = make("UISizeConstraint", "Size");
    auto aspect = make("UIAspectRatioConstraint", "Aspect");
    rbx::Variant v;
    REQUIRE(scale->get_prop("Scale", v));
    CHECK(std::get<double>(v) == doctest::Approx(1.0));
    REQUIRE(constraint->get_prop("MinSize", v));
    CHECK(std::get<rbx::Vector2>(v).x == doctest::Approx(0.0));
    REQUIRE(constraint->get_prop("MaxSize", v));
    CHECK(std::isinf(std::get<rbx::Vector2>(v).x));
    CHECK(std::isinf(std::get<rbx::Vector2>(v).y));
    REQUIRE(aspect->get_prop("AspectRatio", v));
    CHECK(std::get<double>(v) == doctest::Approx(1.0));
    REQUIRE(aspect->get_prop("AspectType", v));
    CHECK(std::get<rbx::EnumItem>(v).value == 0);
    REQUIRE(aspect->get_prop("DominantAxis", v));
    CHECK(std::get<rbx::EnumItem>(v).value == 0);
}

TEST_CASE("AutomaticSize follows native content, layout and size-chain rules") {
    rbx::register_builtin_classes();
    auto screen = make("ScreenGui", "Hud");
    std::string err;
    setv(screen.get(), "IgnoreGuiInset", rbx::Variant(true));
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
    auto auto_size = [&](const char* mode, int value) {
        return rbx::Variant(rbx::EnumItem{mode, value});
    };

    // Default is None; all four enum values are exposed with native numbers.
    auto defaults = make("Frame", "Defaults");
    REQUIRE(defaults->reparent(screen.get(), &err));
    rbx::Variant v;
    REQUIRE(defaults->get_prop("AutomaticSize", v));
    CHECK(std::get<rbx::EnumItem>(v).value == 0);

    // TextWrap and TextWrapped are one native flag under two names.
    auto alias = make("TextLabel", "Alias");
    setv(alias.get(), "TextWrapped", rbx::Variant(true));
    REQUIRE(alias->get_prop("TextWrap", v));
    CHECK(std::get<bool>(v));
    setv(alias.get(), "TextWrap", rbx::Variant(false));
    REQUIRE(alias->get_prop("TextWrapped", v));
    CHECK(!std::get<bool>(v));

    // A child contributes its offset+scale size only on automatic axes.
    auto auto_frame = make("Frame", "Auto");
    auto child = make("Frame", "Child");
    REQUIRE(auto_frame->reparent(screen.get(), &err));
    REQUIRE(child->reparent(auto_frame.get(), &err));
    setv(auto_frame.get(), "AutomaticSize", auto_size("AutomaticSize", 3));
    setv(child.get(), "Position", rbx::Variant(udim(0, 10, 0, 20)));
    setv(child.get(), "Size", rbx::Variant(udim(0, 100, 0, 50)));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(auto_frame.get()).x == doctest::Approx(110.0));
    CHECK(size(auto_frame.get()).y == doctest::Approx(70.0));
    CHECK(abs(child.get()).x - abs(auto_frame.get()).x == doctest::Approx(10.0));
    CHECK(abs(child.get()).y - abs(auto_frame.get()).y == doctest::Approx(20.0));

    setv(auto_frame.get(), "AutomaticSize", auto_size("AutomaticSize", 1));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(auto_frame.get()).x == doctest::Approx(110.0));
    CHECK(size(auto_frame.get()).y == doctest::Approx(0.0));
    setv(auto_frame.get(), "AutomaticSize", auto_size("AutomaticSize", 2));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(auto_frame.get()).x == doctest::Approx(0.0));
    CHECK(size(auto_frame.get()).y == doctest::Approx(70.0));
    setv(auto_frame.get(), "AutomaticSize", auto_size("AutomaticSize", 0));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(auto_frame.get()).x == doctest::Approx(0.0));
    CHECK(size(auto_frame.get()).y == doctest::Approx(0.0));

    // Padding is additive; BorderSizePixel is not part of the content box.
    auto padded = make("Frame", "Padded");
    auto padded_child = make("Frame", "Child");
    auto padding = make("UIPadding", "Padding");
    REQUIRE(padded->reparent(screen.get(), &err));
    REQUIRE(padded_child->reparent(padded.get(), &err));
    REQUIRE(padding->reparent(padded.get(), &err));
    setv(padded.get(), "AutomaticSize", auto_size("AutomaticSize", 3));
    setv(padded.get(), "BorderSizePixel", rbx::Variant(int64_t(3)));
    setv(padded_child.get(), "Size", rbx::Variant(udim(0, 100, 0, 50)));
    setv(padding.get(), "PaddingLeft", rbx::Variant(rbx::UDim{0.0, 10.0}));
    setv(padding.get(), "PaddingRight", rbx::Variant(rbx::UDim{0.0, 10.0}));
    setv(padding.get(), "PaddingTop", rbx::Variant(rbx::UDim{0.0, 10.0}));
    setv(padding.get(), "PaddingBottom", rbx::Variant(rbx::UDim{0.0, 10.0}));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(padded.get()).x == doctest::Approx(120.0));
    CHECK(size(padded.get()).y == doctest::Approx(70.0));
    CHECK(abs(padded_child.get()).x - abs(padded.get()).x == doctest::Approx(10.0));
    CHECK(abs(padded_child.get()).y - abs(padded.get()).y == doctest::Approx(10.0));

    // Invisible children do not contribute.
    auto hidden_frame = make("Frame", "Hidden");
    auto visible_child = make("Frame", "Visible");
    auto hidden_child = make("Frame", "HiddenChild");
    REQUIRE(hidden_frame->reparent(screen.get(), &err));
    REQUIRE(visible_child->reparent(hidden_frame.get(), &err));
    REQUIRE(hidden_child->reparent(hidden_frame.get(), &err));
    setv(hidden_frame.get(), "AutomaticSize", auto_size("AutomaticSize", 3));
    setv(visible_child.get(), "Size", rbx::Variant(udim(0, 40, 0, 30)));
    setv(hidden_child.get(), "Position", rbx::Variant(udim(0, 100, 0, 100)));
    setv(hidden_child.get(), "Size", rbx::Variant(udim(0, 100, 0, 50)));
    setv(hidden_child.get(), "Visible", rbx::Variant(false));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(hidden_frame.get()).x == doctest::Approx(40.0));
    CHECK(size(hidden_frame.get()).y == doctest::Approx(30.0));

    // Nested automatic parents resolve inside out; a zero-sized non-auto
    // parent still lays out its automatic child.
    auto outer = make("Frame", "Outer");
    auto middle = make("Frame", "Middle");
    auto inner = make("Frame", "Inner");
    auto inner_child = make("Frame", "Leaf");
    REQUIRE(outer->reparent(screen.get(), &err));
    REQUIRE(middle->reparent(outer.get(), &err));
    REQUIRE(inner->reparent(middle.get(), &err));
    REQUIRE(inner_child->reparent(inner.get(), &err));
    setv(outer.get(), "AutomaticSize", auto_size("AutomaticSize", 3));
    setv(middle.get(), "AutomaticSize", auto_size("AutomaticSize", 3));
    setv(inner.get(), "AutomaticSize", auto_size("AutomaticSize", 3));
    setv(inner.get(), "Position", rbx::Variant(udim(0, 5, 0, 5)));
    setv(inner_child.get(), "Position", rbx::Variant(udim(0, 10, 0, 20)));
    setv(inner_child.get(), "Size", rbx::Variant(udim(0, 100, 0, 50)));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(inner.get()).x == doctest::Approx(110.0));
    CHECK(size(inner.get()).y == doctest::Approx(70.0));
    CHECK(size(outer.get()).x == doctest::Approx(115.0));
    CHECK(size(outer.get()).y == doctest::Approx(75.0));

    auto empty_parent = make("Frame", "EmptyParent");
    auto zero_child = make("Frame", "ZeroChild");
    REQUIRE(empty_parent->reparent(screen.get(), &err));
    REQUIRE(zero_child->reparent(empty_parent.get(), &err));
    setv(zero_child.get(), "AutomaticSize", auto_size("AutomaticSize", 3));
    setv(zero_child.get(), "Position", rbx::Variant(udim(0, 7, 0, 9)));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(zero_child.get()).x == doctest::Approx(0.0));
    CHECK(size(zero_child.get()).y == doctest::Approx(0.0));

    // UIListLayout contributes item sizes, padding and its parent UIPadding.
    auto list = make("Frame", "List");
    auto la = make("Frame", "A");
    auto lb = make("Frame", "B");
    auto list_layout = make("UIListLayout", "ListLayout");
    REQUIRE(list->reparent(screen.get(), &err));
    REQUIRE(la->reparent(list.get(), &err));
    REQUIRE(lb->reparent(list.get(), &err));
    REQUIRE(list_layout->reparent(list.get(), &err));
    setv(list.get(), "AutomaticSize", auto_size("AutomaticSize", 3));
    setv(la.get(), "Size", rbx::Variant(udim(0, 50, 0, 20)));
    setv(lb.get(), "Size", rbx::Variant(udim(0, 70, 0, 30)));
    setv(list_layout.get(), "Padding", rbx::Variant(rbx::UDim{0.0, 10.0}));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(list.get()).x == doctest::Approx(70.0));
    CHECK(size(list.get()).y == doctest::Approx(60.0));
    CHECK(abs(lb.get()).y - abs(list.get()).y == doctest::Approx(30.0));

    // Grids use cell size/padding; the parent padding adds to the result.
    auto grid = make("Frame", "Grid");
    REQUIRE(grid->reparent(screen.get(), &err));
    std::vector<rbx::Ref<rbx::Instance>> cells;
    for (int i = 0; i < 3; ++i) {
        auto cell = make("Frame", ("G" + std::to_string(i)).c_str());
        REQUIRE(cell->reparent(grid.get(), &err));
        cells.push_back(cell);
    }
    auto grid_layout = make("UIGridLayout", "GridLayout");
    REQUIRE(grid_layout->reparent(grid.get(), &err));
    setv(grid.get(), "AutomaticSize", auto_size("AutomaticSize", 3));
    setv(grid.get(), "Position", rbx::Variant(udim(0, 123, 0, 45)));
    setv(grid_layout.get(), "CellSize", rbx::Variant(udim(0, 60, 0, 40)));
    setv(grid_layout.get(), "CellPadding", rbx::Variant(udim(0, 4, 0, 6)));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(grid.get()).x == doctest::Approx(188.0));
    CHECK(size(grid.get()).y == doctest::Approx(40.0));
    setv(grid_layout.get(), "FillDirection",
         rbx::Variant(rbx::EnumItem{"FillDirection", 1}));
    auto grid_padding = make("UIPadding", "GridPadding");
    REQUIRE(grid_padding->reparent(grid.get(), &err));
    setv(grid_padding.get(), "PaddingLeft", rbx::Variant(rbx::UDim{0.0, 7.0}));
    setv(grid_padding.get(), "PaddingTop", rbx::Variant(rbx::UDim{0.0, 9.0}));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(grid.get()).x == doctest::Approx(67.0));
    CHECK(size(grid.get()).y == doctest::Approx(141.0));

    // Constraint runs before the content floor; the floor restores content
    // after a maximum shrink, while a minimum remains authoritative.
    auto constrained = make("Frame", "Constrained");
    auto constrained_child = make("Frame", "C");
    auto constraint = make("UISizeConstraint", "SizeConstraint");
    REQUIRE(constrained->reparent(screen.get(), &err));
    REQUIRE(constrained_child->reparent(constrained.get(), &err));
    REQUIRE(constraint->reparent(constrained.get(), &err));
    setv(constrained.get(), "AutomaticSize", auto_size("AutomaticSize", 3));
    setv(constrained_child.get(), "Size", rbx::Variant(udim(0, 100, 0, 50)));
    setv(constraint.get(), "MinSize", rbx::Variant(rbx::Vector2{0.0, 0.0}));
    setv(constraint.get(), "MaxSize", rbx::Variant(rbx::Vector2{50.0, 40.0}));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(constrained.get()).x == doctest::Approx(100.0));
    CHECK(size(constrained.get()).y == doctest::Approx(50.0));
    setv(constraint.get(), "MinSize", rbx::Variant(rbx::Vector2{300.0, 200.0}));
    setv(constraint.get(), "MaxSize", rbx::Variant(rbx::Vector2{1000.0, 1000.0}));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(constrained.get()).x == doctest::Approx(300.0));
    CHECK(size(constrained.get()).y == doctest::Approx(200.0));

    // Aspect fits the automatic content before the post-aspect floor.
    auto aspect_frame = make("Frame", "AspectX");
    auto aspect_child = make("Frame", "C");
    auto aspect = make("UIAspectRatioConstraint", "Aspect");
    REQUIRE(aspect_frame->reparent(screen.get(), &err));
    REQUIRE(aspect_child->reparent(aspect_frame.get(), &err));
    REQUIRE(aspect->reparent(aspect_frame.get(), &err));
    setv(aspect_frame.get(), "AutomaticSize", auto_size("AutomaticSize", 1));
    setv(aspect_frame.get(), "Size", rbx::Variant(udim(0, 0, 0, 50)));
    setv(aspect_child.get(), "Size", rbx::Variant(udim(0, 100, 0, 50)));
    setv(aspect.get(), "AspectRatio", rbx::Variant(3.0));
    setv(aspect.get(), "AspectType", rbx::Variant(rbx::EnumItem{"AspectType", 0}));
    setv(aspect.get(), "DominantAxis",
         rbx::Variant(rbx::EnumItem{"DominantAxis", 0}));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(aspect_frame.get()).x == doctest::Approx(100.0));
    CHECK(size(aspect_frame.get()).y == doctest::Approx(100.0 / 3.0));

    // Own UIScale runs last; a parent's UIScale scales automatic content too.
    auto scaled = make("Frame", "Scaled");
    auto scaled_child = make("Frame", "C");
    auto scale = make("UIScale", "Scale");
    REQUIRE(scaled->reparent(screen.get(), &err));
    REQUIRE(scaled_child->reparent(scaled.get(), &err));
    REQUIRE(scale->reparent(scaled.get(), &err));
    setv(scaled.get(), "AutomaticSize", auto_size("AutomaticSize", 3));
    setv(scaled_child.get(), "Size", rbx::Variant(udim(0, 100, 0, 50)));
    setv(scale.get(), "Scale", rbx::Variant(2.0));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(scaled.get()).x == doctest::Approx(200.0));
    CHECK(size(scaled.get()).y == doctest::Approx(100.0));

    auto holder = make("Frame", "Holder");
    auto scaled_auto = make("Frame", "ScaledAuto");
    auto holder_scale = make("UIScale", "HolderScale");
    auto scaled_auto_child = make("Frame", "C");
    REQUIRE(holder->reparent(screen.get(), &err));
    REQUIRE(scaled_auto->reparent(holder.get(), &err));
    REQUIRE(holder_scale->reparent(holder.get(), &err));
    REQUIRE(scaled_auto_child->reparent(scaled_auto.get(), &err));
    setv(holder.get(), "Size", rbx::Variant(udim(0, 400, 0, 300)));
    setv(scaled_auto.get(), "AutomaticSize", auto_size("AutomaticSize", 3));
    setv(scaled_auto.get(), "Position", rbx::Variant(udim(0, 10, 0, 10)));
    setv(scaled_auto_child.get(), "Size", rbx::Variant(udim(0, 100, 0, 50)));
    setv(holder_scale.get(), "Scale", rbx::Variant(2.0));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(scaled_auto.get()).x == doctest::Approx(200.0));
    CHECK(size(scaled_auto.get()).y == doctest::Approx(100.0));

    // Text measuring feeds AutomaticSize: plain Legacy bounds, wrapped
    // multi-line bounds, TextScaled's 1x1 floor and label padding.
    auto text_frame = make("Frame", "TextFrame");
    auto label = make("TextLabel", "T");
    REQUIRE(text_frame->reparent(screen.get(), &err));
    REQUIRE(label->reparent(text_frame.get(), &err));
    setv(text_frame.get(), "AutomaticSize", auto_size("AutomaticSize", 3));
    setv(label.get(), "Position", rbx::Variant(udim(0, 3, 0, 4)));
    setv(label.get(), "Text", rbx::Variant(std::string("Hello")));
    setv(label.get(), "TextSize", rbx::Variant(20.0));
    setv(label.get(), "AutomaticSize", auto_size("AutomaticSize", 3));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(text_frame.get()).x == doctest::Approx(64.0));
    CHECK(size(text_frame.get()).y == doctest::Approx(34.0));
    CHECK(size(label.get()).x == doctest::Approx(61.0));
    CHECK(size(label.get()).y == doctest::Approx(30.0));

    auto wrap_holder = make("Frame", "WrapHolder");
    auto wrapped = make("TextLabel", "Wrapped");
    REQUIRE(wrap_holder->reparent(screen.get(), &err));
    REQUIRE(wrapped->reparent(wrap_holder.get(), &err));
    setv(wrapped.get(), "Size", rbx::Variant(udim(0, 100, 0, 0)));
    setv(wrapped.get(), "Text",
         rbx::Variant(std::string("Hello world this is a long line")));
    setv(wrapped.get(), "TextSize", rbx::Variant(20.0));
    setv(wrapped.get(), "TextWrap", rbx::Variant(true));
    setv(wrapped.get(), "AutomaticSize", auto_size("AutomaticSize", 2));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(wrapped.get()).x == doctest::Approx(100.0));
    CHECK(size(wrapped.get()).y == doctest::Approx(120.0));
    rbx::Variant bounds;
    REQUIRE(wrapped->get_prop("TextBounds", bounds));
    CHECK(std::get<rbx::Vector2>(bounds).x == doctest::Approx(100.0));
    CHECK(std::get<rbx::Vector2>(bounds).y == doctest::Approx(120.0));

    auto scaled_holder = make("Frame", "ScaledTextHolder");
    auto scaled_label = make("TextLabel", "ScaledText");
    REQUIRE(scaled_holder->reparent(screen.get(), &err));
    REQUIRE(scaled_label->reparent(scaled_holder.get(), &err));
    setv(scaled_label.get(), "Text", rbx::Variant(std::string("Hello")));
    setv(scaled_label.get(), "TextSize", rbx::Variant(20.0));
    setv(scaled_label.get(), "TextScaled", rbx::Variant(true));
    setv(scaled_label.get(), "AutomaticSize", auto_size("AutomaticSize", 3));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(scaled_label.get()).x == doctest::Approx(1.0));
    CHECK(size(scaled_label.get()).y == doctest::Approx(1.0));

    // TextScaled inside a fully automatic parent uses native's 100px-line
    // circular fallback; the parent and child settle on the same content.
    auto nested_scaled_holder = make("Frame", "NestedScaledHolder");
    auto nested_scaled = make("TextLabel", "NestedScaled");
    REQUIRE(nested_scaled_holder->reparent(screen.get(), &err));
    REQUIRE(nested_scaled->reparent(nested_scaled_holder.get(), &err));
    setv(nested_scaled_holder.get(), "AutomaticSize",
         auto_size("AutomaticSize", 3));
    setv(nested_scaled.get(), "Text", rbx::Variant(std::string("Hello")));
    setv(nested_scaled.get(), "TextSize", rbx::Variant(20.0));
    setv(nested_scaled.get(), "TextScaled", rbx::Variant(true));
    setv(nested_scaled.get(), "AutomaticSize", auto_size("AutomaticSize", 3));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(nested_scaled_holder.get()).x == doctest::Approx(201.0));
    CHECK(size(nested_scaled_holder.get()).y == doctest::Approx(100.0));
    CHECK(size(nested_scaled.get()).x == doctest::Approx(201.0));
    CHECK(size(nested_scaled.get()).y == doctest::Approx(100.0));
    rbx::Variant nested_bounds;
    REQUIRE(nested_scaled->get_prop("TextBounds", nested_bounds));
    CHECK(std::get<rbx::Vector2>(nested_bounds).x == doctest::Approx(201.0));
    CHECK(std::get<rbx::Vector2>(nested_bounds).y == doctest::Approx(100.0));

    setv(nested_scaled.get(), "Text",
         rbx::Variant(std::string("Hello world this is a long line")));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(nested_scaled_holder.get()).x == doctest::Approx(1110.0));
    CHECK(size(nested_scaled_holder.get()).y == doctest::Approx(100.0));
    setv(nested_scaled.get(), "Text", rbx::Variant(std::string("")));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(nested_scaled_holder.get()).x == doctest::Approx(0.0));
    CHECK(size(nested_scaled_holder.get()).y == doctest::Approx(100.0));
    REQUIRE(nested_scaled->get_prop("TextBounds", nested_bounds));
    CHECK(std::get<rbx::Vector2>(nested_bounds).x == doctest::Approx(0.0));
    CHECK(std::get<rbx::Vector2>(nested_bounds).y == doctest::Approx(100.0));
    setv(nested_scaled.get(), "Text", rbx::Variant(std::string("Hello\nHello")));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(nested_scaled_holder.get()).x == doctest::Approx(201.0));
    CHECK(size(nested_scaled_holder.get()).y == doctest::Approx(200.0));
    REQUIRE(nested_scaled->get_prop("TextBounds", nested_bounds));
    CHECK(std::get<rbx::Vector2>(nested_bounds).y == doctest::Approx(200.0));
    setv(nested_scaled.get(), "Text", rbx::Variant(std::string("Hello")));
    auto nested_padding = make("UIPadding", "NestedScaledPadding");
    REQUIRE(nested_padding->reparent(nested_scaled.get(), &err));
    setv(nested_padding.get(), "PaddingLeft", rbx::Variant(rbx::UDim{0.0, 5.0}));
    setv(nested_padding.get(), "PaddingTop", rbx::Variant(rbx::UDim{0.0, 5.0}));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(nested_scaled_holder.get()).x == doctest::Approx(206.0));
    CHECK(size(nested_scaled_holder.get()).y == doctest::Approx(105.0));
    REQUIRE(nested_scaled->get_prop("TextBounds", nested_bounds));
    CHECK(std::get<rbx::Vector2>(nested_bounds).x == doctest::Approx(201.0));
    CHECK(std::get<rbx::Vector2>(nested_bounds).y == doctest::Approx(100.0));

    // Automatic X removes the wrap width: native measures the whole line.
    auto nested_wrap_holder = make("Frame", "NestedWrapHolder");
    auto nested_wrap = make("TextLabel", "NestedWrap");
    REQUIRE(nested_wrap_holder->reparent(screen.get(), &err));
    REQUIRE(nested_wrap->reparent(nested_wrap_holder.get(), &err));
    setv(nested_wrap_holder.get(), "AutomaticSize",
         auto_size("AutomaticSize", 3));
    setv(nested_wrap.get(), "Size", rbx::Variant(udim(0, 100, 0, 0)));
    setv(nested_wrap.get(), "Text", rbx::Variant(std::string("Hello world")));
    setv(nested_wrap.get(), "TextSize", rbx::Variant(20.0));
    setv(nested_wrap.get(), "TextWrap", rbx::Variant(true));
    setv(nested_wrap.get(), "AutomaticSize", auto_size("AutomaticSize", 3));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(nested_wrap_holder.get()).x == doctest::Approx(132.0));
    CHECK(size(nested_wrap_holder.get()).y == doctest::Approx(30.0));
    CHECK(size(nested_wrap.get()).x == doctest::Approx(132.0));
    CHECK(size(nested_wrap.get()).y == doctest::Approx(30.0));
    rbx::Variant wrap_bounds;
    REQUIRE(nested_wrap->get_prop("TextBounds", wrap_bounds));
    CHECK(std::get<rbx::Vector2>(wrap_bounds).x == doctest::Approx(132.0));
    CHECK(std::get<rbx::Vector2>(wrap_bounds).y == doctest::Approx(30.0));

    auto pad_holder = make("Frame", "TextPadHolder");
    auto pad_label = make("TextLabel", "PaddedText");
    auto label_padding = make("UIPadding", "LabelPadding");
    REQUIRE(pad_holder->reparent(screen.get(), &err));
    REQUIRE(pad_label->reparent(pad_holder.get(), &err));
    REQUIRE(label_padding->reparent(pad_label.get(), &err));
    setv(pad_label.get(), "Text", rbx::Variant(std::string("Hello")));
    setv(pad_label.get(), "TextSize", rbx::Variant(20.0));
    setv(pad_label.get(), "AutomaticSize", auto_size("AutomaticSize", 3));
    setv(label_padding.get(), "PaddingLeft", rbx::Variant(rbx::UDim{0.0, 5.0}));
    setv(label_padding.get(), "PaddingTop", rbx::Variant(rbx::UDim{0.0, 6.0}));
    rbxclient::collectGui(screen.get(), 800, 600);
    CHECK(size(pad_label.get()).x == doctest::Approx(66.0));
    CHECK(size(pad_label.get()).y == doctest::Approx(36.0));
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
