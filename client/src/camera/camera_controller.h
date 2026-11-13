// camera_controller.h — the 2016 CameraScript behavior, in C++.
//
// Constants are taken from the actual 2016 CameraScript (rbxmx), not invented:
//   MAX_Y = 80°, MIN_Y = -80°, HEAD_OFFSET = (0, 1.5, 0),
//   MOUSE_SENSITIVITY = (4π/1920, 1.9π/1200) radians per pixel,
//   default distance 12.5, first-person below 2.
//
// The controller is pure: it owns yaw/pitch/distance and asks the physics
// world for one occlusion ray.  It never reads SDL and never touches an
// Instance, which is what lets the unit suite pin the behavior.
#pragma once

#include "instance/variant.h"
#include "physics/physics.h"

#include <vector>

namespace rbx {
class Instance;
}

namespace rbxclient {

struct CameraState {
    double x = 0.0, y = 0.0, z = 0.0;
    double r00 = 1.0, r01 = 0.0, r02 = 0.0;
    double r10 = 0.0, r11 = 1.0, r12 = 0.0;
    double r20 = 0.0, r21 = 0.0, r22 = 1.0;
    double distance = 12.5;
    double fov_y = 70.0;
    bool first_person = false;

    rbx::CFrame cframe() const {
        rbx::CFrame f;
        f.x = x;
        f.y = y;
        f.z = z;
        f.r00 = r00;
        f.r01 = r01;
        f.r02 = r02;
        f.r10 = r10;
        f.r11 = r11;
        f.r12 = r12;
        f.r20 = r20;
        f.r21 = r21;
        f.r22 = r22;
        return f;
    }
};

class CameraController {
  public:
    static constexpr double kDefaultDistance = 12.5;
    static constexpr double kMinDistance = 0.5;
    static constexpr double kMaxDistance = 128.0;
    static constexpr double kFirstPersonDistance = 2.0;
    static constexpr double kMaxPitch = 80.0 * 3.14159265358979323846 / 180.0;
    static constexpr double kHeadOffset = 1.5;
    static constexpr double kYawPerPixel =
        4.0 * 3.14159265358979323846 / 1920.0;
    static constexpr double kPitchPerPixel =
        1.9 * 3.14159265358979323846 / 1200.0;
    static constexpr double kDefaultFov = 70.0;

    CameraController();

    void reset();
    void set_viewport(int width, int height);

    // Raw pointer delta in pixels; positive dx is mouse-right.
    void add_mouse_delta(double dx, double dy);
    // One wheel notch; positive scrolls out (away from the avatar).
    void add_zoom(double notches);
    void set_yaw(double radians);
    void set_distance(double studs);
    void set_fov(double degrees);

    // Step the camera from a character pose. `head` is the HumanoidRootPart
    // position; the head offset is added here. `ignore` parts are skipped by
    // the occlusion ray (the local avatar's own replicated parts).
    void update(double dt, const rbx::Vector3& head,
                const rbx::physics::World* world,
                const std::vector<const rbx::Instance*>* ignore = nullptr);

    const CameraState& state() const { return state_; }
    double yaw() const { return yaw_; }
    double pitch() const { return pitch_; }
    double target_distance() const { return target_distance_; }

  private:
    double yaw_ = 0.0;
    double pitch_ = -0.15;
    double target_distance_ = kDefaultDistance;
    double distance_ = kDefaultDistance;
    double fov_ = kDefaultFov;
    int viewport_width_ = 0;
    int viewport_height_ = 0;
    CameraState state_;
};

} // namespace rbxclient
