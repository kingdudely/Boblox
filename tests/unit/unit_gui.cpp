// unit_gui.cpp — ScreenGui layout, hit-testing and real input/event routing.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "engine.h"
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
