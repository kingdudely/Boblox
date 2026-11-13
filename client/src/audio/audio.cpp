// audio.cpp — SDL3 playback for the runtime's Sound API.
//
// Resource URIs resolve through the local `rbxasset://` tree first, then
// Roblox assetdelivery with the session cookie (the cookie is never printed).
// WAV, OGG Vorbis and MP3 are decoded locally into F32 PCM and mixed by one
// device stream. The mixer applies Roblox's rolloff modes, Sound/SoundGroup
// volume, playback speed and equal-power panning against the configured
// SoundService listener. The pure listener/attenuation rules live in
// instance/audio_spatial.{h,cpp} so they are testable without a device.
#include "audio/audio.h"

#include "audio/audio_assets.h"
#include "common/asset_path.h"
#include "common/cookie.h"
#include "instance/audio_host.h"
#include "instance/audio_spatial.h"
#include "instance/class_registry.h"
#include "instance/instance.h"
#include "instance/variant.h"

#include <SDL3/SDL.h>

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rbxclient {
namespace audio {
namespace {

constexpr size_t kMaxAssetBytes = 32u * 1024u * 1024u;
constexpr int kMixRate = 48000;

// stb_vorbis is built as C; minimp3 is header-only and implemented below.
extern "C" int stb_vorbis_decode_memory(const unsigned char* mem, int len,
                                        int* channels, int* sample_rate,
                                        short** output);

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3_ex.h"

struct Decoded {
    int channels = 0;
    int rate = 0;
    std::vector<float> pcm;

    size_t frames() const {
        return channels > 0 ? pcm.size() / size_t(channels) : 0;
    }
    double duration() const {
        return rate > 0 ? double(frames()) / double(rate) : 0.0;
    }
};

struct Voice {
    std::shared_ptr<Decoded> decoded;
    // All values read by the audio callback are atomics: the main thread
    // updates them from Sound properties while the callback mixes.
    std::atomic<double> cursor{0.0};
    std::atomic<double> left_gain{1.0};
    std::atomic<double> right_gain{1.0};
    std::atomic<double> speed{1.0};
    std::atomic<double> seek{-1.0};
    std::atomic<bool> looped{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> finished{false};
    bool spatial = false;
    double last_rolloff = 1.0;
    double last_time = 0.0;
};

struct FetchBuffer {
    std::vector<Uint8> bytes;
    bool overflow = false;
};

struct ResolvedAsset {
    std::string local;
    std::string url;
};

bool is_roblox_asset_host(const std::string& url) {
    // Exact origin prefix: do not treat assetdelivery.roblox.com.evil as ours.
    return url.rfind("https://assetdelivery.roblox.com/", 0) == 0;
}

size_t curl_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<FetchBuffer*>(userdata);
    const size_t n = size * nmemb;
    if (n > kMaxAssetBytes || out->bytes.size() > kMaxAssetBytes - n) {
        out->overflow = true;
        return 0;
    }
    out->bytes.insert(out->bytes.end(),
                      reinterpret_cast<Uint8*>(ptr),
                      reinterpret_cast<Uint8*>(ptr) + n);
    return n;
}

std::string numeric_asset_url(long long id) {
    return "https://assetdelivery.roblox.com/v1/asset/?id=" +
           std::to_string(id);
}

bool all_digits(const std::string& value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(),
                       [](unsigned char c) { return c >= '0' && c <= '9'; });
}

// Resolve a script-supplied SoundId to a local bundled file or a delivery URL.
// Built-in aliases (uuhhh -> oof.ogg) are followed, with the source entry's
// numeric id retained as a fallback for clients that do not ship the alias.
bool resolve_sound(const std::string& uri, ResolvedAsset* out,
                   std::string* why) {
    if (uri.rfind("rbxassetid://", 0) == 0) {
        const std::string id = uri.substr(std::strlen("rbxassetid://"));
        if (!all_digits(id)) {
            if (why) *why = "invalid rbxassetid URI";
            return false;
        }
        out->url = numeric_asset_url(std::stoll(id));
        return true;
    }
    if (uri.rfind("https://", 0) == 0 || uri.rfind("http://", 0) == 0) {
        out->url = uri;
        return true;
    }
    if (uri.rfind("rbxasset://", 0) != 0) {
        if (why) *why = "unsupported sound URI '" + uri + "'";
        return false;
    }

    std::string current = uri;
    for (int depth = 0; depth < 4; ++depth) {
        if (resolve_rbxasset(current, nullptr)) {
            out->local = current;
            return true;
        }
        const BuiltinSound builtin = find_builtin_sound(current);
        if (!builtin.alias.empty()) {
            const std::string alias = "rbxasset://sounds/" +
                                      std::string(builtin.alias);
            if (resolve_rbxasset(alias, nullptr)) {
                out->local = alias;
                return true;
            }
            const BuiltinSound alias_entry = find_builtin_sound(alias);
            if (alias_entry.id >= 0) {
                out->url = numeric_asset_url(alias_entry.id);
                return true;
            }
            if (builtin.id >= 0) {
                out->url = numeric_asset_url(builtin.id);
                return true;
            }
            current = alias;
            continue;
        }
        if (builtin.id >= 0) {
            out->url = numeric_asset_url(builtin.id);
            return true;
        }
        if (why)
            *why = "built-in sound '" + current +
                   "' is not in the native content tree and has no asset id";
        return false;
    }
    if (why) *why = "built-in sound alias chain is too deep: '" + uri + "'";
    return false;
}

Decoded decoded_from_s16(const short* samples, size_t count, int channels,
                         int rate) {
    Decoded out;
    out.channels = channels;
    out.rate = rate;
    out.pcm.resize(count);
    for (size_t i = 0; i < count; ++i)
        out.pcm[i] = float(samples[i]) / 32768.0f;
    return out;
}

bool decode_ogg(const std::vector<Uint8>& bytes, Decoded* out,
                std::string* why) {
    int channels = 0;
    int rate = 0;
    short* pcm = nullptr;
    const int frames = stb_vorbis_decode_memory(
        bytes.data(), int(bytes.size()), &channels, &rate, &pcm);
    if (frames < 0 || !pcm || channels <= 0 || channels > 2 || rate <= 0) {
        if (why) *why = "OGG Vorbis decode failed";
        free(pcm);
        return false;
    }
    *out = decoded_from_s16(pcm, size_t(frames) * size_t(channels), channels,
                            rate);
    free(pcm);
    return true;
}

bool decode_mp3(const std::vector<Uint8>& bytes, Decoded* out,
                std::string* why) {
    mp3dec_t decoder;
    mp3dec_init(&decoder);
    mp3dec_file_info_t info{};
    if (mp3dec_load_buf(&decoder, bytes.data(), bytes.size(), &info, nullptr,
                        nullptr) != 0 ||
        !info.buffer || info.samples == 0 || info.channels <= 0 ||
        info.channels > 2) {
        if (why) *why = "MP3 decode failed";
        free(info.buffer);
        return false;
    }
    out->channels = info.channels;
    out->rate = info.hz;
    out->pcm.resize(info.samples);
    for (size_t i = 0; i < info.samples; ++i)
        out->pcm[i] = std::clamp(info.buffer[i], -1.0f, 1.0f);
    free(info.buffer);
    return true;
}

bool decode_wav(const std::vector<Uint8>& bytes, Decoded* out,
                std::string* why) {
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        if (why) *why = "asset delivery returned a non-WAV payload";
        return false;
    }
    SDL_IOStream* io = SDL_IOFromMem(
        const_cast<Uint8*>(bytes.data()), bytes.size());
    if (!io) {
        if (why) *why = std::string("SDL_IOFromMem failed: ") + SDL_GetError();
        return false;
    }
    SDL_AudioSpec spec{};
    Uint8* pcm = nullptr;
    Uint32 len = 0;
    const bool ok = SDL_LoadWAV_IO(io, true, &spec, &pcm, &len);
    if (!ok || !pcm || spec.channels <= 0 || spec.channels > 2 ||
        spec.freq <= 0) {
        if (why) *why = std::string("WAV decode failed: ") + SDL_GetError();
        SDL_free(pcm);
        return false;
    }
    if (spec.format == SDL_AUDIO_F32) {
        out->channels = spec.channels;
        out->rate = spec.freq;
        const size_t count = len / sizeof(float);
        out->pcm.assign(reinterpret_cast<float*>(pcm),
                        reinterpret_cast<float*>(pcm) + count);
    } else if (spec.format == SDL_AUDIO_S16) {
        const size_t count = len / sizeof(short);
        *out = decoded_from_s16(reinterpret_cast<short*>(pcm), count,
                                spec.channels, spec.freq);
    } else {
        if (why) *why = "WAV payload is not 16-bit or F32 PCM";
        SDL_free(pcm);
        return false;
    }
    SDL_free(pcm);
    return true;
}

bool decode_audio(const std::vector<Uint8>& bytes, Decoded* out,
                  std::string* why) {
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "OggS", 4) == 0)
        return decode_ogg(bytes, out, why);
    if (bytes.size() >= 3 && std::memcmp(bytes.data(), "ID3", 3) == 0)
        return decode_mp3(bytes, out, why);
    if (bytes.size() >= 2 && bytes[0] == 0xFF && (bytes[1] & 0xE0) == 0xE0)
        return decode_mp3(bytes, out, why);
    return decode_wav(bytes, out, why);
}

