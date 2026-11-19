#include "automatic.hpp"
#include "station_locations.hpp"

#include <fstream>
#include <memory>
#include <stdexcept>

using namespace std::chrono_literals;
using coach::Action;
using coach::Kind;

void check(bool condition, const char* problem) {
    if (!condition) throw std::runtime_error(problem);
}

scs_named_value_t number(const char* name, scs_u32_t index, scs_u32_t n) {
    scs_named_value_t item{};
    item.name = name;
    item.index = index;
    item.value.type = SCS_VALUE_TYPE_u32;
    item.value.value_u32.value = n;
    return item;
}

scs_named_value_t text(const char* name, scs_u32_t index, const char* value) {
    scs_named_value_t item{};
    item.name = name;
    item.index = index;
    item.value.type = SCS_VALUE_TYPE_string;
    item.value.value_string.value = value;
    return item;
}

scs_named_value_t speed_or_distance(const char* name, float value) {
    scs_named_value_t item{};
    item.name = name;
    item.index = SCS_U32_NIL;
    item.value.type = SCS_VALUE_TYPE_float;
    item.value.value_float.value = value;
    return item;
}

int main(int argc, char** argv) {
    check(argc == 2, "temporary station cache directory is required");
    const std::filesystem::path directory(argv[1]);
    coach::StationLocations locations(directory);
    coach::Route route;
    coach::Automatic auto_play;
    auto record = std::make_unique<coach::Record>();
    const auto base = coach::Route::Clock::time_point{};
    int approach = 1500;
    constexpr scs_named_value_t empty[]{ {} };
    scs_named_value_t trip[]{
        text("bus_job.market", SCS_U32_NIL, "company_job"),
        number("bus.stages.count", SCS_U32_NIL, 2),
        number("bus.stages.current", SCS_U32_NIL, 0),
        text("source_stop.id", 0, "helsinki"),
        text("destination_stop.id", 0, "tallinn"),
        text("source_stop.id", 1, "tallinn"),
        text("destination_stop.id", 1, "parnu"),
        {},
        {},
        {},
        {}
    };
    const auto event = [&](Kind kind, const char* id, const scs_named_value_t* fields,
                           std::chrono::milliseconds elapsed, bool paused = false,
                           std::optional<coach::Position> position = {}) {
        record->assign(kind, id, fields, true, {}, paused, {}, 0, position);
        if (route.leg()) locations.learn(*route.leg(), *record);
        route.accept(*record, base + elapsed);
        const auto target = route.leg() ? locations.target(*route.leg()) : coach::StationTarget{};
        auto_play.observe(route, *record, base + elapsed, approach, target.position, target.from_map);
    };
    const auto config = [&](std::chrono::milliseconds elapsed) {
        event(Kind::configuration, "bus_job", trip, elapsed);
    };
    const auto sample = [&](std::chrono::milliseconds elapsed, float speed, float distance,
                            std::optional<coach::Position> position = {}, float map_scale = 1) {
        const scs_named_value_t data[]{speed_or_distance("speed_mps", speed),
                                     speed_or_distance("distance_m", distance),
                                     speed_or_distance("map_scale", map_scale), {}};
        event(Kind::navigation, "navigation", data, elapsed, false, position);
    };
    const auto ready = [&](std::chrono::milliseconds elapsed, int distance = 1500) {
        return auto_play.ready(route, base + elapsed, distance, true, true);
    };

    config(0ms);
    sample(250ms, 12, 20000);
    check(!ready(250ms), "moving to depot before boarding is not departure");
    trip[7] = number("source_stop.departure_time.actual", 0, 23860);
    config(1s);
    sample(1100ms, 12, 20000);
    sample(2s, 12, 19000);
    sample(3100ms, 12, 18000);
    check(ready(3100ms) == Action::next, "first departure announces next");
    auto_play.used(Action::next);
    check(!ready(3101ms), "next once per leg");

    for (int i = 0; i < 9; ++i) sample(4s + i*250ms, 12, 1450 - i*4);
    check(!ready(6s), "whole-route GPS is never used for an intermediate stop");
    check(!locations.lookup("tallinn"), "telemetry proximity alone does not learn");
    const auto leg = *route.leg();
    trip[8] = number("destination_stop.arrival_time.actual", 0, 24000);
    event(Kind::configuration, "bus_job", trip, 7s, false, coach::Position{5000, 6000});
    check(!ready(7s), "no approach when SDK already confirmed arrival");
    coach::StationLocations from_disk(directory);
    check(from_disk.lookup("tallinn") && from_disk.lookup("tallinn")->x == 5000,
          "learn only the confirmed station and persist its coordinates");

    // Reset trip state, then approach the learned stop while GPS still reports
    // hundreds of kilometres to the final station, as in the user's recording.
    event(Kind::gameplay, "bus_job.cancelled", empty, 8s);
    trip[8] = {};
    config(9s);
    for (int i = 0; i < 10; ++i) {
        sample(10s+i*250ms, 12, 590431, coach::Position{6510 - i*3.0, 6000});
    }
    check(!ready(12250ms, 0), "distance automation disabled at zero");
    check(ready(12250ms) == Action::approaching, "approach a learned intermediate stop, ignoring whole-route GPS");
    check(std::string_view(auto_play.distance_source()) == "station_position_scaled", "diagnostic identifies station");
    auto_play.used(Action::approaching);
    check(!ready(12251ms), "approach once per leg");

    trip[2] = number("bus.stages.current", SCS_U32_NIL, 1);
    trip[7] = number("source_stop.departure_time.actual", 1, 24090);
    trip[8] = number("destination_stop.arrival_time.actual", 0, 24000);
    event(Kind::configuration, "bus_job", trip, 13s, false, coach::Position{5005, 6000});
    sample(13250ms, 0, 12000);
    check(!ready(13250ms), "next leg waits until wheels move");
    for (int i = 0; i < 10; ++i) sample(14s+i*250ms, 8, 11000 - i*30);
    check(ready(16250ms) == Action::next, "intermediate departure announces next");
    auto_play.used(Action::next);

    sample(16500ms, 12, 1200);
    for (int i = 0; i < 6; ++i) sample(16750ms+i*250ms, 12, 1150-i*20);
    check(!ready(18s), "GPS reroute jump into the zone cannot trigger");
    sample(23s, 12, 1550);
    sample(23250ms, 12, 1490);
    sample(23500ms, 12, 1450);
    sample(23750ms, 12, 1410);
    check(ready(23750ms) == Action::finish, "terminal uses finish before actual arrival");
    check(!auto_play.ready(route, base+23750ms, 1500, true, false), "auto finish toggle respected");
    auto_play.used(Action::finish);
    check(!ready(23751ms), "terminal announcement only once");
    trip[8] = number("destination_stop.arrival_time.actual", 1, 24200);
    config(25s);
    event(Kind::configuration, "bus_job", empty, 26s, true);
    event(Kind::gameplay, "bus_job.completed", empty, 27s, true);
    event(Kind::started, "started", empty, 30s);
    check(!ready(30s), "results screen never schedules an automatic finish");
    check(route.station(Action::finish, base+30s).has_value(), "manual finish still works after completion");

    trip[8] = number("destination_stop.arrival_time.actual", 0, 24000);
    record->assign(Kind::configuration, "bus_job", trip, true, {}, false, {}, 0, coach::Position{5500, 6000});
    check(locations.learn(leg, *record) == coach::StationUpdate::ambiguous, "two different depots with the same ID");
    coach::StationLocations conflicted(directory);
    check(!conflicted.lookup("tallinn"), "ambiguous coordinates never trigger after a restart");

    {
        std::ofstream map(directory / "station-map-ets2-1.62.tsv");
        map << "# coach-stations-v1 ets2-1.62\n"
               "line\tduisburg\t-13047\t-6172\n"
               "line\tdusseldorf\t-14098\t-4573\n"
               "tourist\tduisburg\t-15000\t-6200\n"
               "viewpoint\tatbrenner\t2991\t22464\n"
               "line\tbad_position\t0\t0\n";
    }
    locations = coach::StationLocations(directory, true);
    check(locations.map_count() == 4, "invalid map rows are ignored");
    check(coach::StationLocations(directory).map_count() == 0, "map must be enabled for this game build");
    coach::Route::Leg map_leg{};
    map_leg.stop_class = coach::StopClass::tourist;
    map_leg.destination = "duisburg";
    auto target = locations.target(map_leg);
    check(target.from_map && target.position && target.position->x == -15000, "tourist boarding location");
    map_leg.destination = "dusseldorf";
    target = locations.target(map_leg);
    check(target.from_map && target.position && target.position->x == -14098, "tourist stop can use line terminal");
    map_leg.stop_class = coach::StopClass::viewpoint;
    map_leg.destination = "album.viewpoints.atbrenner";
    target = locations.target(map_leg);
    check(target.from_map && target.position && target.position->z == 22464, "SDK viewpoint prefix");
    map_leg.destination = "not_mapped";
    check(!locations.target(map_leg).position, "unknown stops have no guessed coordinates");

    event(Kind::gameplay, "bus_job.cancelled", empty, 31s);
    trip[2] = number("bus.stages.current", SCS_U32_NIL, 0);
    trip[3] = text("source_stop.id", 0, "dortmund");
    trip[4] = text("destination_stop.id", 0, "duisburg");
    trip[5] = text("source_stop.id", 1, "duisburg");
    trip[6] = text("destination_stop.id", 1, "dusseldorf");
    trip[7] = number("source_stop.departure_time.actual", 0, 31420);
    trip[8] = text("destination_stop.type", 0, "city");
    trip[9] = text("destination_stop.type", 1, "city");
    approach = 930;
    config(32s);
    check(route.leg() && !route.leg()->terminal && !locations.lookup("duisburg"), "first run to intermediate stop");
    for (int i = 0; i < 9; ++i) {
        sample(32500ms+i*250ms, 12, 590431, coach::Position{-13047 + 930 - i*3.0, -6172}, 19);
    }
    check(!ready(34500ms, approach), "a 900 world-metre highway pass is too far");
    auto_play.reset_motion();
    for (int i = 0; i < 9; ++i) {
        sample(35s+i*250ms, 12, 590431, coach::Position{-13047 + 316 - i*3.0, -6172}, 3);
    }
    check(ready(37s, approach) == Action::approaching, "mapped stop announces using scaled city estimate");
    check(auto_play.remaining() == 876, "scale 3 gives 876 game metres from 292 world metres");
    check(std::string_view(auto_play.distance_source()) == "map_station_scaled", "diagnostic identifies scaled map");
    auto_play.used(Action::approaching);
    check(!ready(37001ms, approach), "map approach once per leg");

    trip[2] = number("bus.stages.current", SCS_U32_NIL, 1);
    trip[7] = number("source_stop.departure_time.actual", 1, 31490);
    config(38s);
    for (int i = 0; i < 9; ++i) {
        sample(39s+i*250ms, 12, 5800 - i*20, coach::Position{-14098 + 900 - i*3.0, -4573}, 3);
    }
    check(ready(41s, approach) == Action::next, "only the next-stop announcement is ready with 5.8 km of road remaining");
    auto_play.used(Action::next);
    for (int i = 0; i < 12; ++i) {
        sample(43s+i*250ms, 12, 1000 - i*20, coach::Position{-14098 + 250 - i*3.0, -4573}, 3);
    }
    check(ready(45750ms, approach) == Action::finish, "terminal approaches once its GPS road distance is within range");
    check(std::string_view(auto_play.distance_source()) == "terminal_navigation", "diagnostic identifies GPS");
}
