// unit_physics.cpp — Jolt world + character behavior, without a socket.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "instance/class_registry.h"
#include "instance/instance.h"
#include "instance/variant.h"
#include "engine/local_player.h"
#include "physics/physics.h"

#include <algorithm>
#include <cmath>
#include <string>

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

void mark_remote(rbx::Instance* inst) {
    // A replicated part has a server identity; a script-created local part
    // does not and is simulated by the native client.
    inst->set_network_identity(1, 100);
}

rbx::CFrame at(double x, double y, double z) {
    rbx::CFrame f;
    f.x = x;
    f.y = y;
    f.z = z;
    return f;
}

struct Fixture {
    rbx::Ref<rbx::Instance> world_root;
    rbx::Ref<rbx::Instance> floor;
    rbx::Ref<rbx::Instance> wall;
    rbx::Ref<rbx::Instance> character;
    rbx::Ref<rbx::Instance> root_part;
    rbx::Ref<rbx::Instance> humanoid;
    rbx::physics::World world;
    rbx::physics::CharacterController controller;

    Fixture() {
        rbx::register_builtin_classes();
        world_root = make("Model", "Workspace");
        floor = make("Part", "Floor");
        setv(floor.get(), "Size", rbx::Variant(rbx::Vector3{100, 1, 100}));
        setv(floor.get(), "CFrame",
             rbx::Variant(at(0, -0.5, 0)));
        setv(floor.get(), "Anchored", rbx::Variant(true));
        wall = make("Part", "Wall");
        setv(wall.get(), "Size", rbx::Variant(rbx::Vector3{1, 5, 10}));
        setv(wall.get(), "CFrame",
             rbx::Variant(at(2.5, 2.5, 0)));
        setv(wall.get(), "Anchored", rbx::Variant(true));
        std::string err;
        REQUIRE(floor->reparent(world_root.get(), &err));
        REQUIRE(wall->reparent(world_root.get(), &err));

        character = make("Model", "Character");
        humanoid = make("Humanoid", "Humanoid");
        root_part = make("Part", "HumanoidRootPart");
        setv(root_part.get(), "Size", rbx::Variant(rbx::Vector3{2, 5, 1}));
        setv(root_part.get(), "CFrame",
             rbx::Variant(at(0, 3, 0)));
        REQUIRE(root_part->reparent(character.get(), &err));
        REQUIRE(humanoid->reparent(character.get(), &err));

        world.rebuild(world_root.get(), false, character.get());
        REQUIRE(world.size() == 2);
    }
};

} // namespace

TEST_CASE("Jolt world builds static parts and answers a ray") {
    Fixture f;
    rbx::physics::RayHit hit;
    CHECK(f.world.raycast({0, 10, 0}, {0, -1, 0}, 20, &hit));
    REQUIRE(hit.hit);
    CHECK(hit.instance == f.floor.get());
    CHECK(hit.distance == doctest::Approx(10.0).epsilon(0.01));
    CHECK(hit.position.y == doctest::Approx(0.0).epsilon(0.02));
    CHECK(hit.normal.y == doctest::Approx(1.0).epsilon(0.01));
}

TEST_CASE("character falls to the floor and reports Running") {
    Fixture f;
    REQUIRE(f.controller.attach(&f.world, f.character.get()));
    for (int i = 0; i < 120; ++i)
        f.controller.step(1.0 / 60.0);
    const auto& s = f.controller.state();
    REQUIRE(s.attached);
    CHECK(s.grounded);
    CHECK(s.position.y == doctest::Approx(2.5).epsilon(0.05));
    CHECK(s.state == 8);
}

TEST_CASE("jump leaves the ground and gravity brings it back") {
    Fixture f;
    REQUIRE(f.controller.attach(&f.world, f.character.get()));
    for (int i = 0; i < 120; ++i)
        f.controller.step(1.0 / 60.0);
    const double ground = f.controller.state().position.y;
    f.controller.request_jump(true);
    f.controller.step(1.0 / 60.0);
    CHECK(f.controller.state().state == 3);
    CHECK(f.controller.state().position.y > ground);
    for (int i = 0; i < 180; ++i)
        f.controller.step(1.0 / 60.0);
    CHECK(f.controller.state().grounded);
    CHECK(f.controller.state().position.y == doctest::Approx(ground).epsilon(0.05));
}

