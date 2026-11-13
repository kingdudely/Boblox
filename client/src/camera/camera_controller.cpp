// camera_controller.cpp — see camera_controller.h.
#include "camera/camera_controller.h"

#include <algorithm>
#include <cmath>

namespace rbxclient {

namespace {
constexpr double kPi = 3.14159265358979323846;

rbx::Vector3 cross(const rbx::Vector3& a, const rbx::Vector3& b) {
    return rbx::physics::vcross(a, b);
}
rbx::Vector3 normalized(const rbx::Vector3& v) {
    return rbx::physics::vnormalize(v);
}
} // namespace

CameraController::CameraController() { reset(); }

void CameraController::reset() {
    yaw_ = 0.0;
    pitch_ = 0.0;
    target_distance_ = kDefaultDistance;
    distance_ = kDefaultDistance;
    fov_ = kDefaultFov;
    state_ = CameraState{};
    state_.distance = distance_;
    state_.fov_y = fov_;
}

void CameraController::set_viewport(int width, int height) {
    viewport_width_ = std::max(0, width);
    viewport_height_ = std::max(0, height);
}

void CameraController::add_mouse_delta(double dx, double dy) {
    // The 2016 script divides by the *reference* viewport (1920x1200); using
    // the live viewport would change the feel with window size, which the
    // native does not do.
    (void)viewport_width_;
    (void)viewport_height_;
    yaw_ += dx * kYawPerPixel;
    pitch_ = std::clamp(pitch_ - dy * kPitchPerPixel, -kMaxPitch, kMaxPitch);
    while (yaw_ > kPi)
        yaw_ -= 2.0 * kPi;
    while (yaw_ < -kPi)
        yaw_ += 2.0 * kPi;
}

void CameraController::add_zoom(double notches) {
    target_distance_ =
        std::clamp(target_distance_ + notches, kMinDistance, kMaxDistance);
}

void CameraController::set_yaw(double radians) {
    yaw_ = radians;
    while (yaw_ > kPi)
        yaw_ -= 2.0 * kPi;
    while (yaw_ < -kPi)
        yaw_ += 2.0 * kPi;
}

void CameraController::set_distance(double studs) {
    target_distance_ = std::clamp(studs, kMinDistance, kMaxDistance);
    distance_ = target_distance_;
}

void CameraController::set_fov(double degrees) {
    fov_ = std::clamp(degrees, 1.0, 120.0);
}

void CameraController::update(
    double dt, const rbx::Vector3& head,
    const rbx::physics::World* world,
    const std::vector<const rbx::Instance*>* ignore) {
    if (!(dt > 0.0))
        dt = 0.0;
    // Exponential follow keeps the native's spring feel without an unstable
    // spring integrator.  Zoom collapses faster than it extends.
    const double rate = target_distance_ < distance_ ? 16.0 : 10.0;
    const double blend = 1.0 - std::exp(-dt * rate);
    distance_ += (target_distance_ - distance_) * blend;
    if (std::abs(target_distance_ - distance_) < 1e-4)
        distance_ = target_distance_;

    const rbx::Vector3 focus{head.x, head.y + kHeadOffset, head.z};
    const double cp = std::cos(pitch_);
    const rbx::Vector3 look{std::sin(yaw_) * cp, std::sin(pitch_),
                            std::cos(yaw_) * cp};
    const bool first_person = distance_ < kFirstPersonDistance;

    double distance = distance_;
    if (!first_person && world) {
        rbx::physics::RayHit hit;
        const rbx::Vector3 back = rbx::physics::vmul(look, -1.0);
        if (world->raycast(focus, back, distance, &hit, ignore)) {
            distance = std::max(kMinDistance, hit.distance - 0.3);
        }
    }

    const rbx::Vector3 eye = first_person
                                 ? focus
                                 : rbx::physics::vsub(
                                       focus, rbx::physics::vmul(look, distance));

    // CFrame.lookAt(eye, focus, Y): rows are right, up, -look.
    const rbx::Vector3 up{0.0, 1.0, 0.0};
    rbx::Vector3 right = cross(look, up);
    if (rbx::physics::vlength(right) < 1e-6)
        right = rbx::Vector3{1.0, 0.0, 0.0};
    right = normalized(right);
    const rbx::Vector3 cam_up = cross(right, look);
    const rbx::Vector3 cam_back = rbx::physics::vmul(look, -1.0);

    state_.x = eye.x;
    state_.y = eye.y;
    state_.z = eye.z;
    state_.r00 = right.x;
    state_.r01 = cam_up.x;
    state_.r02 = cam_back.x;
    state_.r10 = right.y;
    state_.r11 = cam_up.y;
    state_.r12 = cam_back.y;
    state_.r20 = right.z;
    state_.r21 = cam_up.z;
    state_.r22 = cam_back.z;
    state_.distance = distance;
    state_.fov_y = fov_;
    state_.first_person = first_person;
}

} // namespace rbxclient
