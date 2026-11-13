// unit_camera.cpp — 2016 CameraScript constants and the occlusion ray.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "camera/camera_controller.h"
#include "instance/class_registry.h"
#include "instance/instance.h"
#include "physics/physics.h"

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

} // namespace

TEST_CASE("default third-person distance matches the 2016 CameraScript") {
    rbxclient::CameraController cam;
    cam.update(1.0 / 60.0, {0, 3, 0}, nullptr);
    CHECK(cam.target_distance() == doctest::Approx(12.5));
    CHECK(cam.state().distance == doctest::Approx(12.5).epsilon(0.01));
    CHECK_FALSE(cam.state().first_person);
    // yaw 0 looks along +Z, so the camera sits behind at -Z, above the head.
    CHECK(cam.state().z == doctest::Approx(-12.5).epsilon(0.01));
    CHECK(cam.state().y == doctest::Approx(4.5).epsilon(0.01));
}

TEST_CASE("mouse look uses the 2016 per-pixel sensitivity and clamps pitch") {
    rbxclient::CameraController cam;
    cam.add_mouse_delta(100, 0);
    CHECK(cam.yaw() == doctest::Approx(100.0 * 4.0 * 3.141592653589793 /
                                       1920.0));
    cam.add_mouse_delta(0, 100000);
    CHECK(cam.pitch() == doctest::Approx(-80.0 * 3.141592653589793 / 180.0));
    cam.add_mouse_delta(0, -200000);
    CHECK(cam.pitch() == doctest::Approx(80.0 * 3.141592653589793 / 180.0));
}

TEST_CASE("zoom below two studs becomes first person at the head") {
    rbxclient::CameraController cam;
    cam.set_distance(1.0);
    cam.update(1.0 / 60.0, {0, 3, 0}, nullptr);
    CHECK(cam.state().first_person);
    CHECK(cam.state().x == doctest::Approx(0.0));
    CHECK(cam.state().y == doctest::Approx(4.5));
    CHECK(cam.state().z == doctest::Approx(0.0));
}

TEST_CASE("occlusion shortens the camera before a wall") {
    rbx::register_builtin_classes();
    auto root = make("Model", "World");
    auto wall = make("Part", "Wall");
    setv(wall.get(), "Size", rbx::Variant(rbx::Vector3{20, 20, 1}));
    rbx::CFrame f;
    f.z = -5;
    setv(wall.get(), "CFrame", rbx::Variant(f));
    setv(wall.get(), "Anchored", rbx::Variant(true));
    std::string err;
    REQUIRE(wall->reparent(root.get(), &err));

    rbx::physics::World world;
    world.rebuild(root.get());

    rbxclient::CameraController cam;
    cam.set_distance(12.5);
    cam.update(1.0 / 60.0, {0, 3, 0}, &world);
    CHECK(cam.state().distance < 5.0);
    CHECK(cam.state().distance > 4.0);
    CHECK_FALSE(cam.state().first_person);
}
