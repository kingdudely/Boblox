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

    auto rotation_mul = [](const rbx::CFrame& a, const rbx::CFrame& b) {
        rbx::CFrame out;
        out.r00 = a.r00 * b.r00 + a.r01 * b.r10 + a.r02 * b.r20;
        out.r01 = a.r00 * b.r01 + a.r01 * b.r11 + a.r02 * b.r21;
        out.r02 = a.r00 * b.r02 + a.r01 * b.r12 + a.r02 * b.r22;
        out.r10 = a.r10 * b.r00 + a.r11 * b.r10 + a.r12 * b.r20;
        out.r11 = a.r10 * b.r01 + a.r11 * b.r11 + a.r12 * b.r21;
        out.r12 = a.r10 * b.r02 + a.r11 * b.r12 + a.r12 * b.r22;
        out.r20 = a.r20 * b.r00 + a.r21 * b.r10 + a.r22 * b.r20;
        out.r21 = a.r20 * b.r01 + a.r21 * b.r11 + a.r22 * b.r21;
        out.r22 = a.r20 * b.r02 + a.r21 * b.r12 + a.r22 * b.r22;
        return out;
    };

    auto relative_angle = [](const rbx::CFrame& base,
                             const rbx::CFrame& moving) {
        // R_base^T * R_moving; only the rotation matters for a hinge angle.
        rbx::CFrame rel;
        rel.r00 = base.r00 * moving.r00 + base.r10 * moving.r10 +
                  base.r20 * moving.r20;
        rel.r01 = base.r00 * moving.r01 + base.r10 * moving.r11 +
                  base.r20 * moving.r21;
        rel.r02 = base.r00 * moving.r02 + base.r10 * moving.r12 +
                  base.r20 * moving.r22;
        rel.r10 = base.r01 * moving.r00 + base.r11 * moving.r10 +
                  base.r21 * moving.r20;
        rel.r11 = base.r01 * moving.r01 + base.r11 * moving.r11 +
                  base.r21 * moving.r21;
        rel.r12 = base.r01 * moving.r02 + base.r11 * moving.r12 +
                  base.r21 * moving.r22;
        rel.r20 = base.r02 * moving.r00 + base.r12 * moving.r10 +
                  base.r22 * moving.r20;
        rel.r21 = base.r02 * moving.r01 + base.r12 * moving.r11 +
                  base.r22 * moving.r21;
        rel.r22 = base.r02 * moving.r02 + base.r12 * moving.r12 +
                  base.r22 * moving.r22;
        // Native MotorJoint::getMeInOther rotates jointCoord0 about its
        // local Z axis.  For Part1 relative to Part0, a positive angle is
        // Rz(theta), whose sine is r10 and cosine is r00.
        return std::atan2(rel.r10, rel.r00);
    };

    auto joint_angle = [&](const rbx::CFrame& part0_frame,
                           const rbx::CFrame& c0,
                           const rbx::CFrame& part1_frame,
                           const rbx::CFrame& c1) {
        // Native MotorJoint compares C0's rotated joint frame with C1's
        // joint frame.  Measure Part1 relative to Part0 in that frame, not
        // in Part0's object frame, so a rotated C0 is still native local Z.
        return relative_angle(rotation_mul(part0_frame, c0),
                              rotation_mul(part1_frame, c1));
    };

    struct MotorCase {
        const char* joint_class;
        bool part0_anchored;
        bool part1_anchored;
        bool part0_collide;
        bool part1_collide;
        bool rotated_c0 = false;
    };

    int cases_run = 0;
    auto run_case = [&](const MotorCase& c) {
        ++cases_run;
        CAPTURE(c.joint_class);
        CAPTURE(c.part0_anchored);
        CAPTURE(c.part1_anchored);
        CAPTURE(c.part0_collide);
        CAPTURE(c.part1_collide);
        CAPTURE(c.rotated_c0);

        const rbx::CFrame part1_start =
            c.rotated_c0 ? at(0, 0.5, 0) : at(0.5, 0, 0);
        rbx::CFrame c0;
        if (c.rotated_c0) {
            // A +90 degree C0 yaw makes the joint's local Z point along
            // world +X, so a world-X hinge cannot pass this case.
            const double half_pi = M_PI / 2.0;
            c0.r00 = std::cos(half_pi);
            c0.r02 = std::sin(half_pi);
            c0.r20 = -std::sin(half_pi);
            c0.r22 = std::cos(half_pi);
        }
        rbx::CFrame c1 = c.rotated_c0 ? c0 : at(-0.5, 0, 0);
        if (c.rotated_c0)
            c1.y = -0.5;

        // An anchored endpoint must be the fixed base, not merely a dynamic
        // body that happens to rotate with the hinge.
        auto check_anchors = [&](const rbx::CFrame& frame0,
                                 const rbx::CFrame& frame1) {
            if (c.part0_anchored) {
                CHECK(frame0.x == doctest::Approx(0.0));
                CHECK(frame0.y == doctest::Approx(0.0));
                CHECK(frame0.z == doctest::Approx(0.0));
                CHECK(frame0.r00 == doctest::Approx(1.0));
                CHECK(frame0.r10 == doctest::Approx(0.0));
            }
            if (c.part1_anchored) {
                CHECK(frame1.x == doctest::Approx(part1_start.x));
                CHECK(frame1.y == doctest::Approx(part1_start.y));
                CHECK(frame1.z == doctest::Approx(part1_start.z));
                CHECK(frame1.r00 == doctest::Approx(1.0));
                CHECK(frame1.r10 == doctest::Approx(0.0));
            }
        };

        rbx::Ref<rbx::Instance> workspace = make("Model", "Workspace");
        rbx::Ref<rbx::Instance> part0 = make("Part", "Part0");
        rbx::Ref<rbx::Instance> part1 = make("Part", "Part1");
        setv(part0.get(), "Size", rbx::Variant(rbx::Vector3{2, 2, 2}));
        setv(part0.get(), "CFrame", rbx::Variant(at(0, 0, 0)));
        setv(part0.get(), "Anchored", rbx::Variant(c.part0_anchored));
        setv(part0.get(), "CanCollide", rbx::Variant(c.part0_collide));
        setv(part1.get(), "Size", rbx::Variant(rbx::Vector3{2, 2, 2}));
        setv(part1.get(), "CFrame", rbx::Variant(part1_start));
        setv(part1.get(), "Anchored", rbx::Variant(c.part1_anchored));
        setv(part1.get(), "CanCollide", rbx::Variant(c.part1_collide));

        std::string err;
        REQUIRE(part0->reparent(workspace.get(), &err));
        REQUIRE(part1->reparent(workspace.get(), &err));

        rbx::Ref<rbx::Instance> motor = make(c.joint_class, "Motor");
        setv(motor.get(), "Part0", rbx::Variant(part0.get()));
        setv(motor.get(), "Part1", rbx::Variant(part1.get()));
        setv(motor.get(), "C0", rbx::Variant(c0));
        setv(motor.get(), "C1", rbx::Variant(c1));
        setv(motor.get(), "Enabled", rbx::Variant(true));
        setv(motor.get(), "MaxVelocity", rbx::Variant(0.2));
        setv(motor.get(), "DesiredAngle", rbx::Variant(0.8));
        REQUIRE(motor->reparent(workspace.get(), &err));

        rbx::physics::World world;
        world.set_gravity(0.0);
        world.rebuild(workspace.get(), true);
        REQUIRE(world.size() == 2);

        const double two_pi = 2.0 * M_PI;
        auto read_angle = [&]() {
            rbx::physics::BodyState state0;
            rbx::physics::BodyState state1;
            REQUIRE(world.body_state(part0.get(), &state0));
            REQUIRE(world.body_state(part1.get(), &state1));
            check_anchors(state0.cframe, state1.cframe);
            return joint_angle(state0.cframe, c0, state1.cframe, c1);
        };
        double previous = read_angle();

        auto run_phase = [&](int frames, double max_velocity, double desired) {
            setv(motor.get(), "MaxVelocity", rbx::Variant(max_velocity));
            setv(motor.get(), "DesiredAngle", rbx::Variant(desired));
            for (int frame = 0; frame < frames; ++frame) {
                world.step(1.0 / 60.0);
                rbx::physics::BodyState state0;
                rbx::physics::BodyState state1;
                REQUIRE(world.body_state(part0.get(), &state0));
                REQUIRE(world.body_state(part1.get(), &state1));
                const double actual =
                    joint_angle(state0.cframe, c0, state1.cframe, c1);
                rbx::Variant angle;
                REQUIRE(motor->get_prop("CurrentAngle", angle));
                CHECK(std::get<double>(angle) ==
                      doctest::Approx(actual).epsilon(0.01));
                check_anchors(state0.cframe, state1.cframe);
                CHECK(std::abs(std::remainder(actual - previous, two_pi)) <=
                      std::abs(max_velocity) + 0.02);
                previous = actual;
            }
        };

        // Positive then negative DesiredAngle, each at the positive rate.
        run_phase(12, 0.2, 0.8);
        CHECK(previous == doctest::Approx(0.8).epsilon(0.03));
        run_phase(12, 0.2, -0.4);
        CHECK(previous == doctest::Approx(-0.4).epsilon(0.04));

        // Native treats MaxVelocity as a magnitude: a negative value must
        // still drive toward the target, in either direction.
        run_phase(12, -0.2, 0.8);
        CHECK(previous == doctest::Approx(0.8).epsilon(0.03));
        run_phase(12, -0.2, -0.4);
        CHECK(previous == doctest::Approx(-0.4).epsilon(0.04));

        // The native angle is continuous rather than wrapped to +/-pi.  A
        // target beyond one revolution must settle at the accumulated angle.
        setv(motor.get(), "MaxVelocity", rbx::Variant(0.4));
        setv(motor.get(), "DesiredAngle", rbx::Variant(7.0));
        for (int frame = 0; frame < 30; ++frame) {
            world.step(1.0 / 60.0);
            const double actual = read_angle();
            CHECK(std::abs(std::remainder(actual - previous, two_pi)) <=
                  0.4 + 0.02);
            previous = actual;
        }
        rbx::Variant angle;
        REQUIRE(motor->get_prop("CurrentAngle", angle));
        const double reported = std::get<double>(angle);
        CHECK(reported == doctest::Approx(7.0).epsilon(0.01));
        CHECK(previous == doctest::Approx(std::remainder(7.0, two_pi)).epsilon(0.05));
    };

    const MotorCase configs[] = {
        {nullptr, true, false, true, true},
        {nullptr, false, true, true, true},
        {nullptr, false, false, true, true},
        {nullptr, true, false, true, false},
        {nullptr, true, false, false, true},
        {nullptr, true, false, false, false},
        {nullptr, false, true, true, false},
        {nullptr, false, true, false, true},
        {nullptr, false, true, false, false},
        {nullptr, false, false, true, false},
        {nullptr, false, false, false, true},
        {nullptr, false, false, false, false},
    };
    for (const char* joint_class : {"Motor", "Motor6D"}) {
        for (const MotorCase& base : configs) {
            for (bool rotated_c0 : {false, true}) {
                MotorCase c = base;
                c.joint_class = joint_class;
                c.rotated_c0 = rotated_c0;
                run_case(c);
            }
        }
    }
    CHECK(cases_run == 48);
}

