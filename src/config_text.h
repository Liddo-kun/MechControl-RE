#pragma once
#include <string>
#include <string_view>

namespace mech::config {
std::wstring wide(std::string_view text);
std::string utf8(std::wstring_view text);
std::wstring lower_invariant(std::wstring_view text);
bool equal_ignore_case(std::string_view left, std::string_view right);
bool blank(std::wstring_view text);
bool blank(std::string_view text);
bool letter_or_digit(wchar_t character);
std::string hex_text(unsigned value, int digits = 2); // 0x-prefixed, upper case, zero padded.
// Fixed decimals; trim drops trailing zeros (and then the point).
std::string decimal_text(double value, int places, bool trim = false);
}
