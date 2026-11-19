#pragma once

#include <scssdk_telemetry.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>

namespace coach {

constexpr std::size_t max_attributes = 4096;
constexpr std::size_t max_string_bytes = 4096;
constexpr std::size_t string_capacity = 256 * 1024;
constexpr scs_u32_t max_parsed_stages = 128;

struct Text {
    std::uint32_t offset{};
    std::uint32_t size{};
    bool present{};
    bool shortened{};
};

struct Attribute {
    Text name;
    Text string_value;
    scs_u32_t index;
    scs_value_t value;
};

enum class Kind { configuration, gameplay, paused, started, timer_restart, navigation };

struct Frame {
    std::uint64_t number{};
    scs_telemetry_frame_start_t timing{};
};

struct Position {
    double x;
    double z;
};

struct Record {
    std::uint64_t sequence{};
    Kind kind{};
    Text id;
    Frame frame;
    bool paused{};
    std::optional<scs_u32_t> game_minutes;
    std::optional<Position> position;
    bool missing_attributes{};
    bool attribute_limit{};
    bool text_limit{};
    std::size_t count{};
    std::size_t used_bytes{};
    std::array<Attribute, max_attributes> attributes;
    std::array<char, string_capacity> strings;

    Text copy_text(const char* value);
    std::string_view view(Text value) const;
    void assign(Kind kind, const char* id, const scs_named_value_t* data,
                bool has_attributes, Frame frame, bool paused,
                std::optional<scs_u32_t> game_minutes, std::uint64_t sequence,
                std::optional<Position> position = {});
};

// SDK dispatches from a single game thread. The logger is the only consumer.
class CaptureQueue {
public:
    void push(Kind kind, const char* id, const scs_named_value_t* data,
              bool has_attributes, Frame frame, bool paused,
              std::optional<scs_u32_t> game_minutes, std::optional<Position> position = {}) noexcept;
    const Record* front() const noexcept;
    void pop() noexcept;
    std::uint64_t last_sequence() const noexcept;
    std::uint64_t last_dropped() const noexcept;

private:
    static constexpr std::uint32_t capacity = 16;
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    alignas(64) std::atomic<std::uint32_t> read_{0};
    alignas(64) std::atomic<std::uint32_t> write_{0};
    std::atomic<std::uint64_t> sequence_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::array<Record, capacity> records_;
};

} // namespace coach
