#pragma once

#include "route.hpp"

#include <filesystem>
#include <map>

namespace coach {

enum class StationUpdate { unchanged, learned, ambiguous, write_failed };

struct StationTarget {
    const Position* position{};
    bool from_map{};
};

class StationLocations {
public:
    explicit StationLocations(std::filesystem::path directory = {}, bool use_map = false);
    const Position* lookup(std::string_view id) const;
    StationTarget target(const Route::Leg& leg) const;
    StationUpdate learn(const Route::Leg& leg, const Record& record);
    std::size_t map_count() const { return map_.size(); }

private:
    const Position* map_position(const Route::Leg& leg) const;
    std::filesystem::path file_;
    std::map<std::string, std::optional<Position>, std::less<>> locations_;
    std::map<std::pair<StopClass, std::string>, Position> map_;
};

} // namespace coach