TEST_CASE("Motor rotates about the C0 joint Z axis") {
    rbx::register_builtin_classes();
    rbx::Ref<rbx::Instance> workspace = make("Model", "Workspace");
    rbx::Ref<rbx::Instance> part0 = make("Part", "Part0");
    rbx::Ref<rbx::Instance> part1 = make("Part", "Part1");
    setv(part0.get(), "Size", rbx::Variant(rbx::Vector3{2, 2, 2}));
    setv(part0.get(), "CFrame", rbx::Variant(at(0, 0, 0)));
    setv(part0.get(), "Anchored", rbx::Variant(true));
    setv(part1.get(), "Size", rbx::Variant(rbx::Vector3{2, 2, 2}));
    setv(part1.get(), "CFrame", rbx::Variant(at(0, 0.5, 0)));

    std::string err;
    REQUIRE(part0->reparent(workspace.get(), &err));
    REQUIRE(part1->reparent(workspace.get(), &err));

    // A +90 degree C0 yaw makes the joint's local Z point along world +X.
    const double half_pi = M_PI / 2.0;
    rbx::CFrame c0 = at(0, 0, 0);
    c0.r00 = std::cos(half_pi);
    c0.r02 = std::sin(half_pi);
    c0.r20 = -std::sin(half_pi);
    c0.r22 = std::cos(half_pi);
    // C1 places Part1's joint frame at the same world frame as Part0's C0.
    rbx::CFrame c1 = c0;
    c1.y = -0.5;

    rbx::Ref<rbx::Instance> motor = make("Motor", "Motor");
    setv(motor.get(), "Part0", rbx::Variant(part0.get()));
    setv(motor.get(), "Part1", rbx::Variant(part1.get()));
    setv(motor.get(), "C0", rbx::Variant(c0));
    setv(motor.get(), "C1", rbx::Variant(c1));
    // Native uses fabs(MaxVelocity); negative is the same magnitude, not a
    // reverse direction request.
    setv(motor.get(), "MaxVelocity", rbx::Variant(-0.2));
    setv(motor.get(), "DesiredAngle", rbx::Variant(0.8));
    REQUIRE(motor->reparent(workspace.get(), &err));

    rbx::physics::World world;
    world.set_gravity(0.0);
    world.rebuild(workspace.get(), true);
    for (int frame = 0; frame < 20; ++frame)
        world.step(1.0 / 60.0);

    rbx::Variant angle;
    REQUIRE(motor->get_prop("CurrentAngle", angle));
    CHECK(std::get<double>(angle) == doctest::Approx(0.8).epsilon(0.01));
    rbx::physics::BodyState state0;
    rbx::physics::BodyState state1;
    REQUIRE(world.body_state(part0.get(), &state0));
    REQUIRE(world.body_state(part1.get(), &state1));
    CHECK(state0.cframe.x == doctest::Approx(0.0));
    CHECK(state0.cframe.r00 == doctest::Approx(1.0));
    CHECK(state0.cframe.r10 == doctest::Approx(0.0));
    // Positive rotation about the joint-local +Z is world +X here: Part1's
    // center swings from +Y toward +Z, not within the XY plane.
    CHECK(state1.cframe.z > 0.25);
    CHECK(state1.cframe.y < 0.4);
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

TEST_CASE("CanCollide=false bodies simulate but never generate contacts") {
    Fixture f;
    auto ghost = make("Part", "Ghost");
    setv(ghost.get(), "Size", rbx::Variant(rbx::Vector3{2, 2, 2}));
    setv(ghost.get(), "CFrame", rbx::Variant(at(0, 4, 0)));
    setv(ghost.get(), "Anchored", rbx::Variant(false));
    setv(ghost.get(), "CanCollide", rbx::Variant(false));
    setv(ghost.get(), "NetworkOwnerV3", rbx::Variant(int64_t(2)));
    std::string err;
    REQUIRE(ghost->reparent(f.world_root.get(), &err));
    f.world.set_authority(true, 2);
    f.world.sync(f.world_root.get(), f.character.get());

    rbx::physics::BodyState st;
    REQUIRE(f.world.body_state(ghost.get(), &st));
    CHECK(st.mode == rbx::physics::BodyMode::Dynamic);

    bool touched_floor = false;
    for (int i = 0; i < 120; ++i) {
        f.world.step(1.0 / 60.0);
        for (const rbx::contact::Pair& pair : f.world.take_contact_begun()) {
            const rbx::Instance* a = pair.a.get();
            const rbx::Instance* b = pair.b.get();
            if ((a == ghost.get() && b == f.floor.get()) ||
                (b == ghost.get() && a == f.floor.get()))
                touched_floor = true;
        }
    }
    REQUIRE(f.world.body_state(ghost.get(), &st));
    CHECK(st.cframe.y < -1.0); // gravity moved it through the floor
    CHECK_FALSE(touched_floor);
    std::vector<const rbx::Instance*> hits;
    f.world.touching_parts(ghost.get(), &hits);
    CHECK(hits.empty());
    CHECK_FALSE(f.world.grounded(ghost.get()));
}

TEST_CASE("the character does not stand on a CanCollide=false floor") {
    Fixture f;
    setv(f.floor.get(), "CanCollide", rbx::Variant(false));
    f.world.rebuild(f.world_root.get(), false, f.character.get());
    REQUIRE(f.controller.attach(&f.world, f.character.get()));
    for (int i = 0; i < 120; ++i)
        f.controller.step(1.0 / 60.0);
    CHECK_FALSE(f.controller.state().grounded);
    CHECK(f.controller.state().position.y < 0.0);
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
