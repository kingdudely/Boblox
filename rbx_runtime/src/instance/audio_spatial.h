// audio_spatial.h — pure spatial-audio math and listener/world transforms.
//
// Kept independent of SDL/curl so the runtime and unit tests share exactly the
// same attenuation and panning rules the headful mixer uses.
#pragma once

#include "instance/variant.h"

namespace rbx {

class Instance;

namespace audio {

// Values are the native Enum.RollOffMode values (Inverse=0, Linear=1,
// LinearSquare=2, InverseTapered=3).
enum class RollOffMode : int {
    Inverse = 0,
    Linear = 1,
    LinearSquare = 2,
    InverseTapered = 3,
};

struct SpatialSettings {
    int mode = int(RollOffMode::Inverse);
    double min_distance = 10.0;
    double max_distance = 10000.0;
    double emitter_size = 10.0;
};

// EmitterSize is the legacy source of the minimum distance. A non-zero
// EmitterSize wins over RollOffMinDistance, matching the native client (a
// sound with EmitterSize=5 and RollOffMinDistance=1 attenuates from 5).
double effective_min_distance(const SpatialSettings& settings);

// Attenuation in [0, 1], including the native hard cutoff at max_distance.
// Inverse = clamp(min/d); Linear = clamp((max-d)/(max-min)); LinearSquare is
// Linear squared; InverseTapered is min(Inverse, LinearSquare).
double roll_off_gain(const SpatialSettings& settings, double distance);

struct StereoGain {
    double left = 1.0;
    double right = 1.0;
};

// Equal-power stereo gains for a pan in [-1, 1] (+1 is the listener's right).
StereoGain stereo_gains(double attenuation, double pan);

// Pan from the source position in listener space: dot(direction, right axis).
// Returns 0 for a coincident source or a non-finite listener.
double stereo_pan(const CFrame& listener, const Vector3& source);

// Effective world CFrame for a part/camera/attachment/model listener or
// emitter. Returns identity for an instance with no positional transform.
CFrame instance_world_cframe(const Instance* instance);

// The listener configured by SoundService.ListenerType / ListenerCFrame /
// ListenerObject, falling back to Workspace.CurrentCamera. `context` is any
// instance in the same DataModel (usually the Sound or SoundService).
bool listener_world_cframe(const Instance* context, CFrame* out);

// True for a Sound parented to a BasePart or Attachment (the native rule for
// 3D/spatialized playback).
bool is_spatial_parent(const Instance* parent);

} // namespace audio
} // namespace rbx
