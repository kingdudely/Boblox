// gui.cpp — the Engine-profile GUI surface.
//
// Two layers with different lifetimes:
//
//   * Datatypes (register_gui_datatypes) are part of the Luau surface and are
//     registered unconditionally. A replicated LocalScript may call
//     Font.new even on a run that draws nothing; the native client always has
//     Font, so dropping it would turn a working script into "attempt to index
//     nil with 'new'". TextLabel/TextButton/TextBox.FontFace belongs here
//     too (the pinned API dump types it as Font, not a separate datatype).
//   * The client-side tree (attach_gui) is PlayerGui under each Player and a
//     default Workspace.CurrentCamera. It is installed when Options::gui is
//     on (--gui, on by default). The GUI tree is a real part of the DataModel:
//     scripts parent ScreenGuis to PlayerGui, wait for ChildAdded, and set
//     Camera properties exactly as they do in the native client.
//
// Drawing is the renderer's job; neither layer opens a window or reads an
// asset. --no-gui skips the tree (and therefore PlayerGui) for clients that
// want the absolute minimum, at the cost of parity for GUI scripts.
#include "api/api.h"

#include "instance/class_registry.h"
#include "instance/instance.h"
#include "instance/value_types.h"

#include "lua.h"
#include "lualib.h"

#include <string>
#include <variant>

namespace rbxch {
namespace api {

namespace {

void add_font_face_property(const char* class_name) {
    rbx::attach_property(
        class_name,
        rbx::PropInfo{"FontFace", rbx::PropType::Font, rbx::Font{}, false});
}

// Create `name` under `parent` unless it already exists. The parent owns the
// returned instance strongly; the local Ref is only a construction handle.
rbx::Instance* ensure_child(rbx::Instance* parent, const char* class_name,
                            const char* name) {
    if (!parent)
        return nullptr;
    if (rbx::Instance* existing = parent->find_first_child(name))
        return existing;
    const rbx::ClassInfo* cls = rbx::find_class(class_name);
    if (!cls)
        return nullptr;
    rbx::Ref<rbx::Instance> inst = rbx::instantiate(cls);
    inst->set_name(name);
    std::string err;
    if (!inst->reparent(parent, &err))
        return nullptr;
    return inst.get();
}

} // namespace

void register_gui_datatypes(lua_State* L) {
    rbx::create_font_class(L);
    add_font_face_property("TextLabel");
    add_font_face_property("TextButton");
    add_font_face_property("TextBox");

    // The API dump leaves presentation defaults as placeholders and omits
    // ScreenGui.Enabled entirely. Scripts read these properties directly, so
    // make the effective defaults native instead of only compensating in the
    // renderer/hit-test path.
    rbx::attach_property(
        "ScreenGui",
        rbx::PropInfo{"Enabled", rbx::PropType::Bool, true, false});
    rbx::attach_property(
        "GuiObject",
        rbx::PropInfo{"Visible", rbx::PropType::Bool, true, false});
    rbx::attach_property(
        "GuiObject",
        rbx::PropInfo{"Interactable", rbx::PropType::Bool, true, false});
    rbx::attach_property("GuiButton",
                         rbx::PropInfo{"Active", rbx::PropType::Bool, true,
                                       false});

    // Auto-layout defaults, measured in native 2.743 (place 1818).  The API
    // dump leaves enum/UDim values at zero, which is not native: UIListLayout
    // fills vertically with Left/Top alignment, and UIGridLayout uses 100x100
    // cells with 5 px padding.  Scripts read these before setting anything, so
    // leaving the generated zeroes changes both layout and property readback.
    rbx::attach_property(
        "UIListLayout",
        rbx::PropInfo{"FillDirection", rbx::PropType::Enum,
                      rbx::EnumItem{"FillDirection", 1}, false, "FillDirection"});
    rbx::attach_property(
        "UIListLayout",
        rbx::PropInfo{"HorizontalAlignment", rbx::PropType::Enum,
                      rbx::EnumItem{"HorizontalAlignment", 1}, false,
                      "HorizontalAlignment"});
    rbx::attach_property(
        "UIListLayout",
        rbx::PropInfo{"VerticalAlignment", rbx::PropType::Enum,
                      rbx::EnumItem{"VerticalAlignment", 1}, false,
                      "VerticalAlignment"});
    rbx::attach_property(
        "UIListLayout",
        rbx::PropInfo{"SortOrder", rbx::PropType::Enum,
                      rbx::EnumItem{"SortOrder", 0}, false, "SortOrder"});
    rbx::attach_property(
        "UIListLayout",
        rbx::PropInfo{"Padding", rbx::PropType::UDim, rbx::UDim{0.0, 0.0},
                      false});
    rbx::attach_property(
        "UIGridLayout",
        rbx::PropInfo{"CellSize", rbx::PropType::UDim2,
                      rbx::UDim2{rbx::UDim{0.0, 100.0},
                                 rbx::UDim{0.0, 100.0}},
                      false});
    rbx::attach_property(
        "UIGridLayout",
        rbx::PropInfo{"CellPadding", rbx::PropType::UDim2,
                      rbx::UDim2{rbx::UDim{0.0, 5.0},
                                 rbx::UDim{0.0, 5.0}},
                      false});
    rbx::attach_property(
        "UIGridLayout",
        rbx::PropInfo{"HorizontalAlignment", rbx::PropType::Enum,
                      rbx::EnumItem{"HorizontalAlignment", 1}, false,
                      "HorizontalAlignment"});
    rbx::attach_property(
        "UIGridLayout",
        rbx::PropInfo{"VerticalAlignment", rbx::PropType::Enum,
                      rbx::EnumItem{"VerticalAlignment", 1}, false,
                      "VerticalAlignment"});
    rbx::attach_property(
        "UIGridLayout",
        rbx::PropInfo{"SortOrder", rbx::PropType::Enum,
                      rbx::EnumItem{"SortOrder", 0}, false, "SortOrder"});
}

void attach_gui(lua_State* L, rbx::Instance* root) {
    (void)L;
    if (!root)
        return;

    rbx::Instance* ws = root->find_first_child_of_class("Workspace");
    if (!ws && root->is_a("Workspace"))
        ws = root;
    if (ws) {
        rbx::Variant current;
        bool have = ws->get_prop("CurrentCamera", current);
        const auto* camera = std::get_if<rbx::Instance*>(&current);
        if (!have || !camera || !*camera) {
            rbx::Instance* cam = ensure_child(ws, "Camera", "Camera");
            if (cam) {
                std::string err;
                ws->set_prop("CurrentCamera", rbx::Variant(cam), &err);
            }
        }
    }

    // The client owns PlayerGui; the server never replicates it. A LocalScript
    // that does player:WaitForChild("PlayerGui") must find it immediately,
    // exactly like the native client.
    rbx::Instance* players = root->find_first_child_of_class("Players");
    if (!players)
        return;
    for (const auto& child : players->children()) {
        if (child->is_a("Player"))
            ensure_child(child.get(), "PlayerGui", "PlayerGui");
    }
}

} // namespace api
} // namespace rbxch
