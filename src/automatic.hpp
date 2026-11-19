#pragma once

#include "route.hpp"

namespace coach {

struct Navigation {
    std::optional<float> speed;
    std::optional<float> distance;
    std::optional<float> eta;
    std::optional<float> map_scale;
};

Navigation read_navigation(const Record& record);

class Automatic {
public:
    using Clock = Route::Clock;

    void observe(const Route& route, const Record& record, Clock::time_point now,
                 int approach_metres, const Position* stop = nullptr, bool from_map = false);
    std::optional<Action> ready(const Route& route, Clock::time_point now,
                                int approach_metres, bool next_enabled, bool finish_enabled) const;
    void used(Action action);
    void reset_motion();
    const char* distance_source() const;
    std::optional<double> remaining() const { return last_distance_; }
    std::optional<double> world_remaining() const { return world_remaining_; }
    std::optional<double> distance_scale() const { return distance_scale_; }

private:
    enum class DistanceSource { none, terminal_navigation, map_station, learned_station };
    std::optional<Route::Leg> leg_;
    Clock::time_point cooldown_{};
    Clock::time_point sampled_at_{};
    std::optional<Clock::time_point> moving_since_;
    std::optional<double> last_distance_;
    std::optional<double> world_remaining_;
    std::optional<double> distance_scale_;
    std::optional<Position> last_position_;
    unsigned approach_samples_{};
    bool outside_zone_{};
    DistanceSource source_{};
    bool departure_observed_{};
    bool was_active_{};
    bool next_sent_{};
    bool approach_sent_{};
};

} // namespace coach
