#pragma once

#include "route.hpp"

#include <filesystem>
#include <vector>

namespace coach {

enum class ClipKind { generic, named, assembled, multilingual };

struct Clip {
    std::vector<std::int16_t> samples;
    ClipKind kind{ClipKind::generic};
    bool terminal_fallback{};
};

Clip load_clip(const std::filesystem::path& directory, Action action,
               const std::string& station, int volume, bool terminal_before_arrival = false);

// A sequence is rendered into one buffer so stop and queue semantics stay atomic.
Clip load_broadcast(const std::filesystem::path& audio_root, std::string_view language,
                    Action action, const std::string& station, int volume,
                    bool terminal_before_arrival = false);

} // namespace coach
