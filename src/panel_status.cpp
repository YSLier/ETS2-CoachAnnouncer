#include "panel_status.hpp"

#include "route.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace coach::panel {
namespace {
std::mutex snapshot_mutex;
Snapshot latest;
bool live{};
std::optional<Command> pending;
std::atomic<std::uint64_t> sessions{};

std::uint32_t number(const scs_value_t& value) {
    const auto parsed = unsigned_value(value);
    return parsed && *parsed < unknown ? static_cast<std::uint32_t>(*parsed) : unknown;
}

void select_station(Station& target, const Station& expected, const std::optional<std::string>& id) {
    target = {};
    if (id && !id->empty()) {
        copy_text(target.id, *id);
        if (*id == expected.id) {
            copy_text(target.name, expected.name);
        }
    }
}
} // namespace

void copy_text(char* dest, std::size_t capacity, std::string_view text) noexcept {
    if (!capacity) return;
    auto length = std::min(text.size(), capacity - 1);
    // Drop a partial UTF-8 character at the byte limit.
    while (length && length < text.size()
           && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) {
        --length;
    }
    if (length) {
        std::memcpy(dest, text.data(), length);
    }
    dest[length] = '\0';
}

void invalidate_route(Snapshot& view, RouteState reason) {
    view.route = reason;
    view.stage_count = unknown;
    view.stage_current = unknown;
    view.market[0] = '\0';
    view.next = {};
    view.terminal = {};
    for (auto& stage : view.stages) {
        stage = {};
    }
}

void accept(Snapshot& view, const Record& record) {
    static_assert(max_stages == max_parsed_stages);
    view.paused = record.paused;
    const auto id = record.view(record.id);
    if (record.kind == coach::Kind::timer_restart && record.frame.number > 1) {
        invalidate_route(view, RouteState::waiting);
    } else if ((record.kind == coach::Kind::gameplay &&
                (id == "bus_job.cancelled" || id == "bus_job.completed" || id == "bus_job.delivered"))
               || (record.kind == coach::Kind::configuration && (id == "job" || id == "car_job")
                   && record.count)) {
        invalidate_route(view, RouteState::none);
    } else if (record.kind == coach::Kind::configuration && id == "bus_job") {
        const bool truncated = record.missing_attributes || record.attribute_limit || record.text_limit;
        invalidate_route(view, truncated ? RouteState::incomplete : RouteState::none);
        if (truncated || !record.count) {
            return;
        }

        view.route = RouteState::live;
        for (std::size_t i = 0; i < record.count; ++i) {
            const auto& attr = record.attributes[i];
            const auto name = record.view(attr.name);
            const auto value = attr.value.type == SCS_VALUE_TYPE_string && !attr.string_value.shortened
                ? record.view(attr.string_value) : std::string_view{};
            if (attr.index == SCS_U32_NIL) {
                if (name == "bus_job.market") copy_text(view.market, value);
                if (name == "bus.stages.count") view.stage_count = number(attr.value);
                if (name == "bus.stages.current") view.stage_current = number(attr.value);
            } else if (attr.index < max_stages) {
                auto& stage = view.stages[attr.index];
                if (name == "source_stop.id") copy_text(stage.source.id, value);
                if (name == "source_stop.name") copy_text(stage.source.name, value);
                if (name == "destination_stop.id") copy_text(stage.destination.id, value);
                if (name == "destination_stop.name") copy_text(stage.destination.name, value);
            }
        }
        const auto count = std::min(view.stage_count, max_stages);
        const auto has_station = [](const Station& station) { return station.id[0] || station.name[0]; };
        if (!view.market[0] || !view.stage_count || view.stage_count > max_stages
            || view.stage_current >= view.stage_count
            || std::any_of(view.stages, view.stages + count, [&](const Stage& stage) {
                   return !has_station(stage.source) || !has_station(stage.destination);
               })) {
            view.route = RouteState::incomplete;
        }
    }
}

void set_targets(Snapshot& view, const Route& route) {
    const auto now = Route::Clock::now();
    const Station unknown_station{};
    const bool count_valid = view.stage_count > 0 && view.stage_count <= max_stages;
    const auto* terminal = count_valid ? &view.stages[view.stage_count - 1].destination : &unknown_station;
    const auto* next = count_valid && view.stage_current < view.stage_count
        ? &view.stages[view.stage_current].destination : &unknown_station;
    select_station(view.terminal, *terminal, route.station(Action::welcome, now));
    select_station(view.next, *next, route.station(Action::next, now));
}

void publish(const Snapshot& view) {
    const std::lock_guard lock(snapshot_mutex);
    latest = view;
    live = true;
}

void disconnect() noexcept {
    const std::lock_guard lock(snapshot_mutex);
    live = false;
    pending.reset();
}

std::uint64_t new_session() noexcept {
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())
        + ++sessions;
}

Result submit(const void* source, std::uint32_t size) noexcept {
    if (!source || size != sizeof(Command)) return Result::invalid;
    Command command;
    std::memcpy(&command, source, sizeof(command));
    if (!command.ticket || !std::memchr(command.text, '\0', sizeof(command.text))) return Result::invalid;
    const std::unique_lock lock(snapshot_mutex, std::try_to_lock);
    if (!lock.owns_lock() || pending) return Result::busy;
    if (!live) return Result::inactive;
    if (command.session != latest.session) return Result::stale;
    if (static_cast<std::uint32_t>(command.operation) > static_cast<std::uint32_t>(Operation::reset)) {
        return Result::invalid;
    }
    pending = command;
    return Result::accepted;
}

std::optional<Command> take() {
    const std::lock_guard lock(snapshot_mutex);
    auto command = pending;
    pending.reset();
    return command;
}

Read read(void* destination, std::uint32_t size) noexcept {
    if (!destination || size != sizeof(Snapshot)) {
        return Read::bad_size;
    }
    const std::unique_lock lock(snapshot_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return Read::busy;
    }
    if (!live) {
        return Read::inactive;
    }
    std::memcpy(destination, &latest, sizeof(latest));
    return Read::ready;
}
} // namespace coach::panel
