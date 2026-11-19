#include "settings.hpp"
#include "route.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace coach {
namespace {
constexpr std::size_t max_settings_bytes = 8192;
std::string_view trim(std::string_view text) {
    constexpr std::string_view spaces{" \r\t"};
    const auto begin = text.find_first_not_of(spaces);
    return begin == std::string_view::npos ? std::string_view{} :
        text.substr(begin, text.find_last_not_of(spaces) - begin + 1);
}

std::string upper(std::string_view text) {
    std::string result(text);
    for (auto& ch : result) {
        if (ch >= 'a' && ch <= 'z') {
            ch -= 'a' - 'A';
        }
    }
    return result;
}

std::optional<int> number(std::string_view text) {
    int value{};
    if (text.empty()) {
        return std::nullopt;
    }
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec == std::errc{} && end == text.data() + text.size()) {
        return value;
    }
    return std::nullopt;
}

unsigned char key_code(std::string_view token) {
    if (token.size() == 1 && ((token[0] >= 'A' && token[0] <= 'Z')
                              || (token[0] >= '0' && token[0] <= '9'))) {
        return static_cast<unsigned char>(token[0]);
    }
    if (token.starts_with("F")) {
        if (const auto code = number(token.substr(1)); code && *code >= 1 && *code <= 24) {
            return static_cast<unsigned char>(0x70 + *code - 1);
        }
    }
    if (token.size() == 4 && token.starts_with("NUM") && token[3] >= '0' && token[3] <= '9') {
        return static_cast<unsigned char>(0x60 + token[3] - '0');
    }
    if (token == "END") return 0x23;
    if (token == "HOME") return 0x24;
    if (token == "INSERT") return 0x2d;
    if (token == "PAGEUP") return 0x21;
    if (token == "PAGEDOWN") return 0x22;
    return 0;
}

std::optional<Chord> parse_chord(std::string_view text) {
    Chord chord{};
    if (upper(trim(text)) == "OFF") {
        return chord;
    }
    for (;;) {
        const auto plus = text.find('+');
        const auto token = upper(trim(text.substr(0, plus)));
        if (plus == std::string_view::npos) {
            chord.key = key_code(token);
            return chord.key ? std::optional<Chord>(chord) : std::nullopt;
        }
        const unsigned char modifier = token == "CTRL" ? 1 : token == "SHIFT" ? 2 : token == "ALT" ? 4 : 0;
        if (modifier == 0 || (chord.modifiers & modifier)) {
            return std::nullopt;
        }
        chord.modifiers |= modifier;
        text.remove_prefix(plus + 1);
    }
}

} // namespace

Settings read_settings(const std::filesystem::path& directory) {
    Settings settings;
    const auto path = directory / "settings.ini";
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return settings;
    }

    if (std::filesystem::file_size(path, error) > max_settings_bytes || error) {
        settings.invalid_entries++;
        return settings;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        settings.invalid_entries++;
        return settings;
    }
    std::string bytes(max_settings_bytes + 1, '\0');
    input.read(bytes.data(), bytes.size());
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    if (input.bad() || bytes.size() > max_settings_bytes) {
        settings.invalid_entries++;
        return settings;
    }
    if (bytes.starts_with("\xef\xbb\xbf")) {
        bytes.erase(0, 3);
    }
    std::istringstream lines(std::move(bytes));
    for (std::string line; std::getline(lines, line);) {
        const auto view = trim(std::string_view(line).substr(0, line.find_first_of(";#")));
        if (view.empty()) {
            continue;
        }
        const auto split = view.find('=');
        if (split != std::string_view::npos) {
            const auto field = upper(trim(view.substr(0, split)));
            const auto value = trim(view.substr(split + 1));
            if (field == "LANGUAGE" && (value == "all" || value == "de" || value == "en" || value == "zh" || value == "ja")) {
                settings.language = value;
                continue;
            }
            if (field == "VOLUME") {
                if (const auto volume = number(value); volume && *volume >= 0 && *volume <= 100) {
                    settings.volume = *volume;
                    continue;
                }
            }
            if (field == "AUTO_NEXT" || field == "AUTO_FINISH") {
                if (value == "on" || value == "off") {
                    (field == "AUTO_NEXT" ? settings.auto_next : settings.auto_finish) = value == "on";
                    continue;
                }
            }
            if (field == "AUTO_APPROACH_M") {
                if (const auto metres = number(value);
                    metres && (*metres == 0 || (*metres >= 300 && *metres <= 25000))) {
                    settings.approach_metres = *metres;
                    continue;
                }
            }
            bool parsed = false;
            for (std::size_t i = 0; i < settings.keys.size(); ++i) {
                if (field == upper(action_names[i])) {
                    if (const auto chord = parse_chord(value)) {
                        settings.keys[i] = *chord;
                        parsed = true;
                    }
                    break;
                }
            }
            if (parsed) {
                continue;
            }
        }
        settings.invalid_entries++;
    }
    const auto requested = settings.keys;
    for (std::size_t i = 0; i < settings.keys.size(); ++i) {
        if (requested[i].key && std::ranges::count(requested, requested[i]) > 1) {
            settings.keys[i] = {};
            settings.invalid_entries++;
        }
    }
    return settings;
}

