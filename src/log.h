#pragma once
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>

namespace mech {
class Log {
public:
    explicit Log(const std::filesystem::path& path);
    void write(std::string_view message);
private:
    std::mutex mutex_;
    std::ofstream file_;
};
}
