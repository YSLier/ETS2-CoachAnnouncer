#pragma once

#include "capture.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace coach {

class Journal {
public:
    static constexpr std::size_t file_limit = 32 * 1024 * 1024;

    bool open(const char* game_name, scs_u32_t api_version, scs_u32_t game_version);
    void start();
    void note_registration(const char* name, scs_result_t result);
    bool close() noexcept;

    CaptureQueue capture;

private:
    void run();
    void write_line(const std::string& line);

    std::filesystem::path data_directory_;
    std::filesystem::path log_path_;
    std::ofstream file_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    std::size_t bytes_{};
    bool capped_{};
    bool write_failed_{};
    scs_u32_t api_version_{};
    scs_u32_t game_version_{};
    std::string game_name_;
};

} // namespace coach
