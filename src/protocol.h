#pragma once
#include "win_handle.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mech::cli {
inline constexpr std::int32_t version = 1;
inline constexpr std::uint32_t maxRequestBytes = 65536;
struct Reply {
    int code;
    std::string text;
};
using Deadline = std::chrono::steady_clock::time_point;
std::array<std::uint8_t, 8> header(std::int32_t first, std::uint32_t length);
std::int32_t integer(const std::uint8_t* bytes);
std::vector<std::uint8_t> request(const std::vector<std::string>& args);
void transfer(HANDLE pipe, std::span<std::uint8_t> bytes, bool writing, Deadline deadline);
Reply exchange(const std::vector<std::string>& args, const std::wstring& pipeName = L"MechControl");
}