TEST_CASE("wall blocks horizontal movement") {
    Fixture f;
    REQUIRE(f.controller.attach(&f.world, f.character.get()));
    for (int i = 0; i < 120; ++i)
        f.controller.step(1.0 / 60.0);
    f.controller.set_move_input({1, 0, 0});
    for (int i = 0; i < 180; ++i)
        f.controller.step(1.0 / 60.0);
    // Capsule radius 0.9; wall inner face is x=2, so the centre cannot pass
    // 1.1 plus a little padding.
    CHECK(f.controller.state().position.x < 1.25);
    CHECK(f.controller.state().position.x > 0.5);
}

TEST_CASE("overlap reports the floor push normal") {
    Fixture f;
    double depth = 0.0;
    rbx::Vector3 normal;
    CHECK(f.world.overlaps({0, 0.4, 0}, {1, 0.5, 1}, nullptr, &depth,
                           &normal));
    CHECK(depth > 0.0);
    CHECK(normal.y > 0.9);
    CHECK_FALSE(f.world.overlaps({0, 10, 0}, {0.5, 0.5, 0.5}, nullptr));
}

TEST_CASE("controller retains avatar parts when the tree drops them") {
    Fixture f;
    REQUIRE(f.controller.attach(&f.world, f.character.get()));
    rbx::Instance* character = f.character.get();
    rbx::Instance* root = f.root_part.get();
    // Replication can replace the local avatar while the controller is still
    // stepping. The controller must own strong references instead of leaving
    // its pose writes pointed at freed instances.
    f.character = nullptr;
    f.root_part = nullptr;
    f.humanoid = nullptr;
    REQUIRE(f.controller.attached());
    CHECK(f.controller.character() == character);
    CHECK(f.controller.root() == root);
    f.controller.step(1.0 / 60.0);
    CHECK(f.controller.attached());
    CHECK_FALSE(character->destroyed());
}

TEST_CASE("ignored character subtree is not a collider") {
    Fixture f;
    // The character's own part is in Workspace in a live join; rebuilding
    // with it as ignore_root must leave only the floor and wall.
    std::string err;
    REQUIRE(f.character->reparent(f.world_root.get(), &err));
    f.world.rebuild(f.world_root.get(), false, f.character.get());
    CHECK(f.world.size() == 2);
    rbx::physics::RayHit hit;
    CHECK_FALSE(f.world.raycast({0, 3, 0}, {0, 1, 0}, 10, &hit));
}

TEST_CASE("Automatic authority simulates a granted unanchored part") {
    Fixture f;
    auto ball = make("Part", "Ball");
    setv(ball.get(), "Size", rbx::Variant(rbx::Vector3{2, 2, 2}));
    setv(ball.get(), "CFrame", rbx::Variant(at(0, 10, 0)));
    setv(ball.get(), "Anchored", rbx::Variant(false));
    setv(ball.get(), "NetworkOwnerV3", rbx::Variant(int64_t(2)));
    std::string err;
    REQUIRE(ball->reparent(f.world_root.get(), &err));

    f.world.set_authority(true, 2);
    f.world.sync(f.world_root.get(), f.character.get());
    REQUIRE(f.world.size() == 3);
    rbx::physics::BodyState st;
    REQUIRE(f.world.body_state(ball.get(), &st));
    CHECK(st.mode == rbx::physics::BodyMode::Dynamic);

    for (int i = 0; i < 6; ++i)
        f.world.step(1.0 / 60.0);
    REQUIRE(f.world.body_state(ball.get(), &st));
    CHECK(st.cframe.y < 10.0); // gravity moved the body
    // Workspace.Gravity, not Jolt's 9.81 default: six 60 Hz frames are 24
    // solver steps, so v = -196.2 * 0.1 = -19.62 studs/s.
    CHECK(st.linear_velocity.y == doctest::Approx(-19.62).epsilon(0.05));
    const auto updates = f.world.take_updates();
    REQUIRE(updates.size() == 1);
    CHECK(updates[0].part == ball.get());
    CHECK(updates[0].cframe.y < 10.0);
    CHECK(f.world.stats().dynamic_bodies == 1);
    CHECK(f.world.stats().steps == 24);
    CHECK(f.world.take_updates().empty()); // the queue coalesces per body
}

