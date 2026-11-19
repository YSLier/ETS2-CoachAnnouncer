#include "automatic.hpp"

#include <algorithm>
#include <cmath>

namespace coach {
namespace {

using namespace std::chrono_literals;

bool same_leg(const Route::Leg& left, const Route::Leg& right) {
    return left.index == right.index && left.source == right.source
        && left.destination == right.destination && left.terminal == right.terminal
        && left.stop_class == right.stop_class;
}

} // namespace

Navigation read_navigation(const Record& record) {
    Navigation nav;
    for (std::size_t i = 0; i < record.count; ++i) {
        const auto& attr = record.attributes[i];
        if (attr.value.type != SCS_VALUE_TYPE_float || !std::isfinite(attr.value.value_float.value)) {
            continue;
        }
        const auto name = record.view(attr.name);
        const float value = attr.value.value_float.value;
        if (name == "speed_mps") {
            nav.speed = value;
        } else if (name == "distance_m") {
            nav.distance = value;
        } else if (name == "eta_s") {
            nav.eta = value;
        } else if (name == "map_scale" && value >= 1 && value <= 30) {
            nav.map_scale = value;
        }
    }
    return nav;
}

void Automatic::reset_motion() {
    moving_since_.reset();
    last_distance_.reset();
    world_remaining_.reset();
    distance_scale_.reset();
    last_position_.reset();
    approach_samples_ = 0;
    outside_zone_ = false;
}

const char* Automatic::distance_source() const {
    switch (source_) {
        case DistanceSource::terminal_navigation: return "terminal_navigation";
        case DistanceSource::map_station: return "map_station_scaled";
        case DistanceSource::learned_station: return "station_position_scaled";
        default: return "unavailable";
    }
}

void Automatic::observe(const Route& route, const Record& record, Clock::time_point now,
                        int approach_metres, const Position* stop, bool from_map) {
    const auto id = record.view(record.id);
    const bool trip_reset = (record.kind == Kind::timer_restart && record.frame.number > 1)
        || (record.kind == Kind::gameplay && id == "bus_job.cancelled")
        || (record.kind == Kind::configuration && ((id == "job" || id == "car_job") && record.count))
        || (record.kind == Kind::configuration && id == "bus_job"
            && (record.missing_attributes || record.attribute_limit || record.text_limit));
    if (trip_reset || (route.active() && !was_active_)) *this = {};
    if (trip_reset) return;
    was_active_ = route.active();

    if (!route.leg()) {
        leg_.reset();
        reset_motion();
    } else if (!leg_ || !same_leg(*leg_, *route.leg())) {
        const bool advanced = leg_ && route.leg()->index == leg_->index + 1;
        departure_observed_ = advanced || !route.leg()->departed;
        leg_ = route.leg();
        next_sent_ = approach_sent_ = false;
        reset_motion();
    } else {
        leg_->departed = route.leg()->departed;
        leg_->arrived = route.leg()->arrived;
    }

    if (record.kind == Kind::gameplay && (id == "player.use.ferry" || id == "player.use.train")) {
        cooldown_ = now + 15s;
        reset_motion();
    }
    if (record.paused || record.kind == Kind::started || record.kind == Kind::timer_restart) {
        reset_motion();
    }
    if (record.kind != Kind::navigation || record.paused || !leg_ || !leg_->departed || leg_->arrived) return;

    const auto nav = read_navigation(record);
    const double elapsed = std::chrono::duration<double>(now - sampled_at_).count();
    std::optional<double> raw;
    if (stop && record.position) {
        raw = std::hypot(record.position->x - stop->x, record.position->z - stop->z);
    }
    const bool use_gps = leg_->terminal && nav.distance && *nav.distance > 0;
    const bool use_position = raw && !use_gps && (!leg_->terminal || !nav.distance);
    const auto source = use_gps ? DistanceSource::terminal_navigation
        : !use_position ? DistanceSource::none
        : from_map ? DistanceSource::map_station : DistanceSource::learned_station;
    // 1:19 is the normal ETS2 road scale. Use the in-game value where available,
    // including slower city scale. Suppress distance jumps at scale boundaries.
    const auto scale = use_position ? std::optional<double>(nav.map_scale.value_or(19.0f)) : std::nullopt;
    if (elapsed > 1.5 || elapsed < 0 || source != source_ || scale != distance_scale_) reset_motion();
    source_ = source;
    sampled_at_ = now;
    world_remaining_ = raw;
    distance_scale_ = scale;
    if (!nav.speed || *nav.speed < 2.5f || now < cooldown_) {
        moving_since_.reset();
        approach_samples_ = 0;
        last_distance_.reset();
        last_position_.reset();
        if (now < cooldown_) outside_zone_ = false;
        return;
    }
    if (!moving_since_) moving_since_ = now;

    std::optional<double> distance;
    if (use_gps) {
        distance = *nav.distance;
        // A GPS waypoint elsewhere should not trigger a mapped terminal.
        if (raw) distance = std::max(*distance, *raw * std::min(3.0f, nav.map_scale.value_or(3.0f)));
    } else if (use_position) {
        distance = *raw * *scale;
    }
    if (!distance || approach_metres <= 0) {
        last_distance_.reset();
        last_position_.reset();
        approach_samples_ = 0;
        outside_zone_ = false;
        return;
    }

    const double limit = approach_metres;
    const auto travelled = record.position && last_position_
        ? std::hypot(record.position->x - last_position_->x, record.position->z - last_position_->z) : 0;
    const double max_step = std::max(20.0, *nav.speed * elapsed * 2.5 + 10);
    const double nav_jump = std::max(120.0, *nav.speed * elapsed * 26 + 35);
    const bool discontinuity = travelled > max_step
        || (use_gps && last_distance_ && std::abs(*last_distance_ - *distance) > nav_jump);
    if (discontinuity) {
        reset_motion();
        cooldown_ = now + 5s;
        return;
    }
    if (*distance > limit || *distance <= 60 || !last_distance_) {
        approach_samples_ = 0;
        if (*distance > limit) outside_zone_ = true;
    } else {
        const double gain = *last_distance_ - *distance;
        // Compare projected closure with actual motion when world position is available.
        const bool inbound = use_position ? travelled >= .35 && gain >= travelled * *scale * .30
                                          : outside_zone_ && gain >= .45;
        approach_samples_ = inbound ? std::min(approach_samples_ + 1, 10u) : 0;
    }
    last_distance_ = distance;
    last_position_ = record.position;
}

std::optional<Action> Automatic::ready(const Route& route, Clock::time_point now,
                                       int approach_metres, bool next_enabled, bool finish_enabled) const {
    if (!leg_ || !leg_->departed || leg_->arrived || !moving_since_ || now < cooldown_
        || now - sampled_at_ > 1500ms) return {};
    const bool near = approach_metres > 0 && last_distance_ && *last_distance_ <= approach_metres;
    const auto arrival_action = leg_->terminal ? Action::finish : Action::approaching;
    if (near && *last_distance_ > 60 && !approach_sent_
        && approach_samples_ >= (source_ == DistanceSource::terminal_navigation ? 3u : 5u)
        && now - *moving_since_ >= 750ms && (!leg_->terminal || finish_enabled)
        && route.station(arrival_action, now)) {
        return arrival_action;
    }
    if (next_enabled && !next_sent_ && !approach_sent_ && departure_observed_
        && now - *moving_since_ >= 2s && !near) return Action::next;
    return {};
}

void Automatic::used(Action action) {
    switch (action) {
        case Action::next: next_sent_ = true; break;
        case Action::approaching:
            if (leg_ && !leg_->terminal) approach_sent_ = true;
            break;
        case Action::finish:
            if (leg_ && leg_->terminal) approach_sent_ = true;
            break;
        case Action::welcome: break;
    }
}

} // namespace coach