// Read an audio frame with linear interpolation, wrapping at the end when the
// voice is looped. Returns false when the source has reached its end.
bool sample_voice(const Decoded& decoded, double cursor, bool looped,
                  float* left, float* right, bool* ended) {
    const size_t frames = decoded.frames();
    if (frames == 0 || cursor < 0.0) {
        *ended = true;
        return false;
    }
    if (cursor >= double(frames)) {
        *ended = true;
        if (!looped)
            return false;
        cursor = std::fmod(cursor, double(frames));
    }
    *ended = false;
    const size_t index = size_t(cursor);
    const size_t next = (index + 1 < frames) ? index + 1 : 0;
    const float frac = float(cursor - double(index));
    const size_t channels = size_t(decoded.channels);
    auto at = [&](size_t frame, size_t channel) {
        return decoded.pcm[frame * channels + channel];
    };
    if (decoded.channels == 1) {
        const float a = at(index, 0);
        const float b = at(next, 0);
        *left = a + (b - a) * frac;
        *right = *left;
    } else {
        const float l0 = at(index, 0);
        const float l1 = at(next, 0);
        const float r0 = at(index, 1);
        const float r1 = at(next, 1);
        *left = l0 + (l1 - l0) * frac;
        *right = r0 + (r1 - r0) * frac;
    }
    return true;
}

