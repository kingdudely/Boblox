// audio.cpp — SDL3 playback for the runtime's Sound API.
//
// Resource URIs resolve through the local `rbxasset://` tree first, then
// Roblox assetdelivery with the session cookie (the cookie is never printed).
// WAV, OGG Vorbis and MP3 are decoded locally and mixed by SDL audio streams.
// Unknown built-ins and decode failures fail the Play instead of setting
// IsPlaying=true; no device means start() fails and Sound:Play stays false.
#include "audio/audio.h"

#include "common/asset_path.h"
#include "instance/audio_host.h"
#include "instance/class_registry.h"
#include "instance/instance.h"
#include "instance/variant.h"

#include <SDL3/SDL.h>

#include <curl/curl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace rbxclient {
namespace audio {
namespace {

constexpr size_t kMaxAssetBytes = 32u * 1024u * 1024u;

// stb_vorbis is built as C; minimp3 is header-only and implemented below.
extern "C" int stb_vorbis_decode_memory(const unsigned char* mem, int len,
                                        int* channels, int* sample_rate,
                                        short** output);

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3_ex.h"

struct Decoded {
    SDL_AudioSpec spec{};
    std::vector<Uint8> pcm;
};

struct Voice {
    SDL_AudioStream* stream = nullptr;
    std::shared_ptr<Decoded> decoded;
    size_t pos = 0;
    bool loop = false;
    bool paused = false;
};

struct FetchBuffer {
    std::vector<Uint8> bytes;
    bool overflow = false;
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

std::string lowercase(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// IDs verified for the built-ins observed in place 1818. Unknown names must
// fail rather than silently play a different sound.
long long builtin_sound_id(const std::string& uri) {
    std::string name = uri;
    const size_t slash = name.find_last_of('/');
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    const size_t query = name.find_first_of("?#");
    if (query != std::string::npos)
        name = name.substr(0, query);
    name = lowercase(name);
    static const std::map<std::string, long long> ids = {
        {"electronicpingshort.wav", 12221990},
        {"collide.wav", 12221984},
        {"glassbreak.wav", 12222005},
    };
    const auto it = ids.find(name);
    return it == ids.end() ? -1 : it->second;
}

std::string asset_url(const std::string& uri, std::string* why) {
    if (uri.rfind("rbxassetid://", 0) == 0) {
        const std::string id = uri.substr(std::strlen("rbxassetid://"));
        if (id.empty() ||
            !std::all_of(id.begin(), id.end(),
                         [](unsigned char c) { return c >= '0' && c <= '9'; })) {
            if (why) *why = "invalid rbxassetid URI";
            return {};
        }
        return "https://assetdelivery.roblox.com/v1/asset/?id=" + id;
    }
    if (uri.rfind("rbxasset://", 0) == 0) {
        const long long id = builtin_sound_id(uri);
        if (id < 0) {
            if (why)
                *why = "built-in sound '" + uri +
                       "' has no known asset id in this client";
            return {};
        }
        return "https://assetdelivery.roblox.com/v1/asset/?id=" +
               std::to_string(id);
    }
    if (uri.rfind("https://", 0) == 0 || uri.rfind("http://", 0) == 0)
        return uri;
    if (why) *why = "unsupported sound URI '" + uri + "'";
    return {};
}

bool decode_ogg(const std::vector<Uint8>& bytes, Decoded* out,
                std::string* why) {
    int channels = 0;
    int rate = 0;
    short* pcm = nullptr;
    const int frames = stb_vorbis_decode_memory(
        bytes.data(), int(bytes.size()), &channels, &rate, &pcm);
    if (frames < 0 || !pcm || channels <= 0 || rate <= 0) {
        if (why)
            *why = "OGG Vorbis decode failed";
        if (pcm)
            free(pcm);
        return false;
    }
    out->spec.format = SDL_AUDIO_S16;
    out->spec.channels = channels;
    out->spec.freq = rate;
    const size_t samples = size_t(frames) * size_t(channels);
    out->pcm.resize(samples * sizeof(short));
    std::memcpy(out->pcm.data(), pcm, out->pcm.size());
    free(pcm);
    return true;
}

bool decode_mp3(const std::vector<Uint8>& bytes, Decoded* out,
                std::string* why) {
    mp3dec_t decoder;
    mp3dec_init(&decoder);
    mp3dec_file_info_t info{};
    if (mp3dec_load_buf(&decoder, bytes.data(), bytes.size(), &info, nullptr,
                        nullptr) != 0 || !info.buffer || info.samples == 0) {
        if (why)
            *why = "MP3 decode failed";
        free(info.buffer);
        return false;
    }
    out->spec.format = SDL_AUDIO_S16;
    out->spec.channels = info.channels;
    out->spec.freq = info.hz;
    out->pcm.resize(info.samples * sizeof(short));
    auto* dst = reinterpret_cast<short*>(out->pcm.data());
    for (size_t i = 0; i < info.samples; ++i) {
        const float v = std::clamp(info.buffer[i], -1.0f, 1.0f);
        dst[i] = short(std::lrint(v * 32767.0f));
    }
    free(info.buffer);
    return true;
}

bool decode_wav(const std::vector<Uint8>& bytes, Decoded* out,
                std::string* why);

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

bool decode_wav(const std::vector<Uint8>& bytes, Decoded* out,
                std::string* why) {
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        if (why)
            *why = "asset delivery returned a non-WAV payload";
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
    if (!ok) {
        if (why) *why = std::string("WAV decode failed: ") + SDL_GetError();
        return false;
    }
    out->spec = spec;
    out->pcm.assign(pcm, pcm + len);
    SDL_free(pcm);
    return true;
}

void SDLCALL loop_callback(void* userdata, SDL_AudioStream* stream,
                           int additional_amount, int /*total_amount*/) {
    auto* voice = static_cast<Voice*>(userdata);
    if (!voice || !voice->loop || !voice->decoded || voice->decoded->pcm.empty())
        return;
    int left = additional_amount;
    const size_t size = voice->decoded->pcm.size();
    while (left > 0) {
        const size_t n = std::min<size_t>(size_t(left), size - voice->pos);
        if (n == 0)
            break;
        if (!SDL_PutAudioStreamData(
                stream, voice->decoded->pcm.data() + voice->pos, int(n)))
            break;
        voice->pos += n;
        if (voice->pos == size)
            voice->pos = 0;
        left -= int(n);
    }
}

class SdlBackend final : public rbx::audio::Backend {
public:
    bool init(const std::string& cookie, std::string* err) {
        cookie_ = cookie;
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            if (err) *err = std::string("SDL audio init failed: ") + SDL_GetError();
            return false;
        }
        SDL_AudioSpec desired{};
        desired.format = SDL_AUDIO_F32;
        desired.channels = 2;
        desired.freq = 48000;
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
        initialized_ = true;
        rbx::audio::set_backend(this);
        return true;
    }

    void shutdown() {
        stop_all();
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
        if (!device_) {
            if (why) *why = "audio device is not open";
            return false;
        }
        std::shared_ptr<Decoded> decoded;
        if (!load(uri, &decoded, why))
            return false;
        auto voice = std::make_unique<Voice>();
        voice->decoded = decoded;
        voice->loop = looped;
        voice->stream =
            SDL_CreateAudioStream(&decoded->spec, &device_spec_);
        if (!voice->stream) {
            if (why) *why = std::string("audio stream failed: ") + SDL_GetError();
            return false;
        }
        SDL_SetAudioStreamGain(voice->stream, float(std::max(0.0, volume)));
        if (looped)
            SDL_SetAudioStreamGetCallback(voice->stream, loop_callback,
                                          voice.get());
        else if (!decoded->pcm.empty() &&
                 !SDL_PutAudioStreamData(voice->stream, decoded->pcm.data(),
                                         int(decoded->pcm.size()))) {
            if (why)
                *why = std::string("audio queue failed: ") + SDL_GetError();
            SDL_DestroyAudioStream(voice->stream);
            return false;
        }
        if (!SDL_BindAudioStream(device_, voice->stream)) {
            if (why) *why = std::string("audio bind failed: ") + SDL_GetError();
            SDL_DestroyAudioStream(voice->stream);
            return false;
        }
        SDL_ResumeAudioStreamDevice(voice->stream);
        voices_[sound] = std::move(voice);
        return true;
    }

    void pause(rbx::Instance* sound) override {
        const auto it = voices_.find(sound);
        if (it == voices_.end() || !it->second->stream)
            return;
        it->second->paused = true;
        SDL_PauseAudioStreamDevice(it->second->stream);
    }

    void resume(rbx::Instance* sound) override {
        const auto it = voices_.find(sound);
        if (it == voices_.end() || !it->second->stream)
            return;
        it->second->paused = false;
        SDL_ResumeAudioStreamDevice(it->second->stream);
    }

    void stop(rbx::Instance* sound) override {
        const auto it = voices_.find(sound);
        if (it == voices_.end())
            return;
        destroy_voice(it->second.get());
        voices_.erase(it);
    }

    void forget(rbx::Instance* sound) override { stop(sound); }

    void set_volume(rbx::Instance* sound, double volume) override {
        const auto it = voices_.find(sound);
        if (it != voices_.end() && it->second->stream)
            SDL_SetAudioStreamGain(it->second->stream,
                                   float(std::max(0.0, volume)));
    }

    bool is_playing(rbx::Instance* sound) override {
        const auto it = voices_.find(sound);
        if (it == voices_.end() || !it->second->stream || it->second->paused)
            return false;
        return SDL_GetAudioStreamAvailable(it->second->stream) > 0;
    }

    void poll() override {
        for (auto it = voices_.begin(); it != voices_.end();) {
            Voice* voice = it->second.get();
            if (voice->stream && !voice->paused && !voice->loop &&
                SDL_GetAudioStreamAvailable(voice->stream) == 0) {
                rbx::Instance* sound = it->first;
                sound->set_internal_prop("IsPlaying", rbx::Variant(false));
                sound->set_internal_prop("Playing", rbx::Variant(false));
                destroy_voice(voice);
                it = voices_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    void destroy_voice(Voice* voice) {
        if (!voice || !voice->stream)
            return;
        SDL_UnbindAudioStream(voice->stream);
        SDL_DestroyAudioStream(voice->stream);
        voice->stream = nullptr;
    }

    void stop_all() {
        for (auto& entry : voices_)
            destroy_voice(entry.second.get());
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
            headers = curl_slist_append(headers, ("Cookie: " + cookie_).c_str());
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        // A redirect is followed by hand so the session cookie is never
        // forwarded to the CDN (or to a script-supplied URL).
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        // The CDN serves asset bytes with content-encoding: gzip. Let curl
        // decode it; otherwise the payload looks like a non-WAV blob.
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
        // A built-in under the extracted native tree is loaded from disk,
        // exactly like the Android client: no network, no cookie, and the
        // same content hash the shipped build uses.
        std::filesystem::path local;
        if (resolve_rbxasset(uri, &local)) {
            std::ifstream in(local, std::ios::binary);
            if (!in) {
                if (why)
                    *why = "cannot open built-in asset " + local.string();
                return false;
            }
            FetchBuffer disk;
            disk.bytes.assign(std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>());
            if (disk.bytes.size() > kMaxAssetBytes) {
                if (why)
                    *why = "built-in asset exceeds the size cap";
                return false;
            }
            auto decoded = std::make_shared<Decoded>();
            if (!decode_audio(disk.bytes, decoded.get(), why))
                return false;
            cache_[uri] = decoded;
            *out = decoded;
            return true;
        }
        std::string url = asset_url(uri, why);
        if (url.empty())
            return false;
        FetchBuffer body;
        long status = 0;
        std::string redirect;
        if (!fetch(url, is_roblox_asset_host(url), &body, &status, &redirect,
                   why))
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
                *why = "asset delivery returned HTTP " + std::to_string(status);
            return false;
        }
        auto decoded = std::make_shared<Decoded>();
        if (!decode_audio(body.bytes, decoded.get(), why))
            return false;
        cache_[uri] = decoded;
        *out = decoded;
        return true;
    }

    SDL_AudioDeviceID device_ = 0;
    SDL_AudioSpec device_spec_{};
    bool initialized_ = false;
    std::string cookie_;
    std::unordered_map<std::string, std::shared_ptr<Decoded>> cache_;
    std::unordered_map<rbx::Instance*, std::unique_ptr<Voice>> voices_;
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

void stop() {
    g_backend.shutdown();
}

} // namespace audio
} // namespace rbxclient
