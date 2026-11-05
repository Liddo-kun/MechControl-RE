#pragma once
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>

namespace mech {
class Log {
public:
    explicit Log(const std::filesystem::path& path);
    ~Log();
    void write(std::string_view message);
    void crash(std::string_view message); // Unhandled-exception reports; never waits on a held lock.
private:
    std::mutex mutex_;
    std::ofstream file_;
};
}