std::string chord_label(Chord chord) {
    if (!chord.key) return "Off";
    std::string label;
    if (chord.modifiers & 1) label += "Ctrl+";
    if (chord.modifiers & 2) label += "Shift+";
    if (chord.modifiers & 4) label += "Alt+";
    if (chord.key >= 0x60 && chord.key <= 0x69) return label + "Num" + std::to_string(chord.key - 0x60);
    if (chord.key >= 0x70 && chord.key <= 0x87) return label + "F" + std::to_string(chord.key - 0x6f);
    switch (chord.key) {
        case 0x21: return label + "PageUp";
        case 0x22: return label + "PageDown";
        case 0x23: return label + "End";
        case 0x24: return label + "Home";
        case 0x2d: return label + "Insert";
        default: return label + static_cast<char>(chord.key);
    }
}

namespace {
struct Change {
    std::string_view field;
    std::string value;
};

bool save(const std::filesystem::path& directory, std::vector<Change> changes) {
    const auto path = directory / "settings.ini";
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) return false;
    std::string contents;
    std::string clean;
    if (exists) {
        std::ifstream input(path, std::ios::binary);
        contents.resize(max_settings_bytes + 1);
        if (!input) return false;
        input.read(contents.data(), contents.size());
        contents.resize(static_cast<std::size_t>(input.gcount()));
        if (input.bad() || contents.size() > max_settings_bytes) return false;
        if (contents.starts_with("\xef\xbb\xbf")) {
            clean = "\xef\xbb\xbf";
            contents.erase(0, 3);
        }
    }
    const std::string_view newline = contents.find("\r\n") == std::string::npos ? "\n" : "\r\n";
    std::istringstream lines(std::move(contents));
    for (std::string line; std::getline(lines, line);) {
        const auto value = trim(std::string_view(line).substr(0, line.find_first_of(";#")));
        const auto split = value.find('=');
        const auto field = split == std::string_view::npos ? std::string{} : upper(trim(value.substr(0, split)));
        if (std::any_of(changes.begin(), changes.end(), [&](const auto& change) {
                return upper(change.field) == field;
            })) {
            continue;
        }
        if (line.ends_with('\r')) line.pop_back();
        clean += line;
        clean += newline;
    }
    for (const auto& [key, value] : changes) {
        clean += key;
        clean += '=';
        clean += value;
        clean += newline;
    }
    if (clean.size() > max_settings_bytes) return false;
    static std::atomic<unsigned> next_write{};
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = getpid();
#endif
    const auto temporary = directory / ("settings.ini.coach-tmp-" + std::to_string(pid)
        + "-" + std::to_string(++next_write));
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out.write(clean.data(), static_cast<std::streamsize>(clean.size()));
    out.close();
    if (out) {
#ifdef _WIN32
        if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
#else
        std::filesystem::rename(temporary, path, error);
        if (!error) return true;
#endif
    }
    std::filesystem::remove(temporary, error);
    return false;
}
} // namespace

panel::Result update_settings(Settings& settings, const std::filesystem::path& directory,
                              const panel::Command& command) {
    using panel::Operation;
    using panel::Result;
    std::vector<Change> changes;
    const auto value = command.value;
    switch (command.operation) {
        case Operation::language:
            if (value >= std::size(panel::languages)) return Result::invalid;
            changes.push_back({"language", panel::languages[value]});
            break;
        case Operation::volume:
            if (value > 100) return Result::invalid;
            changes.push_back({"volume", std::to_string(value)});
            break;
        case Operation::auto_next:
        case Operation::auto_finish:
            if (value > 1) return Result::invalid;
            changes.push_back({command.operation == Operation::auto_next ? "auto_next" : "auto_finish",
                               value ? "on" : "off"});
            break;
        case Operation::approach:
            if (value && (value < 300 || value > 25000)) return Result::invalid;
            changes.push_back({"auto_approach_m", std::to_string(value)});
            break;
        case Operation::binding: {
            if (value >= settings.keys.size()) return Result::invalid;
            const auto chord = parse_chord(command.text);
            if (!chord) return Result::invalid;
            const auto current = read_settings(directory);
            for (std::size_t i = 0; i < current.keys.size(); ++i) {
                if (i != value && chord->key && current.keys[i] == *chord) return Result::invalid;
            }
            changes.push_back({action_names[value], chord_label(*chord)});
            break;
        }
        case Operation::reset: {
            const Settings defaults;
            changes = {{"language", defaults.language}, {"volume", std::to_string(defaults.volume)},
                       {"auto_next", "on"}, {"auto_finish", "on"}, {"auto_approach_m", "0"}};
            for (std::size_t i = 0; i < defaults.keys.size(); ++i) {
                changes.push_back({action_names[i], chord_label(defaults.keys[i])});
            }
            break;
        }
        default: return Result::invalid;
    }
    if (!save(directory, std::move(changes))) return Result::write_failed;
    settings = read_settings(directory);
    return Result::applied;
}

} // namespace coach
