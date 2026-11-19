#include "journal.hpp"
#include "panel_status.hpp"

#include <common/scssdk_telemetry_common_channels.h>
#include <common/scssdk_telemetry_common_configs.h>
#include <common/scssdk_telemetry_truck_common_channels.h>
#include <eurotrucks2/scssdk_eut2.h>

#include <cstdio>
#include <cmath>
#include <memory>
#include <string_view>

#ifdef _WIN32
#if !defined(_M_X64) && !defined(__x86_64__)
#error CoachAnnouncer requires Windows x64
#endif
#define COACH_EXPORT
#else
#define COACH_EXPORT __attribute__((visibility("default")))
#endif

namespace {

struct EventRegistration {
    scs_event_t event;
    const char* name;
    bool required;
};

constexpr EventRegistration events[]{
    {SCS_TELEMETRY_EVENT_configuration, "configuration", true},
    {SCS_TELEMETRY_EVENT_gameplay, "gameplay", true},
    {SCS_TELEMETRY_EVENT_frame_start, "frame_start", false},
    {SCS_TELEMETRY_EVENT_frame_end, "frame_end", false},
    {SCS_TELEMETRY_EVENT_paused, "paused", true},
    {SCS_TELEMETRY_EVENT_started, "started", true},
};

struct NavigationChannel {
    const char* channel;
    const char* field;
    std::optional<float> value;
    bool registered{};
};

struct Session {
    coach::Journal journal;
    coach::Frame frame;
    bool paused{true};
    std::optional<scs_u32_t> game_minutes;
    scs_log_t log{};
    std::array<bool, std::size(events)> registered{};
    bool time_registered{};
    bool position_registered{};
    std::optional<coach::Position> position;
    std::uint64_t position_frame{};
    bool bus_active{};
    scs_timestamp_t last_navigation{};
    std::array<NavigationChannel, 4> navigation{{
        {SCS_TELEMETRY_TRUCK_CHANNEL_speed, "speed_mps", {}},
        {SCS_TELEMETRY_TRUCK_CHANNEL_navigation_distance, "distance_m", {}},
        {SCS_TELEMETRY_TRUCK_CHANNEL_navigation_time, "eta_s", {}},
        {SCS_TELEMETRY_CHANNEL_local_scale, "map_scale", {}},
    }};
};

// SCS owns the main-thread lifecycle. Keep DLL attach/detach free of worker operations.
Session* session = nullptr;

void capture(Session& state, coach::Kind kind, const char* id,
             const scs_named_value_t* attributes = nullptr, bool has_attributes = false) noexcept {
    const bool fresh = state.position && state.position_frame <= state.frame.number
        && state.frame.number - state.position_frame <= 2;
    state.journal.capture.push(kind, id, attributes, has_attributes, state.frame,
                               state.paused, state.game_minutes, fresh ? state.position : std::nullopt);
}

void sample_navigation(Session& state) noexcept {
    constexpr scs_timestamp_t interval = 250000; // microseconds
    const auto now = state.frame.timing.render_time;
    if (!state.bus_active || state.paused || now < state.last_navigation
        || now - state.last_navigation < interval) {
        return;
    }
    state.last_navigation = now;
    std::array<scs_named_value_t, 5> fields{};
    std::size_t count = 0;
    for (const auto& channel : state.navigation) {
        if (channel.value) {
            auto& attribute = fields[count++];
            attribute.name = channel.field;
            attribute.index = SCS_U32_NIL;
            attribute.value.type = SCS_VALUE_TYPE_float;
            attribute.value.value_float.value = *channel.value;
        }
    }
    capture(state, coach::Kind::navigation, "navigation", fields.data(), true);
}

SCSAPI_VOID on_navigation(scs_string_t, scs_u32_t, const scs_value_t* value,
                           scs_context_t context) noexcept {
    auto& channel = *static_cast<NavigationChannel*>(context);
    if (value && value->type == SCS_VALUE_TYPE_float) {
        channel.value = value->value_float.value;
    } else {
        channel.value.reset();
    }
}

SCSAPI_VOID on_position(scs_string_t, scs_u32_t, const scs_value_t* value,
                        scs_context_t context) noexcept {
    auto& state = *static_cast<Session*>(context);
    state.position.reset();
    if (value && value->type == SCS_VALUE_TYPE_dplacement) {
        const auto& p = value->value_dplacement.position;
        if (std::isfinite(p.x) && std::isfinite(p.z)) {
            state.position = coach::Position{p.x, p.z};
            state.position_frame = state.frame.number;
        }
    }
}

SCSAPI_VOID on_event(scs_event_t event, const void* info, scs_context_t context) noexcept {
    using coach::Kind;
    auto& state = *static_cast<Session*>(context);
    switch (event) {
        case SCS_TELEMETRY_EVENT_frame_start:
            ++state.frame.number;
            if (const auto* frame = static_cast<const scs_telemetry_frame_start_t*>(info)) {
                state.frame.timing = *frame;
                if (frame->flags & SCS_TELEMETRY_FRAME_START_FLAG_timer_restart) {
                    state.game_minutes.reset();
                    state.position.reset();
                    state.last_navigation = 0;
                    for (auto& channel : state.navigation) channel.value.reset();
                    capture(state, Kind::timer_restart, "frame_start");
                }
            }
            break;
        case SCS_TELEMETRY_EVENT_frame_end:
            sample_navigation(state);
            break;
        case SCS_TELEMETRY_EVENT_paused:
            state.paused = true;
            capture(state, Kind::paused, "paused");
            break;
        case SCS_TELEMETRY_EVENT_started:
            state.paused = false;
            capture(state, Kind::started, "started");
            break;
        case SCS_TELEMETRY_EVENT_configuration:
            if (const auto* cfg = static_cast<const scs_telemetry_configuration_t*>(info);
                cfg && cfg->id) {
                const std::string_view id(cfg->id);
                if (id == SCS_TELEMETRY_CONFIG_bus_job || id == SCS_TELEMETRY_CONFIG_job
                    || id == "car_job") {
                    if (id == SCS_TELEMETRY_CONFIG_bus_job) {
                        state.bus_active = cfg->attributes && cfg->attributes->name;
                    } else if (cfg->attributes && cfg->attributes->name) {
                        state.bus_active = false;
                    }
                    capture(state, Kind::configuration, cfg->id, cfg->attributes, true);
                }
            }
            break;
        case SCS_TELEMETRY_EVENT_gameplay:
            if (const auto* game = static_cast<const scs_telemetry_gameplay_event_t*>(info);
                game && game->id) {
                // Include new event IDs too, to diagnose names introduced by a game patch.
                capture(state, Kind::gameplay, game->id, game->attributes, true);
            }
            break;
        default:
            break;
    }
}

SCSAPI_VOID on_game_time(scs_string_t, scs_u32_t, const scs_value_t* value,
                        scs_context_t context) noexcept {
    auto& state = *static_cast<Session*>(context);
    if (value && value->type == SCS_VALUE_TYPE_u32) {
        state.game_minutes = value->value_u32.value;
    } else {
        state.game_minutes.reset();
    }
}

void report(scs_log_t log, scs_log_type_t type, const char* message) {
    if (log) {
        log(type, message);
    }
}

void rollback(const scs_telemetry_init_params_v101_t& sdk, const Session& state) {
    for (std::size_t i = 0; i < std::size(events); ++i) {
        if (state.registered[i]) {
            sdk.unregister_from_event(events[i].event);
        }
    }
    for (const auto& channel : state.navigation) {
        if (channel.registered) {
            sdk.unregister_from_channel(channel.channel, SCS_U32_NIL, SCS_VALUE_TYPE_float);
        }
    }
    if (state.time_registered) {
        sdk.unregister_from_channel(SCS_TELEMETRY_CHANNEL_game_time, SCS_U32_NIL, SCS_VALUE_TYPE_u32);
    }
    if (state.position_registered) {
        sdk.unregister_from_channel(SCS_TELEMETRY_TRUCK_CHANNEL_world_placement, SCS_U32_NIL,
                                     SCS_VALUE_TYPE_dplacement);
    }
}

} // namespace

