#include "audio_clip.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string_view>

using coach::Action;
using coach::ClipKind;

void check(bool good, const char* message) {
    if (!good) {
        throw std::runtime_error(message);
    }
}

int main(int argc, char** argv) {
    if (argc == 6 && std::string_view(argv[1]) == "render") {
        const auto action = static_cast<Action>(std::stoi(argv[3]));
        const auto clip = coach::load_broadcast(argv[2], "all", action, argv[4], 100);
        check(clip.kind == ClipKind::multilingual, "example must use all four languages");
        std::ofstream out(argv[5], std::ios::binary);
        out.write(reinterpret_cast<const char*>(clip.samples.data()),
                  static_cast<std::streamsize>(clip.samples.size() * sizeof(std::int16_t)));
        return out ? 0 : 1;
    }
    check(argc == 3, "expected fixture and asset directories");
    const std::filesystem::path fixture(argv[1]);
    auto get = [&](Action action, const char* stop) {
        return coach::load_clip(fixture, action, stop, 50);
    };
    const auto example = get(Action::welcome, "sample");
    check(example.kind == ClipKind::assembled, "welcome should assemble");
    check(example.samples.size() == 4414, "two 100ms gaps");
    check(example.samples[0] == 500 && example.samples[2206] == 1000
          && example.samples.back() == 1500, "prefix, name and suffix order and gain");
    check(get(Action::next, "sample").samples.size() == 2208, "next needs no suffix");
    check(get(Action::welcome, "missing").kind == ClipKind::generic, "unknown station fallback");
    check(get(Action::welcome, "custom").kind == ClipKind::named, "preserve whole-WAV override");
    check(get(Action::finish, "sample").kind == ClipKind::generic, "missing tail fallback");
    const auto before_terminal = coach::load_clip(fixture, Action::finish, "sample", 50, true);
    check(before_terminal.kind == ClipKind::assembled && before_terminal.samples.front() == 600
        && before_terminal.samples.back() == 1600, "terminal soon uses distinct pre-arrival audio");
    const auto generic_terminal = coach::load_clip(fixture, Action::finish, "unknown", 50, true);
    check(generic_terminal.kind == ClipKind::generic && generic_terminal.samples.front() == 1800,
          "unknown station still receives pre-arrival language");
    check(get(Action::welcome, "long").kind == ClipKind::generic, "oversized assembly fallback");
    check(get(Action::welcome, "broken").kind == ClipKind::generic, "corrupt station fallback");

    const auto repeated = get(Action::approaching, "sample");
    check(repeated.kind == ClipKind::assembled && repeated.samples.size() == 8825
          && repeated.samples[0] == 500 && repeated.samples[2206] == 1000
          && repeated.samples[4412] == 1500 && repeated.samples[6618] == 1000
          && repeated.samples.back() == 2000, "one stored station clip can play twice");

    const auto all = coach::load_broadcast(fixture, "all", Action::next, "unknown", 50);
    check(all.kind == ClipKind::multilingual, "load four languages");
    check(all.samples.size() == 29774, "chime, four clips, and three language gaps");
    check(all.samples[0] == 500 && all.samples[1] == 1000 && all.samples[9925] == 1500
          && all.samples[19849] == 2000 && all.samples.back() == 2500,
          "one chime and correct language order, including a legacy cue");
    check(coach::load_broadcast(fixture, "all", Action::welcome, "missing", 50).samples.empty(),
          "never emit an incomplete four-language sequence");
    const auto mixed = coach::load_broadcast(fixture, "all", Action::finish, "sample", 50, true);
    check(mixed.kind == ClipKind::multilingual && mixed.terminal_fallback && mixed.samples.back() == 3000,
          "if a language lacks the terminal preview use its approaching audio");
    const std::filesystem::path assets(argv[2]);
    const auto terminal = coach::load_broadcast(assets, "all", Action::finish, "dover", 100, true);
    check(terminal.kind == ClipKind::multilingual && !terminal.terminal_fallback && terminal.samples.size() > 22050
        && terminal.samples.size() < 22050 * 50, "four-language terminal-approach audio must assemble");
    for (const auto* language : {"de", "en", "zh", "ja"}) {
        for (const auto action : {Action::welcome, Action::next, Action::approaching, Action::finish}) {
            for (const auto* station : {"helsinki", "tallinn", "parnu"}) {
                const auto clip = coach::load_clip(assets / language, action, station, 100);
                check(clip.kind == ClipKind::assembled && !clip.samples.empty(),
                      "packaged station/action should assemble in every language");
                check(std::ranges::all_of(clip.samples, [](int s) {
                    return s >= -31000 && s <= 31000;
                }), "packaged clips must have headroom");
            }
            const auto generic = coach::load_clip(assets / language, action, "unknown", 100);
            check(generic.kind == ClipKind::generic && !generic.samples.empty(),
                  "packaged generic fallback missing");
        }
    }
}
