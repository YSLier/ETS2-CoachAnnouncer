#include "audio_clip.hpp"

#include <algorithm>
#include <fstream>

namespace coach {
namespace {

constexpr std::size_t max_station_samples = 15 * 22050;
constexpr std::size_t max_announcement_samples = 45 * 22050;
constexpr std::size_t pause_samples = 2205;

// Release assets use 22,050 Hz mono signed 16-bit PCM. Accept ordinary WAV
// container chunks, with bounded file and clip sizes.
std::vector<std::int16_t> wav(const std::filesystem::path& path, int volume,
                              std::size_t max_samples = max_announcement_samples) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    input.seekg(0, std::ios::end);
    const auto size_on_disk = input.tellg();
    if (size_on_disk < 12 || size_on_disk > static_cast<std::streamoff>(max_samples * 2 + 4096)) {
        return {};
    }
    input.seekg(0, std::ios::beg);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size_on_disk));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return {};
    }
    const auto tag = [&](std::size_t pos) { return std::string_view(
        reinterpret_cast<const char*>(bytes.data() + pos), 4); };
    const auto read16 = [&](std::size_t pos) -> unsigned {
        return bytes[pos] | (static_cast<unsigned>(bytes[pos + 1]) << 8);
    };
    const auto read32 = [&](std::size_t pos) -> std::uint32_t {
        return read16(pos) | (static_cast<std::uint32_t>(read16(pos + 2)) << 16);
    };
    if (tag(0) != "RIFF" || tag(8) != "WAVE" || read32(4) != bytes.size() - 8) {
        return {};
    }
    bool pcm = false;
    for (std::size_t pos = 12; pos + 8 <= bytes.size();) {
        const auto name = tag(pos);
        const std::size_t size = read32(pos + 4);
        pos += 8;
        if (size + (size & 1) > bytes.size() - pos) {
            return {};
        }
        if (name == "fmt " && size >= 16) {
            pcm = read16(pos) == 1 && read16(pos + 2) == 1 && read32(pos + 4) == 22050
                  && read16(pos + 12) == 2 && read16(pos + 14) == 16;
        } else if (name == "data" && pcm && size && !(size & 1) && size / 2 <= max_samples) {
            std::vector<std::int16_t> pcm_data(size / 2);
            for (std::size_t i = 0; i < pcm_data.size(); ++i) {
                const auto word = static_cast<int>(read16(pos + 2 * i));
                const int sample = word < 32768 ? word : word - 65536;
                pcm_data[i] = static_cast<std::int16_t>(sample * volume / 100);
            }
            return pcm_data;
        }
        pos += size + (size & 1);
    }
    return {};
}

Clip load_family(const std::filesystem::path& directory, Action action,
                 const std::string& station, int volume, bool terminal_before_arrival) {
    const auto name = std::string(action == Action::finish && terminal_before_arrival
        ? "terminal_soon" : action_names[static_cast<std::size_t>(action)]);
    if (!station.empty()) {
        if (auto custom = wav(directory / (name + "." + station + ".wav"), volume);
            !custom.empty()) {
            return {std::move(custom), ClipKind::named};
        }
        const auto parts = directory / "parts";
        auto place = wav(directory / "stations" / (station + ".wav"), volume, max_station_samples);
        if (!place.empty()) {
            auto intro = wav(parts / (name + ".before.wav"), volume);
            auto outro = wav(parts / (name + ".after.wav"), volume);
            const auto between_path = parts / (name + ".between.wav");
            std::error_code error;
            const bool repeats_station = std::filesystem::exists(between_path, error);
            auto between = repeats_station ? wav(between_path, volume) : std::vector<std::int16_t>{};
            const auto total = intro.size() + place.size() + outro.size()
                + pause_samples * (outro.empty() ? 1 : 2)
                + (repeats_station ? between.size() + place.size() + 2 * pause_samples : 0);
            const bool needs_outro = action != Action::next;
            if (!intro.empty() && (!needs_outro || !outro.empty()) && !error
                && (!repeats_station || !between.empty()) && total <= max_announcement_samples) {
                intro.reserve(total);
                const auto append = [&](const auto& part) {
                    if (!part.empty()) {
                        intro.resize(intro.size() + pause_samples, 0);
                        intro.insert(intro.end(), part.begin(), part.end());
                    }
                };
                append(place);
                if (repeats_station) {
                    append(between);
                    append(place);
                }
                append(outro);
                return {std::move(intro), ClipKind::assembled};
            }
        }
    }
    return {wav(directory / (name + ".wav"), volume), ClipKind::generic};
}

} // namespace

Clip load_clip(const std::filesystem::path& directory, Action action,
               const std::string& station, int volume, bool terminal_before_arrival) {
    auto clip = load_family(directory, action, station, volume, terminal_before_arrival);
    if (clip.samples.empty() && action == Action::finish && terminal_before_arrival) {
        clip = load_family(directory, Action::approaching, station, volume, false);
        clip.terminal_fallback = !clip.samples.empty();
    }
    return clip;
}

Clip load_broadcast(const std::filesystem::path& audio_root, std::string_view language,
                    Action action, const std::string& station, int volume, bool terminal_before_arrival) {
    constexpr std::size_t language_gap = 9923; // 450 ms at 22,050 Hz.
    auto cue = wav(audio_root / "chime.wav", volume);
    Clip speech{cue, ClipKind::multilingual};
    const auto append = [&](Clip part, bool gap) {
        if (part.samples.empty()) {
            return false;
        }
        speech.terminal_fallback |= part.terminal_fallback;
        // Old whole-WAV overrides can already start with the original chime.
        const auto begin = part.samples.size() > cue.size()
            && std::equal(cue.begin(), cue.end(), part.samples.begin())
            ? part.samples.begin() + cue.size() : part.samples.begin();
        if (gap) {
            speech.samples.resize(speech.samples.size() + language_gap, 0);
        }
        speech.samples.insert(speech.samples.end(), begin, part.samples.end());
        return true;
    };

    if (language != "all") {
        auto part = load_clip(audio_root / std::string(language), action, station, volume, terminal_before_arrival);
        speech.kind = part.kind;
        return append(std::move(part), false) ? std::move(speech) : Clip{};
    }
    bool gap = false;
    for (const auto* code : {"de", "en", "zh", "ja"}) {
        if (!append(load_clip(audio_root / code, action, station, volume, terminal_before_arrival), gap)) {
            return {};
        }
        gap = true;
    }
    return speech;
}

} // namespace coach
