#pragma once

#include "panel_snapshot.hpp"
#include "capture.hpp"

#include <string_view>
#include <optional>

namespace coach {
class Route;

namespace panel {
void copy_text(char* dest, std::size_t capacity, std::string_view text) noexcept;

template<std::size_t Size>
void copy_text(char (&dest)[Size], std::string_view text) noexcept {
    copy_text(dest, Size, text);
}

void accept(Snapshot& view, const Record& record);
void invalidate_route(Snapshot& view, RouteState reason);
void set_targets(Snapshot& view, const Route& route);

void publish(const Snapshot& view);
void disconnect() noexcept;
Read read(void* destination, std::uint32_t size) noexcept;
Result submit(const void* source, std::uint32_t size) noexcept;
std::optional<Command> take();
std::uint64_t new_session() noexcept;
} // namespace panel
} // namespace coach
