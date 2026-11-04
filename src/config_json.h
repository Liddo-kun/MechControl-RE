#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <optional>
#include <string>
#include <limits>
#include <map>
#include <vector>
#include <type_traits>
#include <cstdint>
#include <stdexcept>

namespace mech::config {
using Json = nlohmann::json;
Json parse(std::string_view text);
std::string read_text(const std::filesystem::path& path);
Json read_json(const std::filesystem::path& path);
std::filesystem::path executable_directory();
void write_json(const std::filesystem::path& path, const Json& value, bool atomic);
void object(const Json& j);

// System.Text.Json does not coerce bool/float/string to an integer, or null to zero.
template<class T> void decode(const Json& j, T& value) {
    if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
        if (!j.is_number_integer()) throw std::runtime_error("Expected integer");
        if (j.is_number_unsigned()) {
            auto n = j.get<uint64_t>();
            if (n > static_cast<uint64_t>((std::numeric_limits<T>::max)())) throw std::runtime_error("Integer overflow");
        } else {
            auto n = j.get<int64_t>();
            if constexpr (std::is_unsigned_v<T>) {
                if (n < 0 || static_cast<uint64_t>(n) > (std::numeric_limits<T>::max)()) throw std::runtime_error("Unsigned integer overflow");
            } else if (n < (std::numeric_limits<T>::min)() || n > (std::numeric_limits<T>::max)()) throw std::runtime_error("Integer overflow");
        }
        value = j.get<T>();
    } else if constexpr (std::is_same_v<T, bool>) {
        if (!j.is_boolean()) throw std::runtime_error("Expected boolean");
        value = j.get<bool>();
    } else value = j.get<T>();
}
template<class T> void decode(const Json& j, std::optional<T>& value) {
    if (j.is_null()) value.reset();
    else { T v{}; decode(j, v); value = std::move(v); }
}
template<class T> void decode(const Json& j, std::vector<T>& value) {
    if (!j.is_array()) throw std::runtime_error("Expected array");
    value.clear();
    for (const auto& x : j) { T v{}; decode(x, v); value.push_back(std::move(v)); }
}
template<class T> void decode(const Json& j, std::map<std::string, T>& value) {
    object(j); value.clear();
    for (auto i = j.begin(); i != j.end(); ++i) { T v{}; decode(i.value(), v); value.emplace(i.key(), std::move(v)); }
}
template<class T> void field(const Json& j, const char* key, T& value) {
    if (auto i = j.find(key); i != j.end()) decode(*i, value);
}
template<class T> Json nullable(const std::optional<T>& value) { return value ? Json(*value) : Json(nullptr); }
// Date text remains text, retaining timezone/unknown values without inventing a time.
bool valid_date(std::string_view value,bool date_time_offset=false);
}
