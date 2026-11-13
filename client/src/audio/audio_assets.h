// audio_assets.h — the native client's built-in rbxasset://sounds table.
//
// The table is extracted from the shipped Android client's redirect table
// (see audio_builtins.inc); it maps the legacy Roblox sound names used by
// 2016-era places to the asset ids the current client delivers. A few names
// are aliases to another bundled file (uuhhh/ouch -> oof.ogg), so resolution
// returns either a numeric id or an alias path.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace rbxclient {
namespace audio {

struct BuiltinSound {
    std::string_view name;
    long long id = -1;
    std::string_view alias;
};

// Case-insensitive lookup by the basename of a rbxasset://sounds URI.
// Returns a default (id == -1, empty alias) when the name is not built in.
BuiltinSound find_builtin_sound(std::string_view uri);

// Number of names in the native redirect table. Kept as a probe/test hook so
// a future client update can be compared against this pinned set.
std::size_t builtin_sound_count();

} // namespace audio
} // namespace rbxclient
