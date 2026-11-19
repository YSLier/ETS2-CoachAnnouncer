#include "capture.hpp"

#include <algorithm>
#include <cstring>

namespace coach {

Text Record::copy_text(const char* value) {
    if (!value) {
        return {};
    }
    const auto limit = std::min(max_string_bytes, strings.size() - used_bytes);
    std::size_t length = 0;
    while (length < limit && value[length] != '\0') {
        ++length;
    }
    const bool shortened = length == limit && value[length] != '\0';
    const Text result{static_cast<std::uint32_t>(used_bytes),
                      static_cast<std::uint32_t>(length), true, shortened};
    std::memcpy(strings.data() + used_bytes, value, length);
    used_bytes += length;
    text_limit |= shortened;
    return result;
}

std::string_view Record::view(Text value) const {
    return {strings.data() + value.offset, value.size};
}

void Record::assign(Kind next_kind, const char* next_id, const scs_named_value_t* data,
                    bool has_attributes, Frame next_frame, bool next_paused,
                    std::optional<scs_u32_t> next_minutes, std::uint64_t next_sequence,
                    std::optional<Position> next_position) {
    kind = next_kind;
    frame = next_frame;
    sequence = next_sequence;
    paused = next_paused;
    game_minutes = next_minutes;
    position = next_position;
    count = used_bytes = 0;
    missing_attributes = has_attributes && data == nullptr;
    attribute_limit = text_limit = false;
    id = copy_text(next_id);
    if (!data) {
        return;
    }
    // SCS terminates the attribute array with a null name.
    while (count < attributes.size() && data[count].name) {
        const auto& input = data[count];
        auto& output = attributes[count++];
        output = {};
        output.name = copy_text(input.name);
        output.index = input.index;
        output.value.type = input.value.type;
        if (input.value.type == SCS_VALUE_TYPE_string) {
            output.string_value = copy_text(input.value.value_string.value);
        } else if (input.value.type > SCS_VALUE_TYPE_INVALID
                   && input.value.type <= SCS_VALUE_TYPE_LAST) {
            output.value = input.value;
        }
    }
    attribute_limit = count == attributes.size() && data[count].name != nullptr;
}

void CaptureQueue::push(Kind kind, const char* id, const scs_named_value_t* data,
                        bool has_attributes, Frame frame, bool paused,
                        std::optional<scs_u32_t> game_minutes, std::optional<Position> position) noexcept {
    const auto sequence = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto write = write_.load(std::memory_order_relaxed);
    if (write - read_.load(std::memory_order_acquire) == capacity) {
        dropped_.store(sequence, std::memory_order_release);
        return;
    }
    records_[write % capacity].assign(kind, id, data, has_attributes, frame, paused,
                                      game_minutes, sequence, position);
    write_.store(write + 1, std::memory_order_release);
}

const Record* CaptureQueue::front() const noexcept {
    const auto read = read_.load(std::memory_order_relaxed);
    if (read == write_.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return &records_[read % capacity];
}

void CaptureQueue::pop() noexcept {
    read_.fetch_add(1, std::memory_order_release);
}

std::uint64_t CaptureQueue::last_sequence() const noexcept {
    return sequence_.load(std::memory_order_relaxed);
}

std::uint64_t CaptureQueue::last_dropped() const noexcept {
    return dropped_.load(std::memory_order_acquire);
}

} // namespace coach
