#include "station_locations.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

namespace coach {
namespace {
constexpr std::uintmax_t max_file_size = 512 * 1024;
constexpr std::size_t max_stations = 4096;

bool valid(Position point) {
    return std::isfinite(point.x) && std::isfinite(point.z)
        && (point.x != 0 || point.z != 0)
        && std::abs(point.x) < 10'000'000 && std::abs(point.z) < 10'000'000;
}

bool bounded_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::file_size(path, error) <= max_file_size && !error;
}
} // namespace

StationLocations::StationLocations(std::filesystem::path directory, bool use_map) {
    if (directory.empty()) return;
    const auto map_file = directory / "station-map-ets2-1.62.tsv";
    if (use_map && bounded_file(map_file)) {
        std::ifstream input(map_file);
        std::string line;
        if (std::getline(input, line) && line == "# coach-stations-v1 ets2-1.62") {
            while (map_.size() < max_stations && std::getline(input, line)) {
                if (line.starts_with('#')) continue;
                std::istringstream row(line);
                row.imbue(std::locale::classic());
                std::string kind, id, extra;
                Position point{};
                if (!(row >> kind >> id >> point.x >> point.z) || (row >> extra)
                    || !station_id_valid(id) || !valid(point)) continue;
                const auto type = kind == "line" ? StopClass::line
                    : kind == "tourist" ? StopClass::tourist
                    : kind == "viewpoint" ? StopClass::viewpoint : StopClass::unknown;
                if (type != StopClass::unknown) map_.try_emplace({type, id}, point);
            }
        }
    }
    file_ = directory / "station-locations-v1.tsv";
    if (!bounded_file(file_)) return;
    std::ifstream input(file_);
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        row.imbue(std::locale::classic());
        std::string id;
        std::string type;
        if (!(row >> id >> type) || !station_id_valid(id)) continue;
        if (locations_.size() >= max_stations && !locations_.contains(id)) continue;
        if (type == "ambiguous") {
            locations_[id] = std::nullopt;
        } else if (type == "point") {
            Position point{};
            if (row >> point.x >> point.z && valid(point)) locations_.try_emplace(id, point);
        }
    }
}

const Position* StationLocations::lookup(std::string_view id) const {
    const auto found = locations_.find(id);
    if (found == locations_.end() || !found->second) return nullptr;
    return &*found->second;
}

const Position* StationLocations::map_position(const Route::Leg& leg) const {
    if (leg.stop_class == StopClass::unknown || map_.empty()) return nullptr;
    std::string_view id = leg.destination;
    constexpr std::string_view prefix = "album.viewpoints.";
    if (leg.stop_class == StopClass::viewpoint && id.starts_with(prefix)) id.remove_prefix(prefix.size());
    const auto find = [&](StopClass type) -> const Position* {
        const auto it = map_.find({type, std::string(id)});
        return it == map_.end() ? nullptr : &it->second;
    };
    if (const auto* stop = find(leg.stop_class)) return stop;
    // A tourist city can use the regular terminal if no separate stop exists.
    return leg.stop_class == StopClass::tourist ? find(StopClass::line) : nullptr;
}

StationTarget StationLocations::target(const Route::Leg& leg) const {
    if (const auto* stop = map_position(leg)) return {stop, true};
    return {lookup(leg.destination), false};
}

StationUpdate StationLocations::learn(const Route::Leg& leg, const Record& record) {
    if (!record.position || !valid(*record.position) || !confirms_arrival(leg, record)) {
        return StationUpdate::unchanged;
    }
    if (map_position(leg)) return StationUpdate::unchanged;
    const auto found = locations_.find(leg.destination);
    const auto& position = *record.position;
    const bool conflict = found != locations_.end();
    if (conflict) {
        if (!found->second || std::hypot(found->second->x - position.x, found->second->z - position.z) < 180) {
            return StationUpdate::unchanged;
        }
        found->second.reset();
    } else {
        if (locations_.size() >= max_stations) return StationUpdate::unchanged;
        locations_.emplace(leg.destination, position);
    }
    if (!file_.empty()) {
        std::error_code error;
        const auto size = std::filesystem::file_size(file_, error);
        if ((!error && size >= max_file_size - 160)
            || (error && error != std::errc::no_such_file_or_directory)) return StationUpdate::write_failed;
        std::ofstream output(file_, std::ios::app);
        output.imbue(std::locale::classic());
        // Begin with a newline so a partial previous write cannot swallow this entry.
        output << '\n' << leg.destination << '\t';
        if (conflict) output << "ambiguous\n";
        else output << "point\t" << std::setprecision(15) << position.x << '\t' << position.z << '\n';
        output.close();
        if (!output) return StationUpdate::write_failed;
    }
    return conflict ? StationUpdate::ambiguous : StationUpdate::learned;
}

} // namespace coach
