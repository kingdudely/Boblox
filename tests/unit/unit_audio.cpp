// unit_audio.cpp — pure spatial-audio rules and the native built-in table.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "audio/audio_assets.h"
#include "instance/audio_spatial.h"

#include <doctest/doctest.h>

#include <cmath>

using namespace rbx;
using namespace rbxclient::audio;
using namespace rbx::audio;

namespace {
bool close(double a, double b) { return std::abs(a - b) < 1e-9; }
} // namespace

TEST_CASE("native rolloff modes and effective minimum distance") {
    SpatialSettings settings;
    settings.min_distance = 1.0;
    settings.max_distance = 50.0;
    settings.emitter_size = 1.0;

    settings.mode = int(RollOffMode::Inverse);
    CHECK(close(roll_off_gain(settings, 1.0), 1.0));
    CHECK(close(roll_off_gain(settings, 2.0), 0.5));
    CHECK(close(roll_off_gain(settings, 10.0), 0.1));
    CHECK(close(roll_off_gain(settings, 20.0), 0.05));
    CHECK(close(roll_off_gain(settings, 49.999), 1.0 / 49.999));
    CHECK(close(roll_off_gain(settings, 50.0), 0.0));
    CHECK(close(roll_off_gain(settings, 100.0), 0.0));

    settings.mode = int(RollOffMode::Linear);
    CHECK(close(roll_off_gain(settings, 2.0), 48.0 / 49.0));
    CHECK(close(roll_off_gain(settings, 10.0), 40.0 / 49.0));
    CHECK(close(roll_off_gain(settings, 20.0), 30.0 / 49.0));
    CHECK(close(roll_off_gain(settings, 50.0), 0.0));

    settings.mode = int(RollOffMode::LinearSquare);
    CHECK(close(roll_off_gain(settings, 10.0), (40.0 / 49.0) * (40.0 / 49.0)));

    settings.mode = int(RollOffMode::InverseTapered);
    CHECK(close(roll_off_gain(settings, 2.0),
                std::min(0.5, (48.0 / 49.0) * (48.0 / 49.0))));
    CHECK(close(roll_off_gain(settings, 20.0),
                std::min(0.05, (30.0 / 49.0) * (30.0 / 49.0))));

    // A non-zero EmitterSize is the native minimum, even when
    // RollOffMinDistance says otherwise.
    settings.min_distance = 1.0;
    settings.emitter_size = 5.0;
    settings.mode = int(RollOffMode::Inverse);
    CHECK(close(effective_min_distance(settings), 5.0));
    CHECK(close(roll_off_gain(settings, 2.0), 1.0));
    CHECK(close(roll_off_gain(settings, 10.0), 0.5));
    CHECK(close(roll_off_gain(settings, 20.0), 0.25));
}

TEST_CASE("listener-relative panning uses equal-power stereo gains") {
    const CFrame listener{};
    CHECK(close(stereo_pan(listener, Vector3{10.0, 0.0, 0.0}), 1.0));
    CHECK(close(stereo_pan(listener, Vector3{-10.0, 0.0, 0.0}), -1.0));
    CHECK(close(stereo_pan(listener, Vector3{0.0, 10.0, 0.0}), 0.0));
    CHECK(close(stereo_pan(listener, Vector3{0.0, 0.0, 0.0}), 0.0));

    const StereoGain center = stereo_gains(1.0, 0.0);
    CHECK(close(center.left, std::sqrt(0.5)));
    CHECK(close(center.right, std::sqrt(0.5)));

    const StereoGain right = stereo_gains(0.5, 1.0);
    CHECK(close(right.left, 0.0));
    CHECK(close(right.right, 0.5));
    const StereoGain left = stereo_gains(0.5, -1.0);
    CHECK(close(left.left, 0.5));
    CHECK(close(left.right, 0.0));
}

TEST_CASE("native built-in redirect table is complete and exact") {
    // 182 redirect-table entries plus the ten bundled local-only names the
    // native client resolves from its content pack.
    CHECK(builtin_sound_count() == 192);

    const BuiltinSound ping =
        find_builtin_sound("rbxasset://sounds/electronicpingshort.wav");
    CHECK(ping.id == 911778514);
    CHECK(ping.alias.empty());

    const BuiltinSound glass =
        find_builtin_sound("rbxasset://sounds/GlassBreak.WAV?token=redacted");
    CHECK(glass.id == 12222005);

    const BuiltinSound bass = find_builtin_sound("bass.mp3");
    CHECK(bass.id == 911769342);

    const BuiltinSound falling =
        find_builtin_sound("rbxasset://sounds/action_falling.mp3");
    CHECK(falling.id == -1);
    CHECK(falling.alias == "action_falling.ogg");

    const BuiltinSound ouch = find_builtin_sound("ouch.ogg");
    CHECK(ouch.id == 12222242);
    CHECK(ouch.alias == "oof.ogg");

    const BuiltinSound jump = find_builtin_sound("action_jump.mp3");
    CHECK(jump.id == -1);
    CHECK(jump.alias.empty());

    const BuiltinSound missing = find_builtin_sound("not-a-sound.wav");
    CHECK(missing.id == -1);
    CHECK(missing.alias.empty());
}
