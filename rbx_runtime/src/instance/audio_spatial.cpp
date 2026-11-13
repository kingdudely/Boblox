// audio_spatial.cpp — see audio_spatial.h.
#include "instance/audio_spatial.h"

#include "instance/instance.h"

#include <algorithm>
#include <cmath>

namespace rbx {
namespace audio {
namespace {

constexpr double kEpsilon = 1e-9;

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

bool get_double(const Instance* instance, const char* name, double* out) {
    Variant value;
    if (!instance || !instance->get_prop(name, value))
        return false;
    if (const auto* number = std::get_if<double>(&value)) {
        *out = *number;
        return true;
    }
    return false;
}

bool get_vector3(const Instance* instance, const char* name, Vector3* out) {
    Variant value;
    if (!instance || !instance->get_prop(name, value))
        return false;
    if (const auto* vector = std::get_if<Vector3>(&value)) {
        *out = *vector;
        return true;
    }
    return false;
}

Instance* find_service(const Instance* context, const char* class_name) {
    if (!context)
        return nullptr;
    const Instance* root = context;
    while (root->parent())
        root = root->parent();
    if (root->is_a(class_name))
        return const_cast<Instance*>(root);
    return root->find_first_child_of_class(class_name);
}

Vector3 cframe_point(const CFrame& cframe, const Vector3& local) {
    return Vector3{cframe.r00 * local.x + cframe.r01 * local.y +
                       cframe.r02 * local.z + cframe.x,
                   cframe.r10 * local.x + cframe.r11 * local.y +
                       cframe.r12 * local.z + cframe.y,
                   cframe.r20 * local.x + cframe.r21 * local.y +
                       cframe.r22 * local.z + cframe.z};
}

} // namespace

double effective_min_distance(const SpatialSettings& settings) {
    return settings.emitter_size > 0.0 ? settings.emitter_size
                                       : std::max(0.0, settings.min_distance);
}

double roll_off_gain(const SpatialSettings& settings, double distance) {
    const double min_distance = effective_min_distance(settings);
    const double max_distance = std::max(min_distance, settings.max_distance);
    if (!std::isfinite(distance) || distance >= max_distance)
        return 0.0;
    if (distance <= min_distance)
        return 1.0;
    const auto mode = static_cast<RollOffMode>(settings.mode);
    const double inverse =
        min_distance > 0.0 ? clamp01(min_distance / distance) : 0.0;
    const double linear =
        max_distance > min_distance
            ? clamp01((max_distance - distance) /
                      (max_distance - min_distance))
            : 0.0;
    switch (mode) {
    case RollOffMode::Linear:
        return linear;
    case RollOffMode::LinearSquare:
        return linear * linear;
    case RollOffMode::InverseTapered:
        return std::min(inverse, linear * linear);
    case RollOffMode::Inverse:
    default:
        return inverse;
    }
}

StereoGain stereo_gains(double attenuation, double pan) {
    const double clamped_pan = std::clamp(pan, -1.0, 1.0);
    const double gain = std::max(0.0, attenuation);
    // Equal-power law: the sum of the squared channel gains is constant, so a
    // source crossing the listener does not dip in perceived loudness.
    return StereoGain{gain * std::sqrt((1.0 - clamped_pan) * 0.5),
                      gain * std::sqrt((1.0 + clamped_pan) * 0.5)};
}

double stereo_pan(const CFrame& listener, const Vector3& source) {
    const double x = source.x - listener.x;
    const double y = source.y - listener.y;
    const double z = source.z - listener.z;
    const double length = std::sqrt(x * x + y * y + z * z);
    if (!std::isfinite(length) || length < kEpsilon)
        return 0.0;
    // CFrame.RightVector is the first row of the rotation matrix in this
    // runtime (see cframe.cpp), so the projection is a dot product with it.
    const double pan = (x * listener.r00 + y * listener.r01 +
                        z * listener.r02) /
                       length;
    return std::isfinite(pan) ? std::clamp(pan, -1.0, 1.0) : 0.0;
}

CFrame instance_world_cframe(const Instance* instance) {
    if (!instance)
        return CFrame{};

    // Attachments carry a local Position relative to their parent part. The
    // native listener/emitter position is the attachment's world position.
    if (instance->is_a("Attachment")) {
        CFrame base = instance_world_cframe(instance->parent());
        Vector3 local{};
        get_vector3(instance, "Position", &local);
        CFrame world = base;
        const Vector3 point = cframe_point(base, local);
        world.x = point.x;
        world.y = point.y;
        world.z = point.z;
        return world;
    }

    // A Humanoid listener/emitter resolves through its character's root part.
    if (instance->is_a("Humanoid")) {
        if (Instance* root = instance->parent()
                                 ? instance->parent()->find_first_child_of_class(
                                       "HumanoidRootPart")
                                 : nullptr)
            return instance_world_cframe(root);
    }

    // Models use their PrimaryPart when configured; otherwise fall back to a
    // HumanoidRootPart so a Player.Character listener behaves natively.
    if (instance->is_a("Model")) {
        Variant primary;
        if (instance->get_prop("PrimaryPart", primary)) {
            if (auto* part = std::get_if<Instance*>(&primary); part && *part)
                return instance_world_cframe(*part);
        }
        if (Instance* root = instance->find_first_child_of_class(
                "HumanoidRootPart"))
            return instance_world_cframe(root);
    }

    Variant value;
    if (instance->get_prop("CFrame", value)) {
        if (const auto* cframe = std::get_if<CFrame>(&value))
            return *cframe;
    }
    // WorldCFrame is the authoritative transform for a few newer instances
    // (attachments already returned above); accept it before falling back.
    if (instance->get_prop("WorldCFrame", value)) {
        if (const auto* cframe = std::get_if<CFrame>(&value))
            return *cframe;
    }
    return CFrame{};
}

bool listener_world_cframe(const Instance* context, CFrame* out) {
    if (!out)
        return false;
    Instance* service = find_service(context, "SoundService");
    Instance* workspace = find_service(context, "Workspace");
    if (!workspace && context && context->is_a("Workspace"))
        workspace = const_cast<Instance*>(context);

    int listener_type = 0; // Enum.ListenerType.Camera
    if (service) {
        Variant value;
        if (service->get_prop("ListenerType", value)) {
            if (const auto* item = std::get_if<EnumItem>(&value))
                listener_type = item->value;
        }
    }

    if (service) {
        if (listener_type == 1) { // CFrame
            Variant value;
            if (service->get_prop("ListenerCFrame", value)) {
                if (const auto* cframe = std::get_if<CFrame>(&value)) {
                    *out = *cframe;
                    return true;
                }
            }
        } else if (listener_type == 2 || listener_type == 3) {
            Variant value;
            if (service->get_prop("ListenerObject", value)) {
                if (const auto* object = std::get_if<Instance*>(&value);
                    object && *object) {
                    *out = instance_world_cframe(*object);
                    if (listener_type == 2) {
                        // ObjectPosition uses only the object's position; its
                        // rotation must not rotate the panning axis.
                        out->r00 = 1.0;
                        out->r01 = 0.0;
                        out->r02 = 0.0;
                        out->r10 = 0.0;
                        out->r11 = 1.0;
                        out->r12 = 0.0;
                        out->r20 = 0.0;
                        out->r21 = 0.0;
                        out->r22 = 1.0;
                    }
                    return true;
                }
            }
        }
    }

    if (workspace) {
        Variant value;
        if (workspace->get_prop("CurrentCamera", value)) {
            if (const auto* camera = std::get_if<Instance*>(&value);
                camera && *camera) {
                *out = instance_world_cframe(*camera);
                return true;
            }
        }
    }
    *out = CFrame{};
    return false;
}

bool is_spatial_parent(const Instance* parent) {
    return parent && (parent->is_a("BasePart") || parent->is_a("Attachment"));
}

} // namespace audio
} // namespace rbx
