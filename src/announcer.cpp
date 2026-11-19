#include "announcer.hpp"
#include "station_locations.hpp"
#include "audio_clip.hpp"
#include "automatic.hpp"
#include "panel_status.hpp"
#include "settings.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#endif

namespace coach {
namespace {

panel::Kind panel_kind(Action action) {
    return static_cast<panel::Kind>(static_cast<std::uint32_t>(action) + 1);
}

#ifdef _WIN32
bool down(int key) {
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

bool game_foreground() {
    DWORD process = 0;
    if (const auto window = GetForegroundWindow()) {
        GetWindowThreadProcessId(window, &process);
    }
    return process == GetCurrentProcessId();
}

class Output {
public:
    ~Output() {
        stop();
        if (event_) {
            CloseHandle(event_);
        }
    }

    bool play(std::vector<std::int16_t> samples) {
        const WAVEFORMATEX format{WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0};
        if (!event_) {
            event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }
        if (!event_ || waveOutOpen(&device_, WAVE_MAPPER, &format,
                reinterpret_cast<DWORD_PTR>(event_), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
            device_ = nullptr;
            return false;
        }
        samples_ = std::move(samples);
        header_ = {};
        header_.lpData = reinterpret_cast<LPSTR>(samples_.data());
        header_.dwBufferLength = static_cast<DWORD>(samples_.size() * sizeof(std::int16_t));
        prepared_ = waveOutPrepareHeader(device_, &header_, sizeof(header_)) == MMSYSERR_NOERROR;
        ResetEvent(event_); // Exclude the notification from opening the device.
        if (prepared_ && waveOutWrite(device_, &header_, sizeof(header_)) == MMSYSERR_NOERROR) {
            expected_end_ = Route::Clock::now() + std::chrono::seconds(3)
                + std::chrono::milliseconds(samples_.size() * 1000 / 22050);
            return true;
        }
        stop();
        return false;
    }

    bool active() {
        if (device_ && ((WaitForSingleObject(event_, 0) == WAIT_OBJECT_0 && (header_.dwFlags & WHDR_DONE))
                        || Route::Clock::now() > expected_end_)) {
            stop();
        }
        return device_ != nullptr;
    }

    void stop() {
        if (device_) {
            waveOutReset(device_);
            if (prepared_) {
                waveOutUnprepareHeader(device_, &header_, sizeof(header_));
                prepared_ = false;
            }
            waveOutClose(device_);
            device_ = nullptr;
        }
        samples_.clear();
    }

private:
    HANDLE event_{};
    HWAVEOUT device_{};
    WAVEHDR header_{};
    bool prepared_{};
    std::vector<std::int16_t> samples_;
    Route::Clock::time_point expected_end_{};
};
#endif

} // namespace

struct Announcer::State {
    State(const std::filesystem::path& directory, bool use_station_map)
        : settings(read_settings(directory)), directory(directory), audio(directory / "audio"),
          locations(directory, use_station_map) {}

    struct Request { Action action; std::string station; bool automatic{}; bool preview{}; };

    Settings settings;
    std::filesystem::path directory;
    std::filesystem::path audio;
    Route route;
    std::optional<panel::Station> override_stop;
    std::uint64_t override_revision{};
    std::uint32_t stage{panel::unknown};
    Automatic automatic;
    StationLocations locations;
#ifdef _WIN32
    Output output;
#endif
    std::deque<Request> queue;
    std::array<bool, 4> held{};
    std::optional<Action> playing;
    panel::Kind last_action{};
    panel::Outcome last_result{};
    char last_target[97]{};
    bool last_automatic{};
    bool focused{};
    bool paused{true};
    bool previewing{};
    bool playing_automatic{};

    std::optional<std::string> target(Action action, Route::Clock::time_point now, bool auto_trigger = false) const {
        if (!auto_trigger && override_stop && (action == Action::next || action == Action::approaching)) {
            return std::string(override_stop->id);
        }
        return route.station(action, now);
    }

    void stop() {
        queue.clear();
#ifdef _WIN32
        output.stop();
#endif
        playing.reset();
        previewing = false;
        playing_automatic = false;
    }

    void report(Action action, panel::Outcome result, std::string_view target = {}, bool automatic = false) noexcept {
        last_action = panel_kind(action);
        last_result = result;
        panel::copy_text(last_target, target);
        last_automatic = automatic;
    }
};

Announcer::Announcer(const std::filesystem::path& directory, bool use_station_map)
    : state_(std::make_unique<State>(directory, use_station_map)) {}

std::string Announcer::status() const {
    const auto& config = state_->settings;
    return "\"language\":\"" + config.language + "\",\"volume\":" + std::to_string(config.volume)
        + ",\"auto_next\":" + (config.auto_next ? "true" : "false")
        + ",\"auto_finish\":" + (config.auto_finish ? "true" : "false")
        + ",\"auto_approach_m\":" + std::to_string(config.approach_metres)
        + ",\"station_map_points\":" + std::to_string(state_->locations.map_count())
        + ",\"invalid_config_entries\":" + std::to_string(config.invalid_entries);
}

void Announcer::panel_status(panel::Snapshot& view) const {
    const auto& state = *state_;
    auto& audio = view.audio;
    panel::copy_text(audio.language, state.settings.language);
    audio.volume = state.settings.volume;
    audio.auto_next = state.settings.auto_next;
    audio.auto_finish = state.settings.auto_finish;
    audio.approach_metres = state.settings.approach_metres;
    audio.invalid_config_entries = state.settings.invalid_entries;
#ifdef _WIN32
    audio.available = true;
#endif
    audio.queued = static_cast<std::uint32_t>(state.queue.size());
    audio.playing = state.playing ? panel_kind(*state.playing) : panel::Kind::none;
    audio.last_action = state.last_action;
    audio.last_result = state.last_result;
    audio.last_automatic = state.last_automatic;
    panel::copy_text(audio.last_target, state.last_target);
    audio.previewing = state.previewing;
    for (std::size_t i = 0; i < state.settings.keys.size(); ++i) {
        panel::copy_text(audio.bindings[i], chord_label(state.settings.keys[i]));
    }
    panel::set_targets(view, state.route);
    view.route_revision = state.override_revision;
    view.override_stop = state.override_stop.value_or(panel::Station{});
}

std::optional<std::string> Announcer::accept(const Record& record) {
    auto& state = *state_;
    const auto before = state.route.revision();
    const auto now = Route::Clock::now();
    std::optional<std::string> learning;
    bool keep_arrival_audio = false;
    if (record.kind == Kind::configuration && state.route.leg()) {
        const auto& leg = *state.route.leg();
        keep_arrival_audio = !record.paused && !state.previewing && state.playing
            && (state.playing == Action::approaching || state.playing == Action::finish)
            && leg.destination == state.last_target && confirms_arrival(leg, record);
        const auto learned = state.locations.learn(leg, record);
        if (learned != StationUpdate::unchanged) {
            const auto* status = learned == StationUpdate::learned ? "learned"
                : learned == StationUpdate::ambiguous ? "ambiguous" : "write_failed";
            learning = "\"kind\":\"station_location\",\"station_id\":\"" + leg.destination
                + "\",\"status\":\"" + status + "\"";
        }
    }
    state.route.accept(record, now);
    const auto target = state.route.leg() ? state.locations.target(*state.route.leg()) : StationTarget{};
    state.automatic.observe(state.route, record, now, state.settings.approach_metres, target.position, target.from_map);
    const auto* leg = state.route.leg() ? &*state.route.leg() : nullptr;
    const auto stage = leg ? static_cast<std::uint32_t>(leg->index) : panel::unknown;
    if (state.route.revision() != before || stage != state.stage) {
        state.override_stop.reset();
        state.stage = stage;
        ++state.override_revision;
    }
    state.paused = record.paused;
    if ((state.paused && !state.previewing) || state.route.revision() != before
        || record.kind == Kind::paused) {
        if (keep_arrival_audio) state.queue.clear();
        else state.stop();
    }
    return learning;
}

void Announcer::invalidate() {
    state_->route.invalidate();
    state_->automatic = {};
    state_->override_stop.reset();
    state_->stage = panel::unknown;
    ++state_->override_revision;
    state_->stop();
    state_->focused = false;
}

panel::Result Announcer::command(const panel::Command& request, const panel::Snapshot& view) {
    using panel::Operation;
    using panel::Result;
    auto& state = *state_;
    switch (request.operation) {
        case Operation::stop:
            if (state.playing) state.report(*state.playing, panel::Outcome::stopped);
            state.stop();
            return Result::applied;
        case Operation::preview: {
            if (request.value >= action_names.size()) return Result::invalid;
#ifdef _WIN32
            if (!game_foreground()) return Result::unavailable;
            const auto action = static_cast<Action>(request.value);
            const auto station = state.target(action, Route::Clock::now());
            state.stop();
            state.queue.push_back({action, station.value_or(""), false, true});
            return Result::applied;
#else
            return Result::unavailable;
#endif
        }
        case Operation::override_stop:
        case Operation::clear_override:
            if (request.route_revision != state.override_revision) return Result::stale;
            if (request.operation == Operation::clear_override) {
                state.override_stop.reset();
                return Result::applied;
            }
            if (!state.route.leg() || view.stage_count > panel::max_stages
                || request.value < view.stage_current || request.value >= view.stage_count) {
                return Result::invalid;
            }
            if (const auto& stop = view.stages[request.value].destination;
                station_id_valid(stop.id) && std::string_view(stop.id) == request.text) {
                state.override_stop = stop;
                return Result::applied;
            }
            return Result::invalid;
        case Operation::refresh_audio:
            return Result::applied;
        default: {
            const auto previous_distance = state.settings.approach_metres;
            const auto previous_keys = state.settings.keys;
            const auto result = update_settings(state.settings, state.directory, request);
            if (result == Result::applied) {
                if (previous_distance != state.settings.approach_metres) state.automatic.reset_motion();
                if (previous_keys != state.settings.keys) state.focused = false;
            }
            return result;
        }
    }
}

std::optional<std::string> Announcer::tick() {
#ifdef _WIN32
    auto& state = *state_;
    const bool preview = state.previewing || (!state.queue.empty() && state.queue.front().preview);
    const bool enabled = (!state.paused || preview) && game_foreground();
    if (!enabled) {
        state.automatic.reset_motion();
        state.stop();
        state.focused = false;
        return std::nullopt;
    }
    const unsigned modifiers = (down(VK_CONTROL) ? 1 : 0) | (down(VK_SHIFT) ? 2 : 0)
                               | (down(VK_MENU) ? 4 : 0);
    if (!state.output.active()) {
        state.playing.reset();
        state.previewing = false;
        state.playing_automatic = false;
    }
    const auto now = Route::Clock::now();
    std::optional<std::string> result;
    for (std::size_t i = 0; i < state.held.size(); ++i) {
        const auto chord = state.settings.keys[i];
        const bool held = chord.key && down(chord.key);
        const bool pressed = !state.paused && state.focused && held && !state.held[i] && modifiers == chord.modifiers;
        state.held[i] = held;
        if (!pressed) {
            continue;
        }
        const auto action = static_cast<Action>(i);
        if (state.playing == action) {
            state.stop();
            state.report(action, panel::Outcome::stopped);
            result = "\"action\":\"" + std::string(action_names[i]) + "\",\"status\":\"stopped\"";
        } else if (auto station = state.target(action, now); station && state.queue.size() < 2
                   && std::ranges::none_of(state.queue, [action](const auto& entry) { return entry.action == action; })) {
            state.queue.push_back({action, std::move(*station)});
        } else if (!station) {
            state.report(action, panel::Outcome::no_route);
            result = "\"action\":\"" + std::string(action_names[i]) + "\",\"status\":\"no_route\"";
        }
    }
    state.focused = true;
    if (!state.paused && state.queue.empty()
        && (!state.playing || (state.playing == Action::next && state.playing_automatic))) {
        const auto action = state.automatic.ready(state.route, now, state.settings.approach_metres,
                                                   state.settings.auto_next, state.settings.auto_finish);
        if (action && (!state.playing || *action != Action::next)) {
            if (auto station = state.route.station(*action, now)) {
                if (state.playing) state.stop();
                state.queue.push_back({*action, std::move(*station), true});
            }
        }
    }
    if (!state.playing && !state.queue.empty()) {
        auto request = std::move(state.queue.front());
        state.queue.pop_front();
        const auto action = std::string(action_names[static_cast<std::size_t>(request.action)]);
        const auto current_station = state.target(request.action, now, request.automatic);
        if (!request.preview && (!current_station || *current_station != request.station)) {
            return result;
        }
        // An observed manual playback also consumes the leg trigger.
        if (!request.preview && (request.automatic || (state.route.leg()
            && state.route.leg()->destination == request.station))) {
            state.automatic.used(request.action);
        }
        const bool before_terminal = request.action == Action::finish
            && (state.route.before_terminal_arrival() || (request.preview && !state.route.active()));
        Clip clip;
        if (state.settings.volume > 0) {
            clip = load_broadcast(state.audio, state.settings.language, request.action, request.station,
                                  state.settings.volume, before_terminal);
        }
        const char* status = "missing_audio";
        auto outcome = panel::Outcome::missing_audio;
        if (state.settings.volume == 0) {
            status = "muted";
            outcome = panel::Outcome::muted;
        } else if (!clip.samples.empty()) {
            if (state.output.play(std::move(clip.samples))) {
                state.playing = request.action;
                state.playing_automatic = request.automatic;
                state.previewing = request.preview;
                switch (clip.kind) {
                    case ClipKind::named: status = "playing_named"; outcome = panel::Outcome::playing_named; break;
                    case ClipKind::assembled: status = "playing_assembled"; outcome = panel::Outcome::playing_assembled; break;
                    case ClipKind::generic: status = "playing_generic"; outcome = panel::Outcome::playing_generic; break;
                    case ClipKind::multilingual: status = "playing_multilingual"; outcome = panel::Outcome::playing_multilingual; break;
                }
            } else {
                status = "audio_error";
                outcome = panel::Outcome::audio_error;
            }
        }
        state.report(request.action, outcome, request.station, request.automatic);
        std::string context;
        if (before_terminal) context += ",\"phase\":\"before_terminal\"";
        if (clip.terminal_fallback) context += ",\"audio_fallback\":\"approaching\"";
        if (request.automatic && (request.action == Action::approaching || request.action == Action::finish)) {
            context += ",\"distance_source\":\"" + std::string(state.automatic.distance_source()) + "\"";
            context += ",\"approach_threshold_m\":" + std::to_string(state.settings.approach_metres);
            if (const auto distance = state.automatic.remaining()) {
                context += ",\"trigger_distance_m\":" + std::to_string(static_cast<unsigned>(*distance));
            }
            if (const auto distance = state.automatic.world_remaining()) {
                context += ",\"world_straight_m\":" + std::to_string(*distance);
            }
            if (const auto scale = state.automatic.distance_scale()) {
                context += ",\"map_scale\":" + std::to_string(*scale);
            }
        }
        return "\"action\":\"" + action + "\",\"station_id\":\"" + request.station
                + "\",\"status\":\"" + status + "\",\"trigger\":\""
                + (request.preview ? "preview" : request.automatic ? "automatic" : "manual") + "\"" + context;
    }
    return result;
#else
    return {};
#endif
}

Announcer::~Announcer() = default;

} // namespace coach
