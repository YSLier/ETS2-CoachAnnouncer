#pragma once

#include "capture.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace coach {

enum class Action { welcome, next, approaching, finish };

constexpr std::array<const char*, 4> action_names{"welcome", "next", "approaching", "finish"};

enum class StopClass { unknown, line, tourist, viewpoint };

class Route {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto finish_grace = std::chrono::minutes(2);

    struct Leg {
        std::uint64_t index{};
        std::string source;
        std::string destination;
        bool departed{};
        bool terminal{};
        bool arrived{};
        StopClass stop_class{};
    };

    const std::optional<Leg>& leg() const { return leg_; }
    bool active() const { return trip_.has_value(); }
    bool before_terminal_arrival() const { return trip_ && !trip_->final_arrived; }

    void accept(const Record& record, Clock::time_point now);
    void invalidate();
    std::optional<std::string> station(Action action, Clock::time_point now) const;
    std::uint64_t revision() const { return revision_; }

private:
    struct Trip {
        std::string market;
        std::string next;
        std::string terminal;
        bool final_arrived{};

        bool operator==(const Trip&) const = default;
    };

    void set_trip(Trip trip);
    void clear_finished();

    std::optional<Trip> trip_;
    std::optional<Leg> leg_;
    std::optional<std::string> pending_terminal_;
    std::optional<std::string> completed_terminal_;
    Clock::time_point pending_at_{};
    Clock::time_point completed_at_{};
    std::uint64_t revision_{};
};

std::optional<std::uint64_t> unsigned_value(const scs_value_t& v);
bool station_id_valid(std::string_view id);
bool confirms_arrival(const Route::Leg& leg, const Record& record);

} // namespace coach
