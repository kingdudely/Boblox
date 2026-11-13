// audio_assets.cpp — see audio_assets.h and audio_builtins.inc.
#include "audio/audio_assets.h"

namespace rbxclient {
namespace audio {
namespace {

constexpr BuiltinSound kBuiltinSounds[] = {
#include "audio/audio_builtins.inc"
};

std::string lowercase(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return s;
}

std::string basename_without_query(std::string_view uri) {
    const size_t slash = uri.find_last_of('/');
    if (slash != std::string_view::npos)
        uri.remove_prefix(slash + 1);
    const size_t query = uri.find_first_of("?#");
    if (query != std::string_view::npos)
        uri = uri.substr(0, query);
    return lowercase(std::string(uri));
}

} // namespace

BuiltinSound find_builtin_sound(std::string_view uri) {
    const std::string name = basename_without_query(uri);
    for (const BuiltinSound& entry : kBuiltinSounds) {
        if (entry.name == name)
            return entry;
    }
    return {};
}

std::size_t builtin_sound_count() {
    return sizeof(kBuiltinSounds) / sizeof(kBuiltinSounds[0]);
}

} // namespace audio
} // namespace rbxclient
