#pragma once
#include <windows.h>
#include <perflib.h>
#include <cstdint>

namespace mech {
class Counters {
public:
    Counters();
    ~Counters();
    Counters(const Counters&) = delete;
    Counters& operator=(const Counters&) = delete;
    ULONG error() const { return error_; }
    void publish(std::uint64_t cpu, std::uint64_t gpu);
private:
    HANDLE provider_ = nullptr;
    PPERF_COUNTERSET_INSTANCE instance_ = nullptr;
    ULONG error_ = ERROR_SUCCESS;
};
}
