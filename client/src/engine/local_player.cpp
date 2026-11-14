// local_player.cpp — see local_player.h.
#include "engine/local_player.h"

#include "instance/host_input.h"
#include "instance/instance.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace rbxclient {

namespace {

rbx::Instance* current_camera(rbx::Instance* workspace) {
    if (!workspace)
        return nullptr;
    rbx::Variant v;
    if (!workspace->get_prop("CurrentCamera", v))
        return nullptr;
    const auto* p = std::get_if<rbx::Instance*>(&v);
    return p ? *p : nullptr;
}

} // namespace

LocalPlayer::LocalPlayer() = default;
LocalPlayer::~LocalPlayer() { detach(); }

bool LocalPlayer::attach(rbx::Instance* workspace, rbx::Instance* character) {
    if (!workspace || !character || character->destroyed())
        return false;
    if (character_ == character && controller_.attached())
        return true;
    controller_.detach();
    world_.clear();
    character_ = character;
    workspace_ = workspace;
    camera_instance_ = current_camera(workspace);
    if (!controller_.attach(&world_, character)) {
        character_ = nullptr;
        return false;
    }
    rebuild_world(workspace, character);
    rbx::contact::set_backend(this);
    refresh_accum_ = 0.0;
    physics_accum_ = 0.0;
    control_samples_.clear();
    camera_.reset();
    // Start behind the avatar with the avatar's own facing, exactly like the
    // native camera snapping on spawn.
    const auto& cf = controller_.state().cframe;
    camera_.set_yaw(std::atan2(cf.r02, cf.r22));
    camera_initialized_ = true;
    const auto m = rbx::input::mouse();
    last_mouse_x_ = m.x;
    last_mouse_y_ = m.y;
    last_right_ = m.button2;
    return true;
}

void LocalPlayer::detach() {
    if (rbx::contact::backend() == this)
        rbx::contact::set_backend(nullptr);
    controller_.detach();
    world_.clear();
    workspace_ = nullptr;
    character_ = nullptr;
    camera_instance_ = nullptr;
    camera_initialized_ = false;
    contact_begun_.clear();
    contact_ended_.clear();
    character_touching_.clear();
    control_samples_.clear();
}

void LocalPlayer::rebuild_world(rbx::Instance* workspace,
                                rbx::Instance* character) {
    // Authority is a Workspace property, not a per-place constant:
    //   Server     -> every replicated assembly is kinematic (Crossroads),
    //   Automatic  -> NetworkOwnerV3 grants decide local simulation (NDS).
    // The character's own subtree is skipped: its collision is the
    // CharacterVirtual capsule, not its cosmetic parts.
    rbx::Variant authority;
    bool automatic = false;
    if (workspace->get_prop("AuthorityMode", authority))
        if (const auto* e = std::get_if<rbx::EnumItem>(&authority))
            automatic = e->value == 1; // Enum.AuthorityMode.Automatic
    client_authority_ = automatic;
    rbx::Variant gravity;
    if (workspace->get_prop("Gravity", gravity))
        if (const auto* g = std::get_if<double>(&gravity); g && *g >= 0.0)
            world_.set_gravity(*g);
    world_.set_authority(automatic, network_owner_id_);
    world_.sync(workspace, character);
}

void LocalPlayer::read_input(double dt) {
    (void)dt;
    const double cp = std::cos(camera_.pitch());
    const rbx::Vector3 forward{std::sin(camera_.yaw()) * cp, 0.0,
                               std::cos(camera_.yaw()) * cp};
    rbx::Vector3 f = rbx::physics::vnormalize(forward);
    // Camera-right: for a look vector +Z this is -X (the same basis the
    // renderer's lookAt uses), so D moves toward the right of the screen.
    const rbx::Vector3 right = rbx::physics::vnormalize(
        rbx::physics::vcross(f, {0.0, 1.0, 0.0}));
    // A focused TextBox owns the keyboard: WASD/Space must type, not move or
    // jump.  Camera drag is likewise suppressed while a ScreenGui owns the
    // pointer (the GUI dispatcher sets both flags from the actual hit test).
    const bool keyboard_owned = rbx::input::keyboard_captured();
    rbx::Vector3 move{};
    if (!keyboard_owned) {
        if (rbx::input::key_down(119)) // W
            move = rbx::physics::vadd(move, f);
        if (rbx::input::key_down(115)) // S
            move = rbx::physics::vsub(move, f);
        if (rbx::input::key_down(100)) // D
            move = rbx::physics::vadd(move, right);
        if (rbx::input::key_down(97)) // A
            move = rbx::physics::vsub(move, right);
    }
    if (rbx::physics::vlength(move) > 1.0)
        move = rbx::physics::vmul(move, 1.0 / rbx::physics::vlength(move));
    controller_.set_move_input(move);

    const bool jump = !keyboard_owned && rbx::input::key_down(32); // Space
    if (jump && !jump_held_)
        controller_.request_jump(true);
    jump_held_ = jump;

    const auto m = rbx::input::mouse();
    const bool pointer_owned = rbx::input::pointer_captured();
    if (m.button2 && !last_right_ && !pointer_owned) {
        last_mouse_x_ = m.x;
        last_mouse_y_ = m.y;
    }
    // Right-drag looks, matching the viewer's existing orbit gesture and
    // keeping the pointer free for GUI input.
    if (m.button2 && last_right_ && !pointer_owned)
        camera_.add_mouse_delta(m.x - last_mouse_x_, m.y - last_mouse_y_);
    last_mouse_x_ = m.x;
    last_mouse_y_ = m.y;
    last_right_ = m.button2;
}

