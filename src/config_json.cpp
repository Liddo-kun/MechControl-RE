#include "config_json.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <system_error>
#include <algorithm>

namespace mech::config {
void object(const Json& j) {
    if (!j.is_object()) throw std::runtime_error("Expected object");
}
// Hand-edited profiles and settings may contain comments.
Json parse(std::string_view text) { return Json::parse(text, nullptr, true, true); }
std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read " + path.string());
    std::ostringstream text;
    text << file.rdbuf();
    if (file.bad()) throw std::runtime_error("Read failed " + path.string());
    return text.str();
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
    write_text(path, value.dump(2), atomic);
}
void write_text(const std::filesystem::path& path, std::string_view text, bool atomic) {
    auto target = path;
    if (atomic) target += L".tmp";
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
}