TEST_CASE("Motor and Motor6D drive a hinged assembly to DesiredAngle") {
    rbx::register_builtin_classes();

    auto run_case = [](const char* joint_class) {
        CAPTURE(joint_class);
        rbx::Ref<rbx::Instance> workspace = make("Model", "Workspace");
        rbx::Ref<rbx::Instance> part0 = make("Part", "Part0");
        rbx::Ref<rbx::Instance> part1 = make("Part", "Part1");
        setv(part0.get(), "Size", rbx::Variant(rbx::Vector3{2, 2, 2}));
        setv(part0.get(), "CFrame", rbx::Variant(at(0, 0, 0)));
        setv(part0.get(), "Anchored", rbx::Variant(true));
        setv(part1.get(), "Size", rbx::Variant(rbx::Vector3{2, 2, 2}));
        setv(part1.get(), "CFrame", rbx::Variant(at(0.5, 0, 0)));
        setv(part1.get(), "Anchored", rbx::Variant(false));

        std::string err;
        REQUIRE(part0->reparent(workspace.get(), &err));
        REQUIRE(part1->reparent(workspace.get(), &err));

        rbx::Ref<rbx::Instance> motor = make(joint_class, "Motor");
        rbx::CFrame c1 = at(-0.5, 0, 0);
        setv(motor.get(), "Part0", rbx::Variant(part0.get()));
        setv(motor.get(), "Part1", rbx::Variant(part1.get()));
        setv(motor.get(), "C1", rbx::Variant(c1));
        setv(motor.get(), "Enabled", rbx::Variant(true));
        setv(motor.get(), "MaxVelocity", rbx::Variant(0.2));
        setv(motor.get(), "DesiredAngle", rbx::Variant(0.8));
        REQUIRE(motor->reparent(workspace.get(), &err));

        rbx::physics::World world;
        world.set_gravity(0.0);
        world.rebuild(workspace.get(), true);
        REQUIRE(world.size() == 2);

        double previous = 0.0;
        for (int frame = 0; frame < 12; ++frame) {
            world.step(1.0 / 60.0);
            rbx::physics::BodyState state;
            REQUIRE(world.body_state(part1.get(), &state));
            const double actual = std::atan2(state.cframe.r21,
                                             state.cframe.r11);
            rbx::Variant angle;
            REQUIRE(motor->get_prop("CurrentAngle", angle));
            const double reported = std::get<double>(angle);
            CHECK(reported == doctest::Approx(actual).epsilon(0.01));
            CHECK(std::abs(actual - previous) <= 0.2 + 0.02);
            CHECK(actual >= -0.01);
            CHECK(actual <= 0.8 + 0.05);
            previous = actual;
        }
        CHECK(previous == doctest::Approx(0.8).epsilon(0.03));

        // Reverse the target: the hinge must drive through zero and settle on
        // the other side, still at no more than MaxVelocity per 60 Hz frame.
        setv(motor.get(), "DesiredAngle", rbx::Variant(-0.4));
        for (int frame = 0; frame < 12; ++frame) {
            world.step(1.0 / 60.0);
            rbx::physics::BodyState state;
            REQUIRE(world.body_state(part1.get(), &state));
            const double actual = std::atan2(state.cframe.r21,
                                             state.cframe.r11);
            rbx::Variant angle;
            REQUIRE(motor->get_prop("CurrentAngle", angle));
            CHECK(std::get<double>(angle) ==
                  doctest::Approx(actual).epsilon(0.01));
            CHECK(std::abs(actual - previous) <= 0.2 + 0.02);
            CHECK(actual >= -0.45);
            CHECK(actual <= 0.85);
            previous = actual;
        }
        CHECK(previous == doctest::Approx(-0.4).epsilon(0.04));

        // The native angle is continuous rather than wrapped to +/-pi.  A
        // target beyond one revolution must settle at the accumulated angle.
        setv(motor.get(), "MaxVelocity", rbx::Variant(0.4));
        setv(motor.get(), "DesiredAngle", rbx::Variant(7.0));
        for (int frame = 0; frame < 30; ++frame)
            world.step(1.0 / 60.0);
        rbx::Variant angle;
        REQUIRE(motor->get_prop("CurrentAngle", angle));
        const double reported = std::get<double>(angle);
        CHECK(reported == doctest::Approx(7.0).epsilon(0.01));
        rbx::physics::BodyState state;
        REQUIRE(world.body_state(part1.get(), &state));
        const double wrapped_target = std::remainder(7.0, 2.0 * M_PI);
        CHECK(std::atan2(state.cframe.r21, state.cframe.r11) ==
              doctest::Approx(wrapped_target).epsilon(0.05));
    };

    run_case("Motor");
    run_case("Motor6D");
}

