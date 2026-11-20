#include "vf_numbers.h"
#include "theme.h"
#include <windowsx.h>
#include <algorithm>
#include <stdexcept>

namespace mech {
namespace {
constexpr wchar_t numbersClass[] = L"MechControl.Native.VfNumbers";
SIZE extent(HDC dc, HFONT font, const std::wstring& s) {
    SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, s.c_str(), int(s.size()), &size);
    return size;
}
}

VfNumbers::VfNumbers(HINSTANCE instance, HWND parent) {
    WNDCLASSW c{};
    if (!GetClassInfoW(instance, numbersClass, &c)) {
        c.lpfnWndProc = procedure;
        c.hInstance = instance;
        c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        c.lpszClassName = numbersClass;
        RegisterClassW(&c);
    }
    window_ = CreateWindowExW(0, numbersClass, L"V/F equation numbers", WS_CHILD | WS_HSCROLL, 0, 0, 0, 0, parent,
                              nullptr, instance, this);
    if (!window_) throw std::runtime_error("Cannot create the V/F equation tables");
}
VfNumbers::~VfNumbers() {
    if (mono_) DeleteObject(mono_);
}
void VfNumbers::set(std::vector<VfTable> tables) {
    tables_ = std::move(tables);
    laid_ = -1;
    InvalidateRect(window_, nullptr, FALSE);
}
void VfNumbers::style(HFONT font, HFONT bold, UINT dpi) {
    font_ = font;
    bold_ = bold;
    dpi_ = dpi;
    if (mono_) DeleteObject(mono_);
    mono_ = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    laid_ = -1;
    InvalidateRect(window_, nullptr, FALSE);
}
int VfNumbers::height(int width) {
    layout(width);
    return content_ + (widest_ > width ? GetSystemMetricsForDpi(SM_CYHSCROLL, dpi_) : 0);
}
// Columns side by side; in each column its texts wrap and its numbers fill it, or the items follow one another.
void VfNumbers::layout(int width) {
    if (laid_ == width || !font_) return;
    laid_ = width;
    boxes_.clear();
    grid_.clear();
    shaded_.clear();
    headings_.clear();
    auto dc = GetDC(window_);
    // A number's width: its text and a little room for the highlight.
    auto wide = [&](const VfItem& item) { return extent(dc, mono_, item.text).cx + 2 * scale(3); };
    int pad = scale(4), line = extent(dc, font_, L"Ag").cy,
        number = extent(dc, mono_, L"-0.000000000e-00").cx + scale(6) + 2 * pad;
    int y = 0;
    for (auto& t : tables_) {
        if (!t.heading.empty()) {
            y += scale(8);
            headings_.push_back({{0, y, width, y + line}, t.heading});
            y += line + scale(4);
        }
        // A setting column is as wide as its widest row.
        std::vector<int> widths;
        for (char k : t.columns)
            widths.push_back(k == 'e'   ? extent(dc, font_, L"00000").cx + 2 * pad
                             : k == 'w' ? scale(230)
                             : k == 'n' ? number
                                        : scale(300));
        for (auto& row : t.rows)
            for (auto& [column, items] : row.spans)
                if (t.columns[column] == 's') {
                    int w = 2 * pad;
                    for (auto& item : items) w += item.value ? wide(item) : extent(dc, font_, item.text).cx;
                    widths[column] = std::max(widths[column], w);
                }
        std::vector<int> left{0};
        for (int w : widths) left.push_back(left.back() + w);
        for (auto& row : t.rows) {
            int bottom = y + line + 2 * pad;
            for (size_t s = 0; s < row.spans.size(); ++s) {
                auto& [column, items] = row.spans[s];
                int from = left[column], to = s + 1 < row.spans.size() ? left[row.spans[s + 1].first] : left.back();
                int x = from + pad, top = y + pad, h = 0;
                HFONT f = row.header ? bold_ : font_;
                for (auto& item : items) {
                    if (item.value) {
                        int w = wide(item);
                        boxes_.push_back({{x, top, x + w, top + line}, &item});
                        x += w;
                        h = std::max(h, line);
                    } else if (item.line) {
                        if (x > from + pad || h) top += h, h = 0;
                        RECT r{from + pad, top, to - pad, top};
                        SelectObject(dc, f);
                        DrawTextW(dc, item.text.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                        r.right = to - pad;
                        boxes_.push_back({r, &item, row.header});
                        top = r.bottom;
                        x = from + pad;
                    } else {
                        int w = extent(dc, f, item.text).cx;
                        boxes_.push_back({{x, top, x + w, top + line}, &item, row.header});
                        x += w;
                        h = std::max(h, line);
                    }
                }
                bottom = std::max(bottom, top + h + pad);
                grid_.push_back({from, y, to, 0});
            }
            for (size_t g = grid_.size() - row.spans.size(); g < grid_.size(); ++g) grid_[g].bottom = bottom;
            if (row.header) shaded_.push_back({0, y, left.back(), bottom});
            y = bottom;
        }
    }
    content_ = y + scale(2);
    widest_ = 0;
    for (auto& b : grid_) widest_ = std::max(widest_, int(b.right));
    ReleaseDC(window_, dc);
    scrolled();
}
void VfNumbers::scrolled() {
    RECT r{};
    GetClientRect(window_, &r);
    x_ = std::clamp(x_, 0, std::max(0, widest_ - int(r.right)));
    SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, widest_ - 1), UINT(std::max(0L, r.right)),
                  x_};
    SetScrollInfo(window_, SB_HORZ, &si, TRUE);
}
void VfNumbers::paint(HDC target) {
    RECT client{};
    GetClientRect(window_, &client);
    if (IsRectEmpty(&client)) return;
    layout(laid_ < 0 ? client.right : laid_);
    Buffer buffer(target, client);
    auto dc = buffer.dc;
    auto& p = palette();
    FillRect(dc, &client, brush(p.card));
    SetViewportOrgEx(dc, -x_, 0, nullptr);
    for (auto& r : shaded_) FillRect(dc, &r, brush(mix(p.card, p.border, 0.35)));
    for (auto r : grid_) {
        r.right += 1;
        r.bottom += 1;
        FrameRect(dc, &r, brush(mix(p.card, p.border, 0.8)));
    }
    SelectObject(dc, bold_);
    SetTextColor(dc, p.text);
    for (auto& [r, s] : headings_) {
        RECT t{x_, r.top, x_ + r.right, r.bottom}; // headings stay in view
        DrawTextW(dc, s.c_str(), -1, &t, DT_SINGLELINE | DT_NOPREFIX);
    }
    for (auto& b : boxes_) {
        RECT r = b.r;
        auto& item = *b.item;
        if (!item.value) {
            SelectObject(dc, b.header ? bold_ : font_);
            SetTextColor(dc, item.muted ? p.muted : p.text);
            DrawTextW(dc, item.text.c_str(), -1, &r, (item.line ? DT_WORDBREAK : DT_SINGLELINE) | DT_NOPREFIX);
            continue;
        }
        if (item.changed) FillRect(dc, &r, brush(mix(p.card, p.accent, p.dark ? 0.35 : 0.2)));
        InflateRect(&r, -scale(3), 0);
        SelectObject(dc, mono_);
        SetTextColor(dc, p.text);
        DrawTextW(dc, item.text.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    SetViewportOrgEx(dc, -client.left, -client.top, nullptr);
}
LRESULT CALLBACK VfNumbers::procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto self = reinterpret_cast<VfNumbers*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<VfNumbers*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (message == WM_NCDESTROY && self) SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    return self ? self->message(message, wp, lp) : DefWindowProcW(window, message, wp, lp);
}
LRESULT VfNumbers::message(UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(window_, &ps);
        paint(dc);
        EndPaint(window_, &ps);
        return 0;
    }
    case WM_SIZE:
        scrolled();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_HSCROLL:
    case WM_MOUSEHWHEEL: {
        SCROLLINFO si{sizeof si, SIF_ALL};
        GetScrollInfo(window_, SB_HORZ, &si);
        int target = x_;
        if (message == WM_MOUSEHWHEEL)
            target += GET_WHEEL_DELTA_WPARAM(wp) * scale(60) / WHEEL_DELTA;
        else
            switch (LOWORD(wp)) {
            case SB_LINELEFT: target -= scale(40); break;
            case SB_LINERIGHT: target += scale(40); break;
            case SB_PAGELEFT: target -= int(si.nPage); break;
            case SB_PAGERIGHT: target += int(si.nPage); break;
            case SB_THUMBTRACK: target = si.nTrackPos; break;
            case SB_LEFT: target = 0; break;
            case SB_RIGHT: target = si.nMax; break;
            }
        x_ = target;
        scrolled();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(window_, message, wp, lp);
}
}
