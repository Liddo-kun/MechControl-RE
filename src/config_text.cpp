#include "config_text.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <system_error>

namespace mech::config {
namespace {
WORD character_type(wchar_t c) {
    WORD type = 0;
    GetStringTypeW(CT_CTYPE1, &c, 1, &type);
    return type;
}
}
std::wstring wide(std::string_view text) {
    if (text.empty()) return {};
    int count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) throw std::system_error(GetLastError(), std::system_category(), "UTF-8 conversion");
    std::wstring out(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}
std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    int count =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count, nullptr, nullptr);
    return out;
}
std::wstring lower_invariant(std::wstring_view text) {
    // The invariant locale, so that a Turkish system still lowers "I" to "i".
    std::wstring out(text);
    if (!out.empty())
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), static_cast<int>(text.size()), out.data(),
                      static_cast<int>(out.size()), nullptr, nullptr, 0);
    return out;
}
bool equal_ignore_case(std::string_view left, std::string_view right) {
    auto a = wide(left), b = wide(right);
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}
bool blank(std::wstring_view text) {
    return std::ranges::all_of(text, [](wchar_t c) { return (character_type(c) & C1_SPACE) != 0; });
}
bool blank(std::string_view text) { return blank(wide(text)); }
bool letter_or_digit(wchar_t c) { return (character_type(c) & (C1_ALPHA | C1_DIGIT)) != 0; }
std::string hex_text(unsigned value, int digits) {
    char s[16];
    snprintf(s, sizeof s, "0x%0*X", digits, value);
    return s;
}
std::string decimal_text(double value, int places, bool trim) {
    char s[32];
    snprintf(s, sizeof s, "%.*f", places, value);
    std::string text = s;
    if (trim && text.find('.') != std::string::npos) {
        text.erase(text.find_last_not_of('0') + 1);
        if (text.back() == '.') text.pop_back();
    }
    return text;
}
}