TEST_CASE("Server authority keeps a granted part kinematic") {
    Fixture f;
    auto ball = make("Part", "Ball");
    setv(ball.get(), "Size", rbx::Variant(rbx::Vector3{2, 2, 2}));
    setv(ball.get(), "CFrame", rbx::Variant(at(0, 10, 0)));
    setv(ball.get(), "Anchored", rbx::Variant(false));
    setv(ball.get(), "NetworkOwnerV3", rbx::Variant(int64_t(2)));
    mark_remote(ball.get());
    std::string err;
    REQUIRE(ball->reparent(f.world_root.get(), &err));

    f.world.set_authority(false, 2); // Crossroads
    f.world.sync(f.world_root.get(), f.character.get());
    rbx::physics::BodyState st;
    REQUIRE(f.world.body_state(ball.get(), &st));
    CHECK(st.mode == rbx::physics::BodyMode::Kinematic);
    for (int i = 0; i < 30; ++i)
        f.world.step(1.0 / 60.0);
    REQUIRE(f.world.body_state(ball.get(), &st));
    CHECK(st.cframe.y == doctest::Approx(10.0));
    CHECK(f.world.take_updates().empty());
}

TEST_CASE("a grant for another client stays kinematic") {
    Fixture f;
    auto ball = make("Part", "Ball");
    setv(ball.get(), "Size", rbx::Variant(rbx::Vector3{2, 2, 2}));
    setv(ball.get(), "CFrame", rbx::Variant(at(0, 10, 0)));
    setv(ball.get(), "Anchored", rbx::Variant(false));
    setv(ball.get(), "NetworkOwnerV3", rbx::Variant(int64_t(605)));
    mark_remote(ball.get());
    std::string err;
    REQUIRE(ball->reparent(f.world_root.get(), &err));

    f.world.set_authority(true, 2);
    f.world.sync(f.world_root.get(), f.character.get());
    rbx::physics::BodyState st;
    REQUIRE(f.world.body_state(ball.get(), &st));
    CHECK(st.mode == rbx::physics::BodyMode::Kinematic);
}

TEST_CASE("no learned local owner never falls back to simulating every grant") {
    Fixture f;
    auto ball = make("Part", "Ball");
    setv(ball.get(), "Size", rbx::Variant(rbx::Vector3{2, 2, 2}));
    setv(ball.get(), "CFrame", rbx::Variant(at(0, 10, 0)));
    setv(ball.get(), "Anchored", rbx::Variant(false));
    setv(ball.get(), "NetworkOwnerV3", rbx::Variant(int64_t(448)));
    mark_remote(ball.get());
    std::string err;
    REQUIRE(ball->reparent(f.world_root.get(), &err));

    // Before the avatar's root grant arrives, the client must not guess that
    // an arbitrary grant is its own (0x1c0/448 here).  Guessing made server
    // bodies fall under the client and fight replication.
    f.world.set_authority(true, 0);
    f.world.sync(f.world_root.get(), f.character.get());
    rbx::physics::BodyState st;
    REQUIRE(f.world.body_state(ball.get(), &st));
    CHECK(st.mode == rbx::physics::BodyMode::Kinematic);
}

TEST_CASE("sync moves a kinematic body without rebuilding the world") {
    Fixture f;
    auto ball = make("Part", "Ball");
    setv(ball.get(), "Size", rbx::Variant(rbx::Vector3{2, 2, 2}));
    setv(ball.get(), "CFrame", rbx::Variant(at(0, 10, 0)));
    setv(ball.get(), "Anchored", rbx::Variant(false));
    setv(ball.get(), "NetworkOwnerV3", rbx::Variant(int64_t(3)));
    mark_remote(ball.get());
    std::string err;
    REQUIRE(ball->reparent(f.world_root.get(), &err));

    f.world.set_authority(true, 2);
    f.world.sync(f.world_root.get(), f.character.get());
    const std::size_t n = f.world.size();
    setv(ball.get(), "CFrame", rbx::Variant(at(4, 6, 2)));
    f.world.sync(f.world_root.get(), f.character.get());
    CHECK(f.world.size() == n);
    rbx::physics::BodyState st;
    REQUIRE(f.world.body_state(ball.get(), &st));
    CHECK(st.mode == rbx::physics::BodyMode::Kinematic);
    CHECK(st.cframe.x == doctest::Approx(4.0));
    CHECK(st.cframe.y == doctest::Approx(6.0));
}