class SdlBackend final : public rbx::audio::Backend {
public:
    bool init(const std::string& cookie, std::string* err) {
        cookie_ = cookie;
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            if (err)
                *err = std::string("SDL audio init failed: ") + SDL_GetError();
            return false;
        }
        SDL_AudioSpec desired{};
        desired.format = SDL_AUDIO_F32;
        desired.channels = 2;
        desired.freq = kMixRate;
        device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired);
        if (!device_) {
            if (err)
                *err = std::string("no audio playback device: ") + SDL_GetError();
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return false;
        }
        int frames = 0;
        if (!SDL_GetAudioDeviceFormat(device_, &device_spec_, &frames)) {
            if (err)
                *err = std::string("audio device format failed: ") + SDL_GetError();
            SDL_CloseAudioDevice(device_);
            device_ = 0;
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return false;
        }
        SDL_AudioSpec mix_spec{};
        mix_spec.format = SDL_AUDIO_F32;
        mix_spec.channels = 2;
        mix_spec.freq = kMixRate;
        mix_stream_ = SDL_CreateAudioStream(&mix_spec, &device_spec_);
        if (!mix_stream_) {
            if (err)
                *err = std::string("audio mixer stream failed: ") + SDL_GetError();
            SDL_CloseAudioDevice(device_);
            device_ = 0;
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return false;
        }
        SDL_SetAudioStreamGetCallback(mix_stream_, stream_callback, this);
        if (!SDL_BindAudioStream(device_, mix_stream_)) {
            if (err)
                *err = std::string("audio mixer bind failed: ") + SDL_GetError();
            SDL_DestroyAudioStream(mix_stream_);
            mix_stream_ = nullptr;
            SDL_CloseAudioDevice(device_);
            device_ = 0;
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return false;
        }
        SDL_ResumeAudioStreamDevice(mix_stream_);
        initialized_ = true;
        rbx::audio::set_backend(this);
        return true;
    }

    void shutdown() {
        stop_all();
        if (mix_stream_) {
            SDL_UnbindAudioStream(mix_stream_);
            SDL_DestroyAudioStream(mix_stream_);
            mix_stream_ = nullptr;
        }
        if (device_) {
            SDL_CloseAudioDevice(device_);
            device_ = 0;
        }
        cache_.clear();
        if (initialized_) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            initialized_ = false;
        }
        rbx::audio::set_backend(nullptr);
    }

    bool play(rbx::Instance* sound, const std::string& uri, bool looped,
              double volume, std::string* why) override {
        stop(sound);
        if (!device_ || !mix_stream_) {
            if (why) *why = "audio device is not open";
            return false;
        }
        std::shared_ptr<Decoded> decoded;
        if (!load(uri, &decoded, why))
            return false;
        auto voice = std::make_unique<Voice>();
        voice->decoded = decoded;
        voice->looped.store(looped, std::memory_order_relaxed);
        voice->spatial = rbx::audio::is_spatial_parent(sound->parent());
        double start = 0.0;
        rbx::Variant time;
        if (sound->get_prop("TimePosition", time))
            if (const auto* value = std::get_if<double>(&time))
                start = std::max(0.0, *value);
        const double duration = decoded->duration();
        if (duration > 0.0)
            start = std::min(start, duration);
        voice->cursor.store(start * double(decoded->rate));
        voice->last_time = start;
        apply_mix_state(sound, voice.get(), volume);
        sound->set_internal_prop("IsLoaded", rbx::Variant(true));
        sound->set_internal_prop("IsSpatial", rbx::Variant(voice->spatial));
        sound->set_internal_prop("RollOffGain",
                                 rbx::Variant(voice->last_rolloff));
        sound->set_internal_prop("TimeLength", rbx::Variant(duration));
        {
            std::lock_guard<std::mutex> lock(voices_mutex_);
            voices_[sound] = std::move(voice);
        }
        return true;
    }

    void pause(rbx::Instance* sound) override {
        std::lock_guard<std::mutex> lock(voices_mutex_);
        const auto it = voices_.find(sound);
        if (it != voices_.end())
            it->second->paused.store(true, std::memory_order_relaxed);
    }

    void resume(rbx::Instance* sound) override {
        std::lock_guard<std::mutex> lock(voices_mutex_);
        const auto it = voices_.find(sound);
        if (it != voices_.end())
            it->second->paused.store(false, std::memory_order_relaxed);
    }

    void stop(rbx::Instance* sound) override {
        std::lock_guard<std::mutex> lock(voices_mutex_);
        voices_.erase(sound);
    }

    void forget(rbx::Instance* sound) override { stop(sound); }

    void set_volume(rbx::Instance* sound, double volume) override {
        std::lock_guard<std::mutex> lock(voices_mutex_);
        const auto it = voices_.find(sound);
        if (it == voices_.end())
            return;
        const double gain = std::max(0.0, volume);
        // A direct volume call is non-spatial (or a pre-listener update); the
        // per-frame property update will restore pan gains when appropriate.
        it->second->left_gain.store(gain, std::memory_order_relaxed);
        it->second->right_gain.store(gain, std::memory_order_relaxed);
    }

    bool is_playing(rbx::Instance* sound) override {
        std::lock_guard<std::mutex> lock(voices_mutex_);
        const auto it = voices_.find(sound);
        return it != voices_.end() &&
               !it->second->finished.load(std::memory_order_relaxed);
    }

    void poll() override {
        std::lock_guard<std::mutex> lock(voices_mutex_);
        for (auto it = voices_.begin(); it != voices_.end();) {
            rbx::Instance* sound = it->first;
            Voice* voice = it->second.get();
            if (voice->finished.load(std::memory_order_relaxed)) {
                sound->set_internal_prop("IsPlaying", rbx::Variant(false));
                sound->set_internal_prop("Playing", rbx::Variant(false));
                sound->set_internal_prop("IsPaused", rbx::Variant(false));
                sound->set_internal_prop("TimePosition", rbx::Variant(0.0));
                it = voices_.erase(it);
                continue;
            }

            const double volume = number_prop(sound, "Volume", 0.5);
            const double speed = std::max(0.0, number_prop(sound, "PlaybackSpeed", 1.0));
            const bool looped = bool_prop(sound, "Looped", false);
            voice->spatial = voice_is_spatial(sound);
            voice->speed.store(speed, std::memory_order_relaxed);
            voice->looped.store(looped, std::memory_order_relaxed);
            apply_mix_state(sound, voice, volume);
            sound->set_internal_prop("IsSpatial",
                                     rbx::Variant(voice->spatial));

            const double duration = voice->decoded->duration();
            double cursor = voice->cursor.load(std::memory_order_relaxed);
            double current = voice->decoded->rate > 0
                                 ? cursor / double(voice->decoded->rate)
                                 : 0.0;
            rbx::Variant time;
            if (sound->get_prop("TimePosition", time)) {
                if (const auto* requested = std::get_if<double>(&time)) {
                    if (std::abs(*requested - voice->last_time) > 0.15) {
                        const double seek =
                            std::clamp(*requested, 0.0, duration);
                        voice->seek.store(seek * voice->decoded->rate,
                                          std::memory_order_relaxed);
                        current = seek;
                    }
                }
            }
            voice->last_time = current;
            sound->set_internal_prop("TimeLength", rbx::Variant(duration));
            sound->set_internal_prop("TimePosition", rbx::Variant(current));
            sound->set_internal_prop("IsLoaded", rbx::Variant(true));
            sound->set_internal_prop("RollOffGain",
                                     rbx::Variant(voice->last_rolloff));
            ++it;
        }
    }

