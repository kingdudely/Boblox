// local_player.h — the client-owned first-person/third-person avatar.
//
// Jolt owns collision; this class owns the Roblox policy around it:
//   * WASD/jump -> Humanoid walk speed + jump power,
//   * mouse -> the 2016 CameraScript yaw/pitch/distance,
//   * Workspace.CurrentCamera.CFrame is written every tick,
//   * the local HumanoidRootPart/parts get a local CFrame override so the
//     existing 0x83/0x05 pose upload reports where we actually are.
//
// It is presentation-independent: headless and headful runs drive the same
// object, and the viewer only reads `view_camera()`.
#pragma once

#include "camera/camera_controller.h"
#include "physics/physics.h"

#include <cstdint>
#include <map>
#include <vector>

namespace rbx {
class Instance;
}

namespace rbxclient {

class LocalPlayer : public rbx::contact::Backend {
  public:
    LocalPlayer();
    ~LocalPlayer();

    // Attach to a replicated/local Character and the Workspace that contains
    // the world.  Idempotent for the same instances; a changed character
    // detaches the old one and rebuilds the world without its subtree.
    bool attach(rbx::Instance* workspace, rbx::Instance* character);
    void detach();
    bool attached() const { return controller_.attached(); }

    // Advance input, physics, the character and the camera.  `workspace` and
    // `character` are re-checked every tick so a replicated respawn or a
    // destroyed tree detaches cleanly.
    void tick(double dt, rbx::Instance* workspace, rbx::Instance* character);

    const CameraState& camera() const { return camera_.state(); }
    const rbx::physics::CharacterState& character() const {
        return controller_.state();
    }
    bool view_camera(CameraState* out) const;
    bool has_physics() const { return world_.size() != 0; }
    // True while the keyboard/controller supplies a non-zero move vector.
    // The A6 pump uses this to decide whether the local avatar owns the
    // control state or the executor's boblox.move shim does.
    bool has_move_input() const;

    // rbx::contact::Backend: the runtime's GetTouchingParts/IsGrounded and
    // Touched/TouchEnded dispatcher read the same live Jolt state.
    void touching_parts(rbx::Instance* part,
                        std::vector<rbx::Instance*>* out) override;
    bool grounded(rbx::Instance* part) override;
    std::vector<rbx::contact::Pair> take_begun() override;
    std::vector<rbx::contact::Pair> take_ended() override;

    // Numeric NetworkOwnerV3 value granted to this client (0 until the
    // server replicates a grant for our avatar).  The world decisions follow
    // Workspace.AuthorityMode, not this value alone.
    void set_network_owner_id(std::uint64_t id) { network_owner_id_ = id; }

    // Locally simulated bodies whose pose changed since the previous call.
    // The network layer maps them to replication ids and uploads owner CFrames.
    std::vector<rbx::physics::PartUpdate> take_physics_updates() {
        return world_.take_updates();
    }

    // The 240 Hz CharacterController samples produced by the most recent tick.
    // The network layer feeds these into the long 0xA6 ControlState frame so
    // its four slots carry successive samples, not four copies of one state.
    std::vector<rbx::Vector3> take_control_samples() {
        std::vector<rbx::Vector3> out;
        out.swap(control_samples_);
        return out;
    }
    rbx::physics::WorldStats physics_stats() const { return world_.stats(); }

  private:
    void read_input(double dt);
    void rebuild_world(rbx::Instance* workspace, rbx::Instance* character);
    void update_contacts();

    rbx::physics::World world_;
    rbx::physics::CharacterController controller_;
    CameraController camera_;
    rbx::Instance* workspace_ = nullptr;
    rbx::Instance* character_ = nullptr;
    rbx::Instance* camera_instance_ = nullptr;
    double refresh_accum_ = 0.0;
    double physics_accum_ = 0.0;
    std::uint64_t network_owner_id_ = 0;
    // Workspace.AuthorityMode == Automatic.  Only a locally simulated
    // character reports the capsule's ground state; a replicated
    // server-authority avatar leaves HumanoidRootPart:IsGrounded false.
    bool client_authority_ = false;

    std::vector<rbx::contact::Pair> contact_begun_;
    std::vector<rbx::contact::Pair> contact_ended_;
    std::map<rbx::Instance*, rbx::Ref<rbx::Instance>> character_touching_;
    std::vector<rbx::Vector3> control_samples_;

    bool last_right_ = false;
    double last_mouse_x_ = 0.0;
    double last_mouse_y_ = 0.0;
    bool jump_held_ = false;
    bool camera_initialized_ = false;
};

} // namespace rbxclient
