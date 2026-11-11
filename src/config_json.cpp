#include "config_json.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <system_error>
#include <algorithm>
#include <chrono>

namespace mech::config {
void object(const Json& j) {
    if (!j.is_object()) throw std::runtime_error("Expected object");
}
Json parse(std::string_view text) {
    // Pinned nlohmann 3.12 accepts comments but NOT trailing commas. Remove only
    // those commas outside strings/comments, without accepting empty elements.
    std::string clean(text);
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < clean.size(); ++i) {
        char c = clean[i];
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
            continue;
        }
        if (c == '"') {
            quoted = true;
            continue;
        }
        if (c == '/' && i + 1 < clean.size() && clean[i + 1] == '/') {
            while (i < clean.size() && clean[i] != '\n' && clean[i] != '\r') clean[i++] = ' ';
            if (i == clean.size()) break;
        } else if (c == '/' && i + 1 < clean.size() && clean[i + 1] == '*') {
            auto end = clean.find("*/", i + 2);
            if (end == std::string::npos) throw std::runtime_error("Unterminated JSON comment");
            for (; i < end + 2; ++i)
                if (clean[i] != '\n' && clean[i] != '\r') clean[i] = ' ';
            --i;
        }
    }
    quoted = false;
    escaped = false;
    char previous = 0;
    int depth = 0;
    auto whitespace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    for (size_t i = 0; i < clean.size(); ++i) {
        char c = clean[i];
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"') {
                quoted = false;
                previous = '"';
            }
            continue;
        }
        if (c == '"') {
            quoted = true;
            continue;
        }
        if ((c == '{' || c == '[') && ++depth > 64) throw std::runtime_error("JSON depth exceeds 64");
        if (c == '}' || c == ']') --depth;
        if (c == ',' && previous && previous != '[' && previous != '{' && previous != ',' && previous != ':') {
            auto next = i + 1;
            while (next < clean.size() && whitespace(clean[next])) ++next;
            if (next < clean.size() && (clean[next] == ']' || clean[next] == '}')) clean[i] = ' ';
        }
        if (!whitespace(c)) previous = c;
    }
    return Json::parse(clean);
}
std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read " + path.string());
    std::ostringstream text;
    text << file.rdbuf();
    if (file.bad()) throw std::runtime_error("Read failed " + path.string());
    auto bytes = text.str();
    size_t start = 0;
    unsigned width = 0;
    bool little = true;
    auto begins = [&](std::initializer_list<unsigned char> prefix) {
        return bytes.size() >= prefix.size() &&
               std::equal(prefix.begin(), prefix.end(), reinterpret_cast<const unsigned char*>(bytes.data()));
    };
    if (begins({255, 254, 0, 0})) {
        start = 4;
        width = 4;
    } else if (begins({0, 0, 254, 255})) {
        start = 4;
        width = 4;
        little = false;
    } else if (begins({255, 254})) {
        start = 2;
        width = 2;
    } else if (begins({254, 255})) {
        start = 2;
        width = 2;
        little = false;
    } else if (begins({239, 187, 191}))
        start = 3;
    if (!width) return bytes.substr(start);
    std::wstring decoded;
    for (size_t i = start; i < bytes.size(); i += width) {
        uint32_t c = 0;
        if (i + width > bytes.size())
            c = 0xFFFD;
        else
            for (unsigned j = 0; j < width; ++j)
                c |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[i + j]))
                     << (8 * (little ? j : width - 1 - j));
        if (width == 4 && (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))) c = 0xFFFD;
        if (c > 0xFFFF) {
            c -= 0x10000;
            decoded.push_back(static_cast<wchar_t>(0xD800 + (c >> 10)));
            decoded.push_back(static_cast<wchar_t>(0xDC00 + (c & 1023)));
        } else
            decoded.push_back(static_cast<wchar_t>(c));
    }
    auto size =
        WideCharToMultiByte(CP_UTF8, 0, decoded.data(), static_cast<int>(decoded.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, decoded.data(), static_cast<int>(decoded.size()), utf8.data(), size, nullptr,
                        nullptr);
    return utf8;
}
Json read_json(const std::filesystem::path& path) { return parse(read_text(path)); }
std::filesystem::path executable_directory() {
    std::wstring name(32768, L'\0');
    auto n = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
    if (!n || n == name.size()) throw std::runtime_error("Cannot resolve executable directory");
    name.resize(n);
    return std::filesystem::path(name).parent_path();
}
void write_json(const std::filesystem::path& path, const Json& value, bool atomic) {
    auto target = path;
    if (atomic) target += L".tmp";
    const auto text = value.dump(2);
    HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE, atomic ? 0 : FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::system_error(GetLastError(), std::system_category(), "Create " + target.string());
    DWORD error = ERROR_SUCCESS;
    size_t pos = 0;
    while (pos < text.size()) {
        DWORD written = 0;
        if (!WriteFile(file, text.data() + pos, static_cast<DWORD>((std::min)(text.size() - pos, size_t{1 << 20})),
                       &written, nullptr)) {
            error = GetLastError();
            break;
        }
        if (!written) {
            error = ERROR_WRITE_FAULT;
            break;
        }
        pos += written;
    }
    if (!error && atomic && !FlushFileBuffers(file)) error = GetLastError();
    if (!CloseHandle(file) && !error) error = GetLastError();
    if (error) throw std::system_error(error, std::system_category(), "Write " + target.string());
    if (atomic && !MoveFileExW(target.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        throw std::system_error(GetLastError(), std::system_category(), "Replace " + path.string());
}
bool valid_date(std::string_view s, bool date_time_offset) {
    // ISO 8601 extended DateTime/DateTimeOffset form used by the reference serializer.
    if (s.size() < 10 || s[4] != '-' || s[7] != '-') return false;
    auto number = [&](size_t p, size_t n) {
        int v = 0;
        for (size_t i = p; i < p + n; ++i) {
            if (s[i] < '0' || s[i] > '9') return -1;
            v = v * 10 + s[i] - '0';
        }
        return v;
    };
    int year = number(0, 4), month = number(5, 2), day = number(8, 2);
    if (year < 1 || month < 1 || month > 12 || day < 1) return false;
    static constexpr int days[]{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (day > days[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) return false;
    size_t p = 10;
    int hour = 0, minute = 0, second = 0;
    if (s.size() != 10) {
        if (s.size() < 16 || s[10] != 'T' || s[13] != ':' || (hour = number(11, 2)) < 0 || hour > 23 ||
            (minute = number(14, 2)) < 0 || minute > 59)
            return false;
        p = 16;
    }
    if (p < s.size() && s[p] == ':') {
        if (s.size() < 19 || number(17, 2) < 0 || number(17, 2) > 59) return false;
        second = number(17, 2);
        p = 19;
        if (p < s.size() && s[p] == '.') {
            size_t start = ++p;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
            if (p == start || p - start > 16) return false;
        }
    }
    bool zone = p != s.size();
    int offset = 0;
    if (zone) {
        if (p == 10) return false;
        if (s[p] == 'Z') {
            if (p + 1 != s.size()) return false;
        } else if ((s[p] == '+' || s[p] == '-') && (p + 3 == s.size() || (p + 6 == s.size() && s[p + 3] == ':'))) {
            int h = number(p + 1, 2), m = p + 3 == s.size() ? 0 : number(p + 4, 2);
            if (h < 0 || h > 14 || m < 0 || m >= 60 || (h == 14 && m != 0)) return false;
            offset = (h * 60 + m) * (s[p] == '+' ? 1 : -1);
        } else
            return false;
    } else if (date_time_offset) {
        // DateTimeOffset without a zone uses the machine's local offset, unlike DateTime.
        DYNAMIC_TIME_ZONE_INFORMATION dynamic{};
        GetDynamicTimeZoneInformation(&dynamic);
        TIME_ZONE_INFORMATION tz{};
        if (!GetTimeZoneInformationForYear(static_cast<USHORT>(year), &dynamic, &tz)) return false;
        offset = -tz.Bias - tz.StandardBias;
        SYSTEMTIME local{
            static_cast<WORD>(year),   static_cast<WORD>(month),  0, static_cast<WORD>(day), static_cast<WORD>(hour),
            static_cast<WORD>(minute), static_cast<WORD>(second), 0},
            utc{};
        if (TzSpecificLocalTimeToSystemTime(&tz, &local, &utc)) {
            using namespace std::chrono;
            auto l = sys_days{std::chrono::year{year} / month / day} + hours{hour} + minutes{minute} + seconds{second};
            auto u = sys_days{std::chrono::year{utc.wYear} / utc.wMonth / utc.wDay} + hours{utc.wHour} +
                     minutes{utc.wMinute} + seconds{utc.wSecond};
            offset = static_cast<int>(duration_cast<minutes>(l - u).count());
        }
    }
    if (!zone && !date_time_offset) return true;
    // An offset is at most 14 hours, so only the first/last calendar day can
    // cross the DateTime range. Keep this check in signed minutes (year 10000
    // is outside the persisted type, not a sentinel date to construct).
    int utc_minutes = hour * 60 + minute - offset;
    return !(year == 1 && month == 1 && day == 1 && utc_minutes < 0) &&
           !(year == 9999 && month == 12 && day == 31 && utc_minutes >= 24 * 60);
}
}