private:
    static void SDLCALL stream_callback(void* userdata,
                                        SDL_AudioStream* stream,
                                        int additional_amount, int) {
        static_cast<SdlBackend*>(userdata)->mix_audio(stream,
                                                       additional_amount);
    }

    void mix_audio(SDL_AudioStream* stream, int additional_amount) {
        if (additional_amount <= 0)
            return;
        const int frames = additional_amount / int(sizeof(float) * 2);
        if (frames <= 0)
            return;
        std::vector<float> out(size_t(frames) * 2, 0.0f);
        std::lock_guard<std::mutex> lock(voices_mutex_);
        for (auto& entry : voices_) {
            Voice& voice = *entry.second;
            if (voice.paused.load(std::memory_order_relaxed) ||
                voice.finished.load(std::memory_order_relaxed))
                continue;
            Decoded& decoded = *voice.decoded;
            if (decoded.frames() == 0)
                continue;
            double cursor = voice.cursor.load(std::memory_order_relaxed);
            const double seek = voice.seek.exchange(-1.0, std::memory_order_relaxed);
            if (seek >= 0.0)
                cursor = seek;
            const bool looped = voice.looped.load(std::memory_order_relaxed);
            const double step =
                double(decoded.rate) / double(kMixRate) *
                voice.speed.load(std::memory_order_relaxed);
            const double left_gain = voice.left_gain.load(std::memory_order_relaxed);
            const double right_gain = voice.right_gain.load(std::memory_order_relaxed);
            for (int frame = 0; frame < frames; ++frame) {
                float left = 0.0f;
                float right = 0.0f;
                bool ended = false;
                if (!sample_voice(decoded, cursor, looped, &left, &right, &ended)) {
                    voice.finished.store(true, std::memory_order_relaxed);
                    break;
                }
                out[size_t(frame) * 2] += left * float(left_gain);
                out[size_t(frame) * 2 + 1] += right * float(right_gain);
                cursor += step;
                if (cursor >= double(decoded.frames()) && !looped) {
                    // Let the next callback observe the end; this frame is
                    // still audible, which avoids clipping the tail.
                    voice.finished.store(true, std::memory_order_relaxed);
                    break;
                }
            }
            voice.cursor.store(cursor, std::memory_order_relaxed);
        }
        for (float& sample : out)
            sample = std::clamp(sample, -1.0f, 1.0f);
        SDL_PutAudioStreamData(stream, out.data(), additional_amount);
    }

    static double number_prop(rbx::Instance* sound, const char* name,
                              double fallback) {
        rbx::Variant value;
        if (sound->get_prop(name, value))
            if (const auto* number = std::get_if<double>(&value))
                return *number;
        return fallback;
    }

    static bool bool_prop(rbx::Instance* sound, const char* name,
                          bool fallback) {
        rbx::Variant value;
        if (sound->get_prop(name, value))
            if (const auto* flag = std::get_if<bool>(&value))
                return *flag;
        return fallback;
    }

    static int enum_prop(rbx::Instance* sound, const char* name,
                         int fallback) {
        rbx::Variant value;
        if (sound->get_prop(name, value))
            if (const auto* item = std::get_if<rbx::EnumItem>(&value))
                return item->value;
        return fallback;
    }

    static double group_volume(rbx::Instance* sound) {
        rbx::Variant value;
        if (!sound->get_prop("SoundGroup", value))
            return 1.0;
        const auto* group = std::get_if<rbx::Instance*>(&value);
        if (!group || !*group)
            return 1.0;
        return std::max(0.0, number_prop(*group, "Volume", 1.0));
    }

    // Snapshot properties on the main thread; the mixer only reads atomics.
    void apply_mix_state(rbx::Instance* sound, Voice* voice, double volume) {
        const double group = group_volume(sound);
        const double gain = std::max(0.0, volume) * group;
        const double attenuation = spatial_attenuation(sound);
        voice->last_rolloff = attenuation;
        if (!voice->spatial) {
            voice->left_gain.store(gain, std::memory_order_relaxed);
            voice->right_gain.store(gain, std::memory_order_relaxed);
            return;
        }
        rbx::CFrame listener;
        if (!rbx::audio::listener_world_cframe(sound, &listener)) {
            voice->left_gain.store(gain, std::memory_order_relaxed);
            voice->right_gain.store(gain, std::memory_order_relaxed);
            return;
        }
        const rbx::CFrame source =
            rbx::audio::instance_world_cframe(sound->parent());
        const double pan = rbx::audio::stereo_pan(listener, rbx::Vector3{
            source.x, source.y, source.z});
        const rbx::audio::StereoGain gains =
            rbx::audio::stereo_gains(attenuation, pan);
        voice->left_gain.store(gain * gains.left, std::memory_order_relaxed);
        voice->right_gain.store(gain * gains.right, std::memory_order_relaxed);
    }

    double spatial_attenuation(rbx::Instance* sound) {
        if (!voice_is_spatial(sound))
            return 1.0;
        rbx::CFrame listener;
        if (!rbx::audio::listener_world_cframe(sound, &listener))
            return 1.0;
        const rbx::CFrame source =
            rbx::audio::instance_world_cframe(sound->parent());
        const double dx = source.x - listener.x;
        const double dy = source.y - listener.y;
        const double dz = source.z - listener.z;
        const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        rbx::audio::SpatialSettings settings;
        settings.mode = enum_prop(sound, "RollOffMode", 0);
        settings.min_distance =
            number_prop(sound, "RollOffMinDistance", 10.0);
        settings.max_distance =
            number_prop(sound, "RollOffMaxDistance", 10000.0);
        settings.emitter_size = number_prop(sound, "EmitterSize", 10.0);
        return rbx::audio::roll_off_gain(settings, distance);
    }

    static bool voice_is_spatial(rbx::Instance* sound) {
        return rbx::audio::is_spatial_parent(sound->parent());
    }

    void stop_all() {
        std::lock_guard<std::mutex> lock(voices_mutex_);
        voices_.clear();
    }

    bool fetch(const std::string& url, bool with_cookie, FetchBuffer* body,
               long* status, std::string* redirect, std::string* why) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            if (why) *why = "curl init failed";
            return false;
        }
        struct curl_slist* headers = nullptr;
        if (with_cookie && !cookie_.empty())
            headers = curl_slist_append(
                headers, ("Cookie: " + cookie_header(cookie_)).c_str());
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        // A redirect is followed by hand so the session cookie is never
        // forwarded to the CDN (or to a script-supplied URL).
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Boblox/1.0");
        const CURLcode rc = curl_easy_perform(curl);
        char* location = nullptr;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
        curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
        if (location) *redirect = location;
        if (headers) curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (rc != CURLE_OK) {
            if (why)
                *why = std::string("asset delivery failed: ") +
                       curl_easy_strerror(rc) +
                       (body->overflow ? " (asset too large)" : "");
            return false;
        }
        return true;
    }

    bool load(const std::string& uri, std::shared_ptr<Decoded>* out,
              std::string* why) {
        const auto cached = cache_.find(uri);
        if (cached != cache_.end()) {
            *out = cached->second;
            return true;
        }
        ResolvedAsset resolved;
        if (!resolve_sound(uri, &resolved, why))
            return false;
        auto decoded = std::make_shared<Decoded>();
        if (!resolved.local.empty()) {
            std::filesystem::path local;
            if (!resolve_rbxasset(resolved.local, &local)) {
                if (why) *why = "cannot resolve built-in asset " + uri;
                return false;
            }
            std::ifstream in(local, std::ios::binary);
            if (!in) {
                if (why) *why = "cannot open built-in asset " + local.string();
                return false;
            }
            FetchBuffer disk;
            disk.bytes.assign(std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>());
            if (disk.bytes.size() > kMaxAssetBytes) {
                if (why) *why = "built-in asset exceeds the size cap";
                return false;
            }
            if (!decode_audio(disk.bytes, decoded.get(), why))
                return false;
        } else {
            FetchBuffer body;
            long status = 0;
            std::string redirect;
            if (!fetch(resolved.url, is_roblox_asset_host(resolved.url), &body,
                       &status, &redirect, why))
                return false;
            if (status >= 300 && status < 400) {
                if (redirect.empty()) {
                    if (why) *why = "asset delivery redirect had no location";
                    return false;
                }
                body.bytes.clear();
                status = 0;
                if (!fetch(redirect, false, &body, &status, &redirect, why))
                    return false;
            }
            if (status < 200 || status >= 300) {
                if (why)
                    *why = "asset delivery returned HTTP " +
                           std::to_string(status);
                return false;
            }
            if (!decode_audio(body.bytes, decoded.get(), why))
                return false;
        }
        cache_[uri] = decoded;
        *out = decoded;
        return true;
    }

    SDL_AudioDeviceID device_ = 0;
    SDL_AudioSpec device_spec_{};
    SDL_AudioStream* mix_stream_ = nullptr;
    bool initialized_ = false;
    std::string cookie_;
    std::unordered_map<std::string, std::shared_ptr<Decoded>> cache_;
    std::unordered_map<rbx::Instance*, std::unique_ptr<Voice>> voices_;
    std::mutex voices_mutex_;
};

SdlBackend g_backend;

} // namespace

bool start(const std::string& cookie, std::string* err) {
    stop();
    if (!g_backend.init(cookie, err))
        return false;
    std::fprintf(stderr, "audio: SDL playback enabled\n");
    return true;
}

void stop() { g_backend.shutdown(); }

} // namespace audio
} // namespace rbxclient