extern "C" COACH_EXPORT coach::panel::Read coach_announcer_panel_snapshot_v2(
    void* destination, std::uint32_t size) noexcept {
    return coach::panel::read(destination, size);
}

extern "C" COACH_EXPORT coach::panel::Result coach_announcer_panel_command_v2(
    const void* source, std::uint32_t size) noexcept {
    return coach::panel::submit(source, size);
}

extern "C" COACH_EXPORT SCSAPI_RESULT scs_telemetry_init(
    scs_u32_t version, const scs_telemetry_init_params_t* params) {
    if (version != SCS_TELEMETRY_VERSION_1_01) {
        return SCS_RESULT_unsupported;
    }
    if (!params || session) {
        return SCS_RESULT_invalid_parameter;
    }
    const auto& sdk = *static_cast<const scs_telemetry_init_params_v101_t*>(params);
    if (!sdk.common.game_id || std::string_view(sdk.common.game_id) != SCS_GAME_ID_EUT2) {
        return SCS_RESULT_unsupported;
    }
    if (!sdk.register_for_event || !sdk.unregister_from_event) {
        return SCS_RESULT_invalid_parameter;
    }

    std::unique_ptr<Session> state;
    try {
        state = std::make_unique<Session>();
        state->log = sdk.common.log;
        if (!state->journal.open(sdk.common.game_name, version, sdk.common.game_version)) {
            report(state->log, SCS_LOG_TYPE_error, "[CoachAnnouncer] Cannot open diagnostic log.");
            return SCS_RESULT_generic_error;
        }
        for (std::size_t i = 0; i < std::size(events); ++i) {
            const auto& event = events[i];
            const auto result = sdk.register_for_event(event.event, on_event, state.get());
            state->registered[i] = result == SCS_RESULT_ok;
            state->journal.note_registration(event.name, result);
            if (result != SCS_RESULT_ok && event.required) {
                rollback(sdk, *state);
                report(state->log, SCS_LOG_TYPE_error, "[CoachAnnouncer] Required SDK event registration failed.");
                return SCS_RESULT_generic_error;
            }
        }
        scs_result_t time_result = SCS_RESULT_unsupported;
        if (sdk.register_for_channel && sdk.unregister_from_channel) {
            time_result = sdk.register_for_channel(SCS_TELEMETRY_CHANNEL_game_time, SCS_U32_NIL,
                         SCS_VALUE_TYPE_u32, SCS_TELEMETRY_CHANNEL_FLAG_no_value
                             | SCS_TELEMETRY_CHANNEL_FLAG_each_frame,
                         on_game_time, state.get());
        }
        state->time_registered = time_result == SCS_RESULT_ok;
        state->journal.note_registration("game.time", time_result);
        for (auto& channel : state->navigation) {
            scs_result_t result = SCS_RESULT_unsupported;
            if (sdk.register_for_channel && sdk.unregister_from_channel) {
                result = sdk.register_for_channel(channel.channel, SCS_U32_NIL, SCS_VALUE_TYPE_float,
                    SCS_TELEMETRY_CHANNEL_FLAG_no_value | SCS_TELEMETRY_CHANNEL_FLAG_each_frame,
                    on_navigation, &channel);
                channel.registered = result == SCS_RESULT_ok;
            }
            state->journal.note_registration(channel.channel, result);
        }
        scs_result_t position_result = SCS_RESULT_unsupported;
        if (sdk.register_for_channel && sdk.unregister_from_channel) {
            position_result = sdk.register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_world_placement,
                SCS_U32_NIL, SCS_VALUE_TYPE_dplacement,
                SCS_TELEMETRY_CHANNEL_FLAG_no_value | SCS_TELEMETRY_CHANNEL_FLAG_each_frame,
                on_position, state.get());
        }
        state->position_registered = position_result == SCS_RESULT_ok;
        state->journal.note_registration(SCS_TELEMETRY_TRUCK_CHANNEL_world_placement, position_result);
        state->journal.start();
        session = state.release();
        report(session->log, SCS_LOG_TYPE_message,
               "[CoachAnnouncer] " COACH_PROBE_VERSION " loaded. Logs: %LOCALAPPDATA%/CoachAnnouncer/logs");
        return SCS_RESULT_ok;
    } catch (...) {
        if (state) {
            rollback(sdk, *state);
        }
        report(sdk.common.log, SCS_LOG_TYPE_error, "[CoachAnnouncer] Initialization failed. Check LocalAppData permissions.");
        return SCS_RESULT_generic_error;
    }
}

extern "C" COACH_EXPORT SCSAPI_VOID scs_telemetry_shutdown() {
    std::unique_ptr<Session> state(session);
    session = nullptr;
    if (state && !state->journal.close()) {
        report(state->log, SCS_LOG_TYPE_warning, "[CoachAnnouncer] Diagnostic log could not be fully written.");
    }
    // The SDK removes registrations at the end of this callback.
}
