#include "route.hpp"

#include <algorithm>
#include <utility>

namespace coach {
namespace {

std::string_view text(const Record& record, const Attribute& attr) {
    if (attr.value.type != SCS_VALUE_TYPE_string || !attr.string_value.present
        || attr.string_value.shortened) {
        return {};
    }
    return record.view(attr.string_value);
}

} // namespace

bool station_id_valid(std::string_view text) {
    return !text.empty() && text.size() <= 96 && std::ranges::all_of(text, [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
               || ch == '_' || ch == '.' || ch == '-';
    });
}

std::optional<std::uint64_t> unsigned_value(const scs_value_t& v) {
    switch (v.type) {
        case SCS_VALUE_TYPE_u32: return v.value_u32.value;
        case SCS_VALUE_TYPE_u64: return v.value_u64.value;
        case SCS_VALUE_TYPE_s32:
            if (v.value_s32.value >= 0) {
                return static_cast<std::uint64_t>(v.value_s32.value);
            }
            break;
        case SCS_VALUE_TYPE_s64:
            if (v.value_s64.value >= 0) {
                return static_cast<std::uint64_t>(v.value_s64.value);
            }
            break;
        default: break;
    }
    return std::nullopt;
}

void Route::clear_finished() {
    pending_terminal_.reset();
    completed_terminal_.reset();
}

void Route::invalidate() {
    trip_.reset();
    leg_.reset();
    clear_finished();
    ++revision_;
}

void Route::set_trip(Trip trip) {
    if (!trip_ || *trip_ != trip) {
        trip_ = std::move(trip);
        ++revision_;
    }
}

void Route::accept(const Record& record, Clock::time_point now) {
    const auto id = record.view(record.id);
    if (record.kind == Kind::timer_restart && record.frame.number > 1) {
        invalidate();
    } else if (record.kind == Kind::configuration && id == "bus_job") {
        if (record.missing_attributes || record.attribute_limit || record.text_limit) {
            invalidate();
            return;
        }
        leg_.reset();
        if (!record.count) {
            if (trip_) {
                if (trip_->final_arrived) {
                    pending_terminal_ = trip_->terminal;
                    pending_at_ = now;
                }
                trip_.reset();
                ++revision_;
            }
            return;
        }
        clear_finished();

        Trip trip;
        std::optional<std::uint64_t> count;
        std::optional<std::uint64_t> current;
        for (std::size_t i = 0; i < record.count; ++i) {
            const auto& attr = record.attributes[i];
            if (attr.index != SCS_U32_NIL) {
                continue;
            }
            const auto name = record.view(attr.name);
            if (name == "bus_job.market") {
                trip.market = text(record, attr);
            } else if (name == "bus.stages.count") {
                count = unsigned_value(attr.value);
            } else if (name == "bus.stages.current") {
                current = unsigned_value(attr.value);
            }
        }
        if (count && *count > 0 && *count <= max_parsed_stages) {
            if (current && *current < *count) {
                leg_ = Leg{*current, {}, {}, false, *current + 1 == *count};
            }
            for (std::size_t i = 0; i < record.count; ++i) {
                const auto& attr = record.attributes[i];
                const auto name = record.view(attr.name);
                if (leg_ && attr.index == leg_->index) {
                    if (name == "source_stop.id") {
                        leg_->source = text(record, attr);
                    } else if (name == "source_stop.departure_time.actual") {
                        leg_->departed = unsigned_value(attr.value).has_value();
                    } else if (name == "destination_stop.arrival_time.actual") {
                        leg_->arrived = unsigned_value(attr.value).has_value();
                    } else if (name == "destination_stop.type") {
                        const auto type = text(record, attr);
                        if (type == "viewpoints") leg_->stop_class = StopClass::viewpoint;
                        if (type == "city" && trip.market == "company_job") leg_->stop_class = StopClass::line;
                        if (type == "city" && trip.market == "tourist_job") leg_->stop_class = StopClass::tourist;
                    }
                }
                if (name == "destination_stop.id") {
                    const auto value = text(record, attr);
                    if (station_id_valid(value)) {
                        if (attr.index == *count - 1) {
                            trip.terminal = value;
                        }
                        if (current && *current < *count && attr.index == *current) {
                            trip.next = value;
                            leg_->destination = value;
                        }
                    }
                } else if (name == "destination_stop.arrival_time.actual" && attr.index == *count - 1) {
                    trip.final_arrived = unsigned_value(attr.value).has_value();
                }
            }
        }
        if (trip.final_arrived) {
            leg_.reset();
        }
        set_trip(std::move(trip));
    } else if (record.kind == Kind::configuration && (id == "job" || id == "car_job")
               && record.count) {
        invalidate();
    } else if (record.kind == Kind::gameplay && id == "bus_job.cancelled") {
        invalidate();
    } else if (record.kind == Kind::gameplay && (id == "bus_job.completed" || id == "bus_job.delivered")) {
        // A terminal confirmation can follow the empty configuration in the same frame.
        if (trip_) {
            completed_terminal_ = trip_->terminal;
        } else if (pending_terminal_ && now - pending_at_ < std::chrono::seconds(5)) {
            completed_terminal_ = std::move(pending_terminal_);
        } else {
            return;
        }
        completed_at_ = now;
        trip_.reset();
        leg_.reset();
        pending_terminal_.reset();
        ++revision_;
    }
}

bool confirms_arrival(const Route::Leg& leg, const Record& record) {
    if (!leg.departed || leg.arrived || leg.destination.empty() || record.kind != Kind::configuration
        || record.view(record.id) != "bus_job" || record.missing_attributes
        || record.attribute_limit || record.text_limit) return false;
    bool match = false;
    bool arrived = false;
    for (std::size_t i = 0; i < record.count; ++i) {
        const auto& attr = record.attributes[i];
        if (attr.index != leg.index) continue;
        const auto name = record.view(attr.name);
        if (name == "destination_stop.id") match = text(record, attr) == leg.destination;
        if (name == "destination_stop.arrival_time.actual") arrived = unsigned_value(attr.value).has_value();
    }
    return match && arrived;
}

std::optional<std::string> Route::station(Action action, Clock::time_point now) const {
    if (trip_) {
        if (action == Action::welcome || action == Action::finish) {
            return trip_->terminal;
        }
        if (!trip_->final_arrived) {
            return trip_->next;
        }
    } else if (action == Action::finish && completed_terminal_ && now - completed_at_ < finish_grace) {
        return completed_terminal_;
    }
    return std::nullopt;
}

} // namespace coach
