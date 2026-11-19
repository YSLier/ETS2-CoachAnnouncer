#include "route.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>

using coach::Action;
using coach::Kind;
using coach::Route;

namespace {
scs_named_value_t num(const char* name, scs_u32_t index, scs_u32_t value) {
    scs_named_value_t result{};
    result.name = name;
    result.index = index;
    result.value.type = SCS_VALUE_TYPE_u32;
    result.value.value_u32.value = value;
    return result;
}

scs_named_value_t text(const char* name, scs_u32_t index, const char* value) {
    scs_named_value_t result{};
    result.name = name;
    result.index = index;
    result.value.type = SCS_VALUE_TYPE_string;
    result.value.value_string.value = value;
    return result;
}

void require(bool pass, const char* message) {
    if (!pass) {
        throw std::runtime_error(message);
    }
}
} // namespace

int main() {
    using namespace std::chrono_literals;
    try {
        Route route;
        const Route::Clock::time_point now{};
        auto record = std::make_unique<coach::Record>();
        const auto send = [&](Kind kind, const char* id, const scs_named_value_t* fields,
                              unsigned frame, std::chrono::seconds delay) {
            record->assign(kind, id, fields, false, {frame, {}}, false, {}, 0);
            route.accept(*record, now + delay);
        };
        const auto config = [&](const scs_named_value_t* fields) {
            send(Kind::configuration, "bus_job", fields, 0, 0s);
        };
        const auto at = [&](Action action, std::chrono::seconds delay) {
            return route.station(action, now + delay);
        };

        scs_named_value_t trip[]{
            text("bus_job.market", SCS_U32_NIL, "company_job"),
            num("bus.stages.count", SCS_U32_NIL, 2),
            num("bus.stages.current", SCS_U32_NIL, 0),
            text("source_stop.id", 0, "helsinki"),
            text("destination_stop.id", 0, "tallinn"),
            text("destination_stop.id", 1, "parnu"),
            {},
            {},
            {}
        };
        const scs_named_value_t empty[]{ {} };
        config(empty);
        require(!at(Action::welcome, 0s), "no bus job");
        config(trip);
        send(Kind::timer_restart, "frame_start", nullptr, 1, 0s);
        require(at(Action::welcome, 0s) == "parnu", "terminal from first update");
        require(at(Action::next, 0s) == "tallinn", "Tallinn before intermediate arrival");
        auto revision = route.revision();
        config(trip);
        require(revision == route.revision(), "duplicate must not interrupt");
        trip[2] = num("bus.stages.current", SCS_U32_NIL, 1);
        trip[6] = num("destination_stop.arrival_time.actual", 0, 24040);
        config(trip);
        require(at(Action::next, 0s) == "parnu" && at(Action::approaching, 0s) == "parnu",
                "both actions use Parnu after Tallinn");
        trip[7] = num("destination_stop.arrival_time.actual", 1, 24200);
        config(trip);
        require(!at(Action::approaching, 0s) && at(Action::finish, 0s) == "parnu",
                "only destination actions remain after terminal arrival");
        config(empty);
        require(!at(Action::finish, 0s), "wait for completion confirmation");
        send(Kind::gameplay, "bus_job.completed", empty, 0, 1s);
        require(at(Action::finish, 119s) == "parnu" && !at(Action::finish, 121s),
                "confirmed finish grace expires");
        require(!at(Action::next, 1s), "completed trip only permits finish");

        config(trip);
        send(Kind::gameplay, "bus_job.completed", empty, 0, 0s);
        config(empty);
        require(at(Action::finish, 0s) == "parnu", "opposite completion ordering");
        send(Kind::gameplay, "bus_job.cancelled", empty, 0, 0s);
        require(!at(Action::finish, 0s), "cancellation clears finish");
        config(trip);
        config(empty);
        send(Kind::gameplay, "bus_job.completed", empty, 0, 6s);
        require(!at(Action::finish, 6s), "old empty config cannot claim a later completion");

        trip[7] = {};
        trip[0] = text("bus_job.market", SCS_U32_NIL, "tourist_job");
        trip[5] = text("destination_stop.id", 1, "../invalid");
        config(trip);
        const auto fallback = at(Action::next, 0s);
        require(fallback && fallback->empty(), "unsafe station ID should use generic audio");
        send(Kind::configuration, "job", empty, 0, 0s);
        require(at(Action::finish, 0s).has_value(), "empty freight config preserves bus trip");
        send(Kind::configuration, "job", trip, 0, 0s);
        require(!at(Action::finish, 0s), "active freight config clears trip");
        config(trip);
        send(Kind::timer_restart, "frame_start", nullptr, 2, 0s);
        require(!at(Action::finish, 0s), "reset clears trip");
        std::cout << "PASS: real-route field sequence, terminal grace, cancellation, freight, reset and fallback\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
