#pragma once

#include "panel_snapshot.hpp"

#include <array>
#include <filesystem>
#include <string>

namespace coach {

struct Chord {
    unsigned char key{};
    unsigned char modifiers{};
    bool operator==(const Chord&) const = default;
};

struct Settings {
    std::string language{"all"};
    int volume{65};
    int approach_metres{};
    bool auto_next{true};
    bool auto_finish{true};
    std::array<Chord, 4> keys{{{0x61, 0}, {0x62, 0}, {0x63, 0}, {0x65, 0}}};
    unsigned invalid_entries{};
};

Settings read_settings(const std::filesystem::path& directory);
std::string chord_label(Chord chord);
panel::Result update_settings(Settings& settings, const std::filesystem::path& directory,
                              const panel::Command& command);

} // namespace coach
