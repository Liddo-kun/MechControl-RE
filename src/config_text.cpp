#include "config_text.h"
#include <windows.h>
#include <icu.h>
#include <cstdint>
#include <cstdio>
#include <system_error>

namespace mech::config {
namespace {
std::wstring change_case(std::wstring_view text,bool ordinal_upper) {
    std::wstring out; out.reserve(text.size());
    for(size_t i=0;i<text.size();++i) {
        int32_t c=text[i];
        if(c>=0xD800 && c<=0xDBFF && i+1<text.size() && text[i+1]>=0xDC00 && text[i+1]<=0xDFFF)
            c=0x10000+((c-0xD800)<<10)+(text[++i]-0xDC00);
        // Invariant lower retains dotted I. OrdinalIgnoreCase does not equate
        // dotless I or long S to ASCII I/S (unlike linguistic uppercasing).
        if(ordinal_upper) { if(c!=0x131 && c!=0x17F) c=u_toupper(c); }
        else if(c!=0x130) c=u_tolower(c);
        if(c>0xFFFF) { c-=0x10000; out.push_back(static_cast<wchar_t>(0xD800+(c>>10))); out.push_back(static_cast<wchar_t>(0xDC00+(c&1023))); }
        else out.push_back(static_cast<wchar_t>(c));
    }
    return out;
}
}
std::wstring wide(std::string_view text) {
    if(text.empty()) return {};
    int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(!count) throw std::system_error(GetLastError(),std::system_category(),"UTF-8 conversion");
    std::wstring out(count,L'\0'); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),out.data(),count); return out;
}
std::wstring lower_invariant(std::wstring_view text) { return change_case(text,false); }
bool equal_ignore_case(std::string_view left,std::string_view right) {
    auto a=wide(left),b=wide(right);
    return change_case(a,true)==change_case(b,true);
}
bool blank(std::wstring_view text) {
    // Char.IsWhiteSpace's Unicode White_Space set, not locale-dependent isspace.
    for(wchar_t c:text) if(!((c>=9 && c<=13) || c==0x20 || c==0x85 || c==0xA0 || c==0x1680 ||
        (c>=0x2000 && c<=0x200A) || c==0x2028 || c==0x2029 || c==0x202F || c==0x205F || c==0x3000)) return false;
    return true;
}
bool blank(std::string_view text) { return blank(wide(text)); }
bool letter_or_digit(wchar_t c) {
    // Like char.IsLetterOrDigit, classify a UTF-16 code unit, not a whole Rune.
    return u_isalnum(c)!=0;
}
std::string hex_text(unsigned value,int digits) {char s[16];snprintf(s,sizeof s,"0x%0*X",digits,value);return s;}
}
