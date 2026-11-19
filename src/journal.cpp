#include "journal.hpp"

#include "announcer.hpp"
#include "panel_status.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#endif

namespace coach {
namespace {

std::filesystem::path log_directory() {
#ifdef _WIN32
    PWSTR path = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path);
    struct Folder {
        PWSTR path;
        ~Folder() { CoTaskMemFree(path); }
    } owned_path{path};
    if (FAILED(result) || !owned_path.path) {
        throw std::runtime_error("LocalAppData not available");
    }
    return std::filesystem::path(owned_path.path) / "CoachAnnouncer" / "logs";
#else
    // Used only by the simulated host.
    const char* root = std::getenv("COACH_PROBE_TEST_DIR");
    if (!root || !*root) {
        throw std::runtime_error("COACH_PROBE_TEST_DIR is required");
    }
    return std::filesystem::path(root);
#endif
}

unsigned long process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

std::int64_t unix_micros() {
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

// Validate SDK UTF-8 and keep the file parseable even after a byte cap splits a character.
void quoted(std::ostream& out, std::string_view value) {
    out << '"';
    for (std::size_t offset = 0; offset < value.size();) {
        const auto first = static_cast<unsigned char>(value[offset]);
        if (first < 0x80) {
            if (first == '"' || first == '\\') {
                out << '\\' << static_cast<char>(first);
            } else if (first < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                out << "\\u00" << hex[first >> 4] << hex[first & 15];
            } else {
                out << static_cast<char>(first);
            }
            ++offset;
            continue;
        }
        const unsigned width = first >= 0xc2 && first <= 0xdf ? 2
                             : first >= 0xe0 && first <= 0xef ? 3
                             : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        auto codepoint = static_cast<unsigned>(first & (0x7f >> width));
        bool valid = width && width <= value.size() - offset;
        for (unsigned j = 1; valid && j < width; ++j) {
            const auto continuation = static_cast<unsigned char>(value[offset + j]);
            valid = (continuation & 0xc0) == 0x80;
            codepoint = (codepoint << 6) | (continuation & 0x3f);
        }
        constexpr unsigned minimum[]{0, 0, 0x80, 0x800, 0x10000};
        valid = valid && codepoint >= minimum[width] && codepoint <= 0x10ffff
                && (codepoint < 0xd800 || codepoint > 0xdfff);
        if (valid) {
            out << value.substr(offset, width);
            offset += width;
        } else {
            out << "\\ufffd";
            ++offset;
        }
    }
    out << '"';
}

template<class T>
void maybe_number(std::ostream& out, std::optional<T> value) {
    if (value) {
        out << *value;
    } else {
        out << "null";
    }
}

void maybe_text(std::ostream& out, const Record& record, Text text) {
    if (text.present) {
        quoted(out, record.view(text));
    } else {
        out << "null";
    }
}

void real(std::ostream& out, double value) {
    if (std::isfinite(value)) {
        out << value;
    } else {
        quoted(out, std::isnan(value) ? "NaN" : value < 0 ? "-Infinity" : "Infinity");
    }
}

template<class V>
void vector(std::ostream& out, V v) {
    out << '[';
    real(out, v.x);
    out << ',';
    real(out, v.y);
    out << ',';
    real(out, v.z);
    out << ']';
}

void euler(std::ostream& out, scs_value_euler_t v) {
    out << '[';
    real(out, v.heading);
    out << ',';
    real(out, v.pitch);
    out << ',';
    real(out, v.roll);
    out << ']';
}

template<class P>
void placement(std::ostream& out, P p) {
    out << "{\"position\":";
    vector(out, p.position);
    out << ",\"orientation\":";
    euler(out, p.orientation);
    out << '}';
}

void attribute_value(std::ostream& out, const Record& record, const Attribute& attr) {
    const auto& v = attr.value;
    switch (v.type) {
        case SCS_VALUE_TYPE_bool: out << (v.value_bool.value ? "true" : "false"); break;
        case SCS_VALUE_TYPE_s32: out << v.value_s32.value; break;
        case SCS_VALUE_TYPE_u32: out << v.value_u32.value; break;
        case SCS_VALUE_TYPE_u64: out << v.value_u64.value; break;
        case SCS_VALUE_TYPE_s64: out << v.value_s64.value; break;
        case SCS_VALUE_TYPE_float: real(out, v.value_float.value); break;
        case SCS_VALUE_TYPE_double: real(out, v.value_double.value); break;
        case SCS_VALUE_TYPE_fvector: vector(out, v.value_fvector); break;
        case SCS_VALUE_TYPE_dvector: vector(out, v.value_dvector); break;
        case SCS_VALUE_TYPE_euler: euler(out, v.value_euler); break;
        case SCS_VALUE_TYPE_fplacement: placement(out, v.value_fplacement); break;
        case SCS_VALUE_TYPE_dplacement: placement(out, v.value_dplacement); break;
        case SCS_VALUE_TYPE_string: maybe_text(out, record, attr.string_value); break;
        default: out << "null"; break;
    }
}

std::ostringstream json_stream() {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(17);
    return stream;
}

std::string raw_attributes(const Record& record) {
    auto out = json_stream();
    out << '[';
    for (std::size_t i = 0; i < record.count; ++i) {
        const auto& attr = record.attributes[i];
        if (i) {
            out << ',';
        }
        out << "{\"name\":";
        maybe_text(out, record, attr.name);
        out << ",\"index\":";
        if (attr.index == SCS_U32_NIL) {
            out << "null";
        } else {
            out << attr.index;
        }
        out << ",\"type\":" << attr.value.type << ",\"value\":";
        attribute_value(out, record, attr);
        if (attr.name.shortened || attr.string_value.shortened) {
            out << ",\"truncated\":true";
        }
        out << '}';
    }
    out << ']';
    return std::move(out).str();
}

std::string_view kind_name(Kind kind) {
    switch (kind) {
        case Kind::configuration: return "configuration";
        case Kind::gameplay: return "gameplay";
        case Kind::paused: return "paused";
        case Kind::started: return "started";
        case Kind::timer_restart: return "timer_restart";
        case Kind::navigation: return "navigation";
    }
    return "unknown";
}

using StopFields = std::map<std::string_view, const Attribute*>;

struct Stage {
    StopFields source;
    StopFields destination;
};

void write_stop(std::ostream& out, const Record& record, const StopFields& stop) {
    out << '{';
    bool comma = false;
    for (const auto& [field, attr] : stop) {
        if (comma) {
            out << ',';
        }
        comma = true;
        quoted(out, field);
        out << ':';
        attribute_value(out, record, *attr);
    }
    out << '}';
}

void bus_summary(std::ostream& out, const Record& record, bool duplicate) {
    Text market;
    std::optional<std::uint64_t> count;
    std::optional<std::uint64_t> current;
    std::map<scs_u32_t, Stage> stages;
    std::size_t skipped = 0;
    std::size_t unindexed = 0;

    for (std::size_t i = 0; i < record.count; ++i) {
        const auto& attr = record.attributes[i];
        if (attr.name.shortened) {
            continue;
        }
        const auto name = record.view(attr.name);
        if (attr.index == SCS_U32_NIL) {
            if (name == "bus_job.market" && attr.value.type == SCS_VALUE_TYPE_string) {
                market = attr.string_value;
            } else if (name == "bus.stages.count") {
                count = unsigned_value(attr.value);
            } else if (name == "bus.stages.current") {
                current = unsigned_value(attr.value);
            }
        }

        const bool source = name.starts_with("source_stop.");
        const bool destination = name.starts_with("destination_stop.");
        if (!source && !destination) {
            continue;
        }
        if (attr.index == SCS_U32_NIL) {
            ++unindexed;
            continue;
        }
        if (attr.index >= max_parsed_stages) {
            ++skipped;
            continue;
        }
        const auto field = name.substr(source ? std::string_view("source_stop.").size()
                                              : std::string_view("destination_stop.").size());
        auto& stage = stages[attr.index];
        (source ? stage.source : stage.destination).insert_or_assign(field, &attr);
    }

    out << "{\"active\":";
    if (record.missing_attributes) {
        out << "null";
    } else {
        out << (record.count ? "true" : "false");
    }
    out << ",\"duplicate\":" << (duplicate ? "true" : "false") << ",\"market\":";
    maybe_text(out, record, market);
    out << ",\"stage_count\":";
    maybe_number(out, count);
    out << ",\"stage_current\":";
    maybe_number(out, current);
    out << ",\"current_in_range\":";
    if (current && count) {
        out << (*current < *count ? "true" : "false");
    } else {
        out << "null";
    }
    out << ",\"stage_limit_exceeded\":"
        << ((count && *count > max_parsed_stages) || skipped ? "true" : "false")
        << ",\"skipped_stop_fields\":" << skipped
        << ",\"unindexed_stop_fields\":" << unindexed << ",\"stages\":[";
    bool comma = false;
    for (const auto& [index, stage] : stages) {
        if (comma) {
            out << ',';
        }
        comma = true;
        out << "{\"index\":" << index << ",\"in_declared_range\":";
        if (count) {
            out << (index < *count ? "true" : "false");
        } else {
            out << "null";
        }
        out << ",\"source\":";
        write_stop(out, record, stage.source);
        out << ",\"destination\":";
        write_stop(out, record, stage.destination);
        out << '}';
    }
    out << "]}";
}

std::string event_line(const Record& record, std::optional<std::string>& previous_bus) {
    auto out = json_stream();
    out << "{\"seq\":" << record.sequence << ",\"kind\":";
    quoted(out, kind_name(record.kind));
    out << ",\"id\":";
    maybe_text(out, record, record.id);
    out << ",\"frame\":" << record.frame.number
        << ",\"render_us\":" << record.frame.timing.render_time
        << ",\"simulation_us\":" << record.frame.timing.simulation_time
        << ",\"paused_simulation_us\":" << record.frame.timing.paused_simulation_time
        << ",\"frame_flags\":" << record.frame.timing.flags
        << ",\"paused\":" << (record.paused ? "true" : "false") << ",\"game_minutes_last_seen\":";
    maybe_number(out, record.game_minutes);

    if (record.kind == Kind::navigation) {
        out << ",\"attributes\":" << raw_attributes(record);
    }
    const auto id = record.view(record.id);
    if (record.kind == Kind::configuration || record.kind == Kind::gameplay) {
        auto raw = raw_attributes(record);
        out << ",\"attributes_missing\":" << (record.missing_attributes ? "true" : "false")
            << ",\"attribute_limit\":" << (record.attribute_limit ? "true" : "false")
            << ",\"text_limit\":" << (record.text_limit ? "true" : "false")
            << ",\"attributes\":" << raw;

        if (record.kind == Kind::configuration && id == "bus_job") {
            const bool complete = !record.missing_attributes && !record.attribute_limit && !record.text_limit;
            out << ",\"bus\":";
            bus_summary(out, record, complete && previous_bus && *previous_bus == raw);
            if (complete) {
                previous_bus = std::move(raw);
            } else {
                previous_bus.reset();
            }
        } else if (record.kind == Kind::gameplay) {
            if (id == "bus_job.completed" || id == "bus_job.delivered" || id == "bus_job.cancelled") {
                out << ",\"bus_result\":";
                quoted(out, id.substr(std::string_view("bus_job.").size()));
                previous_bus.reset();
            }
        }
    }
    if (record.kind == Kind::timer_restart) {
        previous_bus.reset();
    }
    out << '}';
    return std::move(out).str();
}

} // namespace

bool Journal::open(const char* game_name, scs_u32_t api_version, scs_u32_t game_version) {
    const auto dir = log_directory();
    data_directory_ = dir.parent_path();
    std::filesystem::create_directories(dir);
    const auto now = unix_micros();
    const auto stem = "CoachAnnouncer-" + std::to_string(now) + "-" + std::to_string(process_id());
    auto path = dir / (stem + ".jsonl");
    for (unsigned suffix = 1; std::filesystem::exists(path); ++suffix) {
        path = dir / (stem + "-" + std::to_string(suffix) + ".jsonl");
    }
    file_.open(path, std::ios::binary | std::ios::out);
    if (!file_) {
        return false;
    }
    log_path_ = path;
    api_version_ = api_version;
    game_version_ = game_version;
    game_name_ = game_name ? game_name : "";
    auto out = json_stream();
    out << "{\"kind\":\"session\",\"log_schema\":1,\"plugin_version\":\"" COACH_PROBE_VERSION
           "\",\"sdk_headers\":\"1.16\",\"game_id\":\"eut2\",\"game_name\":";
    quoted(out, game_name ? game_name : "");
    out << ",\"telemetry_api\":[" << SCS_GET_MAJOR_VERSION(api_version) << ','
        << SCS_GET_MINOR_VERSION(api_version) << "],\"game_telemetry_version\":["
        << SCS_GET_MAJOR_VERSION(game_version) << ',' << SCS_GET_MINOR_VERSION(game_version)
        << "],\"start_unix_us\":" << now << ",\"pid\":" << process_id() << '}';
    write_line(std::move(out).str());
    return !write_failed_;
}

void Journal::note_registration(const char* name, scs_result_t result) {
    auto out = json_stream();
    out << "{\"kind\":\"registration\",\"id\":";
    quoted(out, name);
    out << ",\"result\":" << result << '}';
    write_line(std::move(out).str());
}

void Journal::start() {
    worker_ = std::thread([this] {
        try {
            run();
        } catch (...) {
            write_failed_ = true;
        }
        panel::disconnect();
    });
}

void Journal::write_line(const std::string& line) {
    if (capped_ || write_failed_) {
        return;
    }
    constexpr char cap_message[] = "{\"kind\":\"log_limit\",\"limit_bytes\":33554432}\n";
    if (line.size() + 1 > file_limit - bytes_ - sizeof(cap_message)) {
        file_ << cap_message;
        capped_ = true;
    } else {
        file_ << line << '\n';
        bytes_ += line.size() + 1;
    }
    file_.flush();
    write_failed_ = !file_;
}

void Journal::run() {
    using namespace std::chrono_literals;
    std::uint64_t previous_seq = 0;
    std::uint64_t written = 0;
    std::optional<std::string> previous_bus;
    const bool use_station_map = game_name_.find(" 1.62.") != std::string::npos;
    Announcer announcer(data_directory_, use_station_map);
    panel::Snapshot view;
    view.session = panel::new_session();
    panel::copy_text(view.version, COACH_PROBE_VERSION);
    const auto path = log_path_.u8string();
    panel::copy_text(view.log_path, {reinterpret_cast<const char*>(path.data()), path.size()});
    view.api_version = api_version_;
    view.game_sdk_version = game_version_;
    const auto audio_directory = data_directory_ / "audio";
    const auto audio_path = audio_directory.u8string();
    panel::copy_text(view.audio_path, {reinterpret_cast<const char*>(audio_path.data()), audio_path.size()});
    const auto scan_audio = [&] {
        std::error_code error;
        view.audio.station_files = 0;
        if (!std::filesystem::is_directory(audio_directory, error)) {
            view.audio.assets = error && error != std::errc::no_such_file_or_directory
                ? panel::Assets::inaccessible : panel::Assets::missing;
            return;
        }
        view.audio.assets = panel::Assets::present;
        for (const auto* language : {"de", "en", "zh", "ja"}) {
            std::filesystem::directory_iterator files(audio_directory / language / "stations", error), end;
            while (!error && files != end) {
                if (files->path().extension() == ".wav" && files->is_regular_file(error)) {
                    ++view.audio.station_files;
                }
                if (!error) files.increment(error);
            }
            if (error && error != std::errc::no_such_file_or_directory) view.audio.assets = panel::Assets::inaccessible;
        }
    };
    scan_audio();
    write_line("{\"kind\":\"announcer\"," + announcer.status() + "}");
    auto next_publish = std::chrono::steady_clock::time_point{};
    std::uint64_t invalidated_through = 0;
    for (;;) {
        const bool stopping = stopping_.load(std::memory_order_acquire);
        while (const auto* record = capture.front()) {
            if (record->sequence != previous_seq + 1) {
                previous_bus.reset();
                if (record->sequence > invalidated_through) {
                    announcer.invalidate();
                    panel::invalidate_route(view, panel::RouteState::incomplete);
                }
            }
            const auto diagnostic = announcer.accept(*record);
            panel::accept(view, *record);
            if (!capped_ && !write_failed_) {
                write_line(event_line(*record, previous_bus));
                if (diagnostic) write_line("{" + *diagnostic + "}");
                ++written;
            }
            previous_seq = record->sequence;
            capture.pop();
        }
        if (stopping) {
            break;
        }
        const auto dropped = capture.last_dropped();
        if (dropped > previous_seq) {
            if (dropped > invalidated_through) {
                announcer.invalidate();
                panel::invalidate_route(view, panel::RouteState::incomplete);
                invalidated_through = dropped;
            }
        }
        if (const auto command = panel::take()) {
            view.ticket = command->ticket;
            if (command->operation == panel::Operation::refresh_audio) scan_audio();
            view.command_result = announcer.command(*command, view);
            write_line("{\"kind\":\"panel_command\",\"operation\":"
                + std::to_string(static_cast<std::uint32_t>(command->operation))
                + (command->operation == panel::Operation::approach
                    ? ",\"value\":" + std::to_string(command->value) : "")
                + ",\"result\":" + std::to_string(static_cast<std::uint32_t>(view.command_result)) + "}");
            next_publish = {};
        }
        if (dropped <= previous_seq && capture.last_sequence() == previous_seq) {
            if (const auto action = announcer.tick()) {
                write_line("{\"kind\":\"announcement\"," + *action + "}");
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_publish) {
            announcer.panel_status(view);
            panel::publish(view);
            next_publish = now + 100ms;
        }
        std::this_thread::sleep_for(10ms);
    }
    const auto attempted = capture.last_sequence();
    write_line("{\"kind\":\"shutdown\",\"last_written_seq\":" + std::to_string(previous_seq)
               + ",\"last_attempted_seq\":" + std::to_string(attempted)
               + ",\"dropped_records\":" + std::to_string(attempted - written) + "}");
}

bool Journal::close() noexcept {
    stopping_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
    if (file_.is_open()) {
        file_.close();
    }
    return !write_failed_ && !file_.fail();
}

} // namespace coach
