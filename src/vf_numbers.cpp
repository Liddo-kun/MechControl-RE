#include "vf_numbers.h"
#include "theme.h"
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <stdexcept>

namespace mech {
namespace {
constexpr wchar_t numbersClass[] = L"MechControl.Native.VfNumbers";
COLORREF mix(COLORREF a, COLORREF b, double x) {
    auto c = [x](int p, int q) { return int(p + (q - p) * x + 0.5); };
    return RGB(c(GetRValue(a), GetRValue(b)), c(GetGValue(a), GetGValue(b)), c(GetBValue(a), GetBValue(b)));
}
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
    window_ = CreateWindowExW(0, numbersClass, L"V/F equation numbers", WS_CHILD | WS_HSCROLL | WS_CLIPCHILDREN, 0, 0,
                              0, 0, parent, nullptr, instance, this);
    if (!window_) throw std::runtime_error("Cannot create the V/F equation tables");
    edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 0, 0, window_, nullptr,
                            instance, nullptr);
    styleFrame(edit_);
    SetWindowSubclass(edit_, editProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
}
VfNumbers::~VfNumbers() {
    if (mono_) DeleteObject(mono_);
}
void VfNumbers::set(std::vector<VfTable> tables, std::vector<VfCell> cells) {
    close(false);
    tables_ = std::move(tables);
    cells_ = std::move(cells);
    laid_ = -1;
    InvalidateRect(window_, nullptr, FALSE);
}
void VfNumbers::cells(std::vector<VfCell> cells) {
    cells_ = std::move(cells);
    InvalidateRect(window_, nullptr, FALSE);
}
void VfNumbers::style(HFONT font, HFONT bold, UINT dpi) {
    font_ = font;
    bold_ = bold;
    dpi_ = dpi;
    if (mono_) DeleteObject(mono_);
    mono_ = CreateFontW(-MulDiv(8, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    SendMessageW(edit_, WM_SETFONT, WPARAM(mono_), FALSE);
    laid_ = -1;
    InvalidateRect(window_, nullptr, FALSE);
}
int VfNumbers::height(int width) {
    layout(width);
    int widest = 0;
    for (auto& b : grid_) widest = std::max(widest, int(b.right));
    return content_ + (widest > width ? GetSystemMetricsForDpi(SM_CYHSCROLL, dpi_) : 0);
}
// Columns side by side; in each column its texts wrap and its cells fill it, or the items follow one another.
void VfNumbers::layout(int width) {
    if (laid_ == width || !font_) return;
    laid_ = width;
    boxes_.clear();
    grid_.clear();
    shaded_.clear();
    headings_.clear();
    auto dc = GetDC(window_);
    int pad = scale(4), line = extent(dc, font_, L"Ag").cy, field = extent(dc, mono_, L"0").cy + scale(6),
        number = extent(dc, mono_, L"-0.000000000e-00").cx + scale(12);
    auto cellWidth = [&](int cell) {
        auto& c = cells_[cell];
        if (c.choices.empty()) return number;
        int w = 0;
        for (auto& s : c.choices) w = std::max(w, int(extent(dc, mono_, s + L" ▾").cx));
        return w + scale(12);
    };
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
                    for (auto& item : items)
                        w += item.cell >= 0 ? cellWidth(item.cell) + pad : extent(dc, font_, item.text).cx;
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
                for (auto& item : items) {
                    HFONT f = row.header ? bold_ : font_;
                    if (item.cell >= 0) {
                        bool alone = items.size() == 1;
                        int w = alone ? to - from - 2 * pad : cellWidth(item.cell);
                        boxes_.push_back(
                            {{x, top + (alone ? 0 : (line - field) / 2), x + w, top + field}, item.cell, &item, false});
                        x += w + pad;
                        h = std::max(h, field);
                    } else if (item.line) {
                        if (x > from + pad || h) top += h, h = 0;
                        RECT r{from + pad, top, to - pad, top};
                        SelectObject(dc, f);
                        DrawTextW(dc, item.text.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                        r.right = to - pad;
                        boxes_.push_back({r, -1, &item, row.header});
                        top = r.bottom;
                        x = from + pad;
                    } else {
                        int w = extent(dc, f, item.text).cx;
                        boxes_.push_back({{x, top, x + w, top + line}, -1, &item, row.header});
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
    ReleaseDC(window_, dc);
    scrolled();
}
void VfNumbers::scrolled() {
    RECT r{};
    GetClientRect(window_, &r);
    int widest = 0;
    for (auto& b : grid_) widest = std::max(widest, int(b.right));
    SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, widest - 1), UINT(std::max(0L, r.right)), 0,
                  0};
    x_ = std::clamp(x_, 0, std::max(0, widest - int(r.right)));
    si.nPos = x_;
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
        if (b.cell < 0) {
            SelectObject(dc, b.header ? bold_ : font_);
            SetTextColor(dc, b.item->muted ? p.muted : p.text);
            DrawTextW(dc, b.item->text.c_str(), -1, &r, (b.item->line ? DT_WORDBREAK : DT_SINGLELINE) | DT_NOPREFIX);
            continue;
        }
        auto& c = cells_[b.cell];
        COLORREF back = c.changed ? mix(p.field, p.accent, p.dark ? 0.3 : 0.14) : p.field;
        FillRect(dc, &r, brush(back));
        FrameRect(dc, &r, brush(c.changed ? p.accent : p.border));
        InflateRect(&r, -scale(3), 0);
        SelectObject(dc, mono_);
        SetTextColor(dc, p.text);
        auto s = c.choices.empty() ? c.text : c.text + L" ▾";
        DrawTextW(dc, s.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SetViewportOrgEx(dc, -client.left, -client.top, nullptr);
}
void VfNumbers::open(int cell) {
    close(true);
    auto& c = cells_[cell];
    auto box = std::ranges::find_if(boxes_, [&](const Box& b) { return b.cell == cell; });
    if (box == boxes_.end()) return;
    RECT r = box->r;
    OffsetRect(&r, -x_, 0);
    if (!c.choices.empty()) {
        HMENU menu = CreatePopupMenu();
        for (size_t i = 0; i < c.choices.size(); ++i)
            AppendMenuW(menu, MF_STRING | (c.choices[i] == c.text ? MF_CHECKED : 0), i + 1, c.choices[i].c_str());
        POINT at{r.left, r.bottom};
        ClientToScreen(window_, &at);
        int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN, at.x, at.y, 0, window_, nullptr);
        DestroyMenu(menu);
        if (chosen && edited) edited(cell, std::to_wstring(chosen - 1));
        return;
    }
    editing_ = cell;
    SetWindowTextW(edit_, c.text.c_str());
    MoveWindow(edit_, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
    ShowWindow(edit_, SW_SHOW);
    SetFocus(edit_);
    SendMessageW(edit_, EM_SETSEL, 0, -1);
}
void VfNumbers::close(bool commit) {
    if (editing_ < 0) return;
    int cell = editing_;
    editing_ = -1;
    int n = GetWindowTextLengthW(edit_);
    std::wstring s(n + 1, L'\0');
    s.resize(GetWindowTextW(edit_, s.data(), n + 1));
    ShowWindow(edit_, SW_HIDE);
    if (commit && s != cells_[cell].text && edited) edited(cell, s);
}
LRESULT CALLBACK VfNumbers::editProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto self = reinterpret_cast<VfNumbers*>(data);
    if (message == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    if (message == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) {
        self->close(wp == VK_RETURN);
        SetFocus(self->window_);
        return 0;
    }
    if (message == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;
    if (message == WM_KILLFOCUS) {
        auto result = DefSubclassProc(window, message, wp, lp);
        self->close(true);
        return result;
    }
    return DefSubclassProc(window, message, wp, lp);
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
        close(true);
        scrolled();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(wp), palette().text);
        SetBkColor(reinterpret_cast<HDC>(wp), palette().field);
        return reinterpret_cast<LRESULT>(brush(palette().field));
    case WM_LBUTTONDOWN: {
        POINT at{GET_X_LPARAM(lp) + x_, GET_Y_LPARAM(lp)};
        for (auto& b : boxes_)
            if (b.cell >= 0 && PtInRect(&b.r, at)) {
                open(b.cell);
                return 0;
            }
        close(true);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && reinterpret_cast<HWND>(wp) == window_) {
            POINT at{};
            GetCursorPos(&at);
            ScreenToClient(window_, &at);
            at.x += x_;
            bool cell = std::ranges::any_of(boxes_, [&](const Box& b) { return b.cell >= 0 && PtInRect(&b.r, at); });
            SetCursor(LoadCursorW(nullptr, cell ? IDC_IBEAM : IDC_ARROW));
            return TRUE;
        }
        break;
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
        close(true);
        x_ = target;
        scrolled();
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(window_, message, wp, lp);
}
}