TEST_CASE("a script CFrame write teleports a dynamic body before integration") {
    Fixture f;
    auto ball = make("Part", "TeleportBall");
    setv(ball.get(), "Size", rbx::Variant(rbx::Vector3{2, 2, 2}));
    setv(ball.get(), "CFrame", rbx::Variant(at(0, 10, 0)));
    setv(ball.get(), "Anchored", rbx::Variant(false));
    std::string err;
    REQUIRE(ball->reparent(f.world_root.get(), &err));
    f.world.set_authority(true, 0);
    f.world.sync(f.world_root.get(), f.character.get());
    for (int i = 0; i < 6; ++i)
        f.world.step(1.0 / 60.0);

    setv(ball.get(), "CFrame", rbx::Variant(at(0, 20, 0)));
    f.world.step(1.0 / 60.0);
    rbx::physics::BodyState st;
    REQUIRE(f.world.body_state(ball.get(), &st));
    CHECK(st.cframe.y > 19.5);
}

TEST_CASE("narrow-phase touching and grounding use the real collider") {
    Fixture f;
    auto pad = make("Part", "Pad");
    setv(pad.get(), "Size", rbx::Variant(rbx::Vector3{2, 2, 2}));
    setv(pad.get(), "CFrame", rbx::Variant(at(0, 0.9, 0)));
    setv(pad.get(), "Anchored", rbx::Variant(true));
    std::string err;
    REQUIRE(pad->reparent(f.world_root.get(), &err));
    f.world.rebuild(f.world_root.get(), false, f.character.get());

    std::vector<const rbx::Instance*> hits;
    f.world.touching_parts(pad.get(), &hits);
    CHECK(std::find(hits.begin(), hits.end(), f.floor.get()) != hits.end());
    CHECK(f.world.grounded(pad.get()));
}

TEST_CASE("static contacts answer queries but do not emit touch events") {
    Fixture f;
    auto pad = make("Part", "StaticPad");
    setv(pad.get(), "Size", rbx::Variant(rbx::Vector3{2, 2, 2}));
    setv(pad.get(), "CFrame", rbx::Variant(at(0, 0.9, 0)));
    setv(pad.get(), "Anchored", rbx::Variant(true));
    std::string err;
    REQUIRE(pad->reparent(f.world_root.get(), &err));
    f.world.rebuild(f.world_root.get(), false, f.character.get());

    std::vector<const rbx::Instance*> hits;
    f.world.touching_parts(pad.get(), &hits);
    CHECK(std::find(hits.begin(), hits.end(), f.floor.get()) != hits.end());
    CHECK(f.world.grounded(pad.get()));
    f.world.step(1.0 / 60.0);
    CHECK(f.world.take_contact_begun().empty());
    CHECK(f.world.take_contact_ended().empty());
}

TEST_CASE("a falling body emits a began contact and stays grounded") {
    Fixture f;
    auto ball = make("Part", "ContactBall");
    setv(ball.get(), "Size", rbx::Variant(rbx::Vector3{2, 2, 2}));
    setv(ball.get(), "CFrame", rbx::Variant(at(0, 4, 0)));
    setv(ball.get(), "Anchored", rbx::Variant(false));
    setv(ball.get(), "NetworkOwnerV3", rbx::Variant(int64_t(2)));
    std::string err;
    REQUIRE(ball->reparent(f.world_root.get(), &err));
    f.world.set_authority(true, 2);
    f.world.sync(f.world_root.get(), f.character.get());

    bool touched_floor = false;
    for (int i = 0; i < 120; ++i) {
        f.world.step(1.0 / 60.0);
        for (const rbx::contact::Pair& pair : f.world.take_contact_begun()) {
            const rbx::Instance* a = pair.a.get();
            const rbx::Instance* b = pair.b.get();
            if ((a == ball.get() && b == f.floor.get()) ||
                (b == ball.get() && a == f.floor.get()))
                touched_floor = true;
        }
    }
    CHECK(touched_floor);
    CHECK(f.world.grounded(ball.get()));
}

TEST_CASE("the character capsule reports the floor contact") {
    Fixture f;
    REQUIRE(f.controller.attach(&f.world, f.character.get()));
    for (int i = 0; i < 120; ++i)
        f.controller.step(1.0 / 60.0);
    std::vector<rbx::Instance*> hits;
    f.controller.touching_parts(&hits);
    CHECK(std::find(hits.begin(), hits.end(), f.floor.get()) != hits.end());
}

TEST_CASE("a server-authority fallback root does not claim capsule grounding") {
    Fixture f;
    rbxclient::LocalPlayer local;
    REQUIRE(local.attach(f.world_root.get(), f.character.get()));
    for (int i = 0; i < 120; ++i)
        local.tick(1.0 / 60.0, f.world_root.get(), f.character.get());
    CHECK(local.character().grounded);
    // The native replicated HumanoidRootPart reports IsGrounded()==false in
    // a Server-authority world even while the avatar stands on the floor.
    CHECK_FALSE(local.grounded(f.root_part.get()));
}
