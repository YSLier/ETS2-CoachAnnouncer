#pragma once

#include "route.hpp"
#include "panel_snapshot.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace coach {

class Announcer {
public:
    explicit Announcer(const std::filesystem::path& directory, bool use_station_map);
    ~Announcer();

    std::string status() const;
    std::optional<std::string> accept(const Record& record);
    void invalidate();
    void panel_status(panel::Snapshot& view) const;
    panel::Result command(const panel::Command& request, const panel::Snapshot& view);
    // All methods are called exclusively by the logging/announcement worker.
    std::optional<std::string> tick();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace coach
