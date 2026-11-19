#pragma once

#include <cstdint>
#include <type_traits>

namespace coach::panel {

// Versioned byte-copy contract between two independently loaded x64 DLLs.
inline constexpr char export_name[] = "coach_announcer_panel_snapshot_v2";
inline constexpr char command_export[] = "coach_announcer_panel_command_v2";
inline constexpr std::uint32_t max_stages = 128;
inline constexpr std::uint32_t unknown = UINT32_MAX;

enum class Read : std::uint32_t { ready = 0, busy, inactive, bad_size };
enum class RouteState : std::uint32_t { waiting, none, live, incomplete };
enum class Kind : std::uint32_t { none, welcome, next, approaching, finish };
enum class Outcome : std::uint32_t {
    none, stopped, no_route, missing_audio, muted, audio_error,
    playing_named, playing_assembled, playing_generic, playing_multilingual
};
enum class Assets : std::uint32_t { unknown, present, missing, inaccessible };
enum class Operation : std::uint32_t {
    preview, stop, language, volume, auto_next, auto_finish, approach, binding,
    override_stop, clear_override, refresh_audio, reset
};
enum class Result : std::uint32_t {
    accepted, busy, inactive, invalid, stale, applied, write_failed, unavailable
};
inline constexpr const char* languages[]{"all", "de", "en", "zh", "ja"};

struct Command {
    std::uint64_t session{};
    std::uint64_t ticket{};
    std::uint64_t route_revision{};
    Operation operation{};
    std::uint32_t value{};
    char text[97]{};
};
static_assert(std::is_trivially_copyable_v<Command> && std::is_standard_layout_v<Command>);

struct Station {
    char id[97]{};
    char name[192]{};
};

struct Stage {
    Station source;
    Station destination;
};

struct Audio {
    char language[4]{};
    std::uint32_t volume{};
    std::uint32_t auto_next{};
    std::uint32_t auto_finish{};
    std::uint32_t approach_metres{};
    std::uint32_t invalid_config_entries{};
    std::uint32_t available{};
    Assets assets{};
    std::uint32_t queued{};
    Kind playing{};
    Kind last_action{};
    Outcome last_result{};
    std::uint32_t last_automatic{};
    char last_target[97]{};
    char bindings[4][40]{};
    std::uint32_t previewing{};
    std::uint32_t station_files{};
};

struct Snapshot {
    char version[32]{};
    char log_path[1024]{};
    char audio_path[1024]{};
    std::uint64_t session{};
    std::uint64_t ticket{};
    std::uint64_t route_revision{};
    Result command_result{};
    std::uint32_t api_version{};
    std::uint32_t game_sdk_version{};
    std::uint32_t paused{1};
    RouteState route{};
    std::uint32_t stage_count{unknown};
    std::uint32_t stage_current{unknown};
    char market[64]{};
    Station next;
    Station terminal;
    Station override_stop;
    Stage stages[max_stages];
    Audio audio;
};

static_assert(std::is_trivially_copyable_v<Snapshot> && std::is_standard_layout_v<Snapshot>);
using Reader = Read (*)(void*, std::uint32_t) noexcept;
using Sender = Result (*)(const void*, std::uint32_t) noexcept;

} // namespace coach::panel