void LocalPlayer::tick(double dt, rbx::Instance* workspace,
                       rbx::Instance* character) {
    if (!workspace || !character || character->destroyed()) {
        detach();
        return;
    }
    if (workspace != workspace_ || character != character_ ||
        !controller_.attached()) {
        if (!attach(workspace, character))
            return;
    }
    if (!(dt > 0.0))
        return;

    read_input(dt);
    // Replication streams bodies and property changes continuously.  A 30 Hz
    // reconciliation keeps collisions current without rebuilding the world.
    refresh_accum_ += dt;
    if (refresh_accum_ >= 1.0 / 30.0) {
        refresh_accum_ = 0.0;
        rebuild_world(workspace, character);
    }

    // The native solver runs four 240 Hz steps per 60 Hz frame.  Character
    // and rigid bodies share that cadence; Jolt flushes the resulting poses
    // to the replicated instances once per render frame.
    physics_accum_ = std::min(physics_accum_ + dt, 0.1);
    int budget = 16;
    while (physics_accum_ + 1e-12 >= rbx::physics::kPhysicsStep &&
           budget-- > 0) {
        controller_.step(rbx::physics::kPhysicsStep);
        if (control_samples_.size() < 4)
            control_samples_.push_back(controller_.state().move_direction);
        physics_accum_ -= rbx::physics::kPhysicsStep;
    }
    world_.step(dt);
    update_contacts();

    const auto& s = controller_.state();
    const std::vector<const rbx::Instance*> ignore{character};
    camera_.update(dt, s.position, &world_, &ignore);
    if (camera_instance_) {
        std::string err;
        camera_instance_->set_prop("CFrame", rbx::Variant(camera_.state().cframe()),
                                   &err);
    }
}

bool LocalPlayer::view_camera(CameraState* out) const {
    if (!out || !controller_.attached() || !camera_initialized_)
        return false;
    *out = camera_.state();
    return true;
}

bool LocalPlayer::has_move_input() const {
    const rbx::Vector3& d = controller_.state().move_direction;
    return d.x * d.x + d.y * d.y + d.z * d.z > 1e-12;
}

void LocalPlayer::update_contacts() {
    auto world_begun = world_.take_contact_begun();
    contact_begun_.insert(contact_begun_.end(),
                          std::make_move_iterator(world_begun.begin()),
                          std::make_move_iterator(world_begun.end()));
    auto world_ended = world_.take_contact_ended();
    contact_ended_.insert(contact_ended_.end(),
                          std::make_move_iterator(world_ended.begin()),
                          std::make_move_iterator(world_ended.end()));

    std::vector<rbx::Instance*> touching;
    controller_.touching_parts(&touching);
    std::map<rbx::Instance*, rbx::Ref<rbx::Instance>> next;
    for (rbx::Instance* part : touching)
        if (part && !part->destroyed())
            next.emplace(part, rbx::Ref<rbx::Instance>(part));

    rbx::Instance* root = controller_.root();
    if (root) {
        for (const auto& old : character_touching_)
            if (!next.count(old.first)) {
                rbx::contact::Pair pair;
                pair.a = rbx::Ref<rbx::Instance>(root);
                pair.b = old.second;
                contact_ended_.push_back(std::move(pair));
            }
        for (const auto& now : next)
            if (!character_touching_.count(now.first)) {
                rbx::contact::Pair pair;
                pair.a = rbx::Ref<rbx::Instance>(root);
                pair.b = now.second;
                contact_begun_.push_back(std::move(pair));
            }
    }
    character_touching_.swap(next);
}

void LocalPlayer::touching_parts(rbx::Instance* part,
                                 std::vector<rbx::Instance*>* out) {
    if (!out)
        return;
    out->clear();
    if (!part)
        return;
    if (controller_.attached() && part == controller_.root()) {
        controller_.touching_parts(out);
        return;
    }
    std::vector<const rbx::Instance*> hits;
    world_.touching_parts(part, &hits);
    for (const rbx::Instance* hit : hits)
        if (hit && !hit->destroyed())
            out->push_back(const_cast<rbx::Instance*>(hit));
}

bool LocalPlayer::grounded(rbx::Instance* part) {
    if (!part)
        return false;
    if (controller_.attached() && part == controller_.root())
        // A server-authority world replicates the avatar pose; the native
        // client does not claim capsule grounding for that replicated part.
        return client_authority_ && controller_.state().grounded;
    return world_.grounded(part);
}

std::vector<rbx::contact::Pair> LocalPlayer::take_begun() {
    std::vector<rbx::contact::Pair> out;
    out.swap(contact_begun_);
    return out;
}

std::vector<rbx::contact::Pair> LocalPlayer::take_ended() {
    std::vector<rbx::contact::Pair> out;
    out.swap(contact_ended_);
    return out;
}

} // namespace rbxclient
