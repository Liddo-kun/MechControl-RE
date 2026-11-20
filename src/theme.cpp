#include "theme.h"
#include <dwmapi.h>
#include <uxtheme.h>
#include <objidl.h>
#include <windowsx.h>
#include <algorithm>
#include <map>
#include <string>
// gdiplus.h expects the min/max macros that NOMINMAX removes.
namespace Gdiplus {
using std::min;
using std::max;
}
#include <gdiplus.h>

namespace mech {
namespace {
namespace gp = Gdiplus;
constexpr Palette lightColors{.dark = false,
                              .window = RGB(243, 244, 247),
                              .card = RGB(255, 255, 255),
                              .border = RGB(218, 221, 227),
                              .text = RGB(28, 30, 33),
                              .muted = RGB(98, 104, 114),
                              .accent = RGB(16, 110, 220),
                              .accentHot = RGB(12, 96, 196),
                              .onAccent = RGB(255, 255, 255),
                              .link = RGB(10, 94, 196),
                              .control = RGB(249, 250, 251),
                              .hot = RGB(236, 239, 243),
                              .pressed = RGB(224, 228, 234),
                              .field = RGB(255, 255, 255),
                              .bar = RGB(235, 237, 241),
                              .ok = RGB(46, 125, 50),
                              .warn = RGB(214, 110, 0),
                              .error = RGB(198, 40, 40)},
    darkColors{.dark = true,
               .window = RGB(25, 26, 30),
               .card = RGB(36, 38, 43),
               .border = RGB(56, 59, 66),
               .text = RGB(232, 234, 238),
               .muted = RGB(150, 156, 166),
               .accent = RGB(40, 128, 232),
               .accentHot = RGB(58, 142, 240),
               .onAccent = RGB(255, 255, 255),
               .link = RGB(112, 178, 255),
               .control = RGB(50, 53, 60),
               .hot = RGB(60, 64, 72),
               .pressed = RGB(44, 47, 53),
               .field = RGB(28, 29, 33),
               .bar = RGB(31, 32, 36),
               .ok = RGB(102, 190, 110),
               .warn = RGB(255, 170, 60),
               .error = RGB(240, 96, 92)};
Palette current = lightColors;
std::map<COLORREF, HBRUSH> brushes;

gp::Color color(COLORREF c, BYTE alpha = 255) { return {alpha, GetRValue(c), GetGValue(c), GetBValue(c)}; }
UINT dpi = 96;
int px(int n) { return MulDiv(n, dpi, 96); }
// With a border, the path runs through the centers of r's edge pixels so that a 1 px pen stays sharp.
void rounded(gp::GraphicsPath& path, const RECT& r, float radius, bool centered) {
    float inset = centered ? 0.5f : 0, x = r.left + inset, y = r.top + inset, w = r.right - r.left - 2 * inset,
          h = r.bottom - r.top - 2 * inset, d = std::min({2 * radius, w, h});
    if (d <= 0) {
        path.AddRectangle(gp::RectF(x, y, w, h));
        return;
    }
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}
void fill(gp::Graphics& g, const RECT& r, float radius, COLORREF inside, COLORREF edge) {
    gp::GraphicsPath path;
    rounded(path, r, radius, edge != inside);
    gp::SolidBrush b(color(inside));
    g.FillPath(&b, &path);
    if (edge == inside) return;
    gp::Pen pen(color(edge));
    g.DrawPath(&pen, &path);
}
void circle(gp::Graphics& g, float cx, float cy, float radius, COLORREF inside, COLORREF edge) {
    gp::SolidBrush b(color(inside));
    g.FillEllipse(&b, cx - radius, cy - radius, 2 * radius, 2 * radius);
    if (edge == inside) return;
    gp::Pen pen(color(edge));
    g.DrawEllipse(&pen, cx - radius, cy - radius, 2 * radius, 2 * radius);
}
std::wstring text(HWND window) {
    std::wstring s(GetWindowTextLengthW(window) + 1, L'\0');
    s.resize(GetWindowTextW(window, s.data(), int(s.size())));
    return s;
}
// The brush the parent paints behind the control: a section's or the page's.
HBRUSH backdrop(HWND control, HDC dc) {
    auto b = reinterpret_cast<HBRUSH>(SendMessageW(GetParent(control), WM_CTLCOLORBTN, WPARAM(dc), LPARAM(control)));
    return b ? b : brush(current.window);
}
bool shown(HWND window, WORD flag) { return !(SendMessageW(window, WM_QUERYUISTATE, 0, 0) & flag); }
RECT client(HWND window) {
    RECT r{};
    GetClientRect(window, &r);
    return r;
}
// Keeps the part under the mouse (0: none) as the subclass's reference data and repaints when it changes.
void hover(HWND window, SUBCLASSPROC procedure, DWORD_PTR hot, DWORD_PTR part) {
    if (part == hot) return;
    SetWindowSubclass(window, procedure, 2, part);
    if (part) {
        TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, window};
        TrackMouseEvent(&t);
    }
    InvalidateRect(window, nullptr, FALSE);
}
// Paints an area of a control off screen: these controls repaint while the mouse moves.
template <class Draw> void buffered(HWND window, HDC target, const RECT& area, Draw draw) {
    if (IsRectEmpty(&area) || !gdiplus()) return;
    Buffer buffer(target, area);
    SelectObject(buffer.dc, reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)));
    gp::Graphics g(buffer.dc);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    draw(buffer.dc, g, area);
}

LRESULT drawButton(const NMCUSTOMDRAW& d, bool primary) {
    if (d.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    auto window = d.hdr.hwndFrom;
    auto& p = current;
    auto type = GetWindowLongW(window, GWL_STYLE) & BS_TYPEMASK;
    bool disabled = d.uItemState & CDIS_DISABLED, hot = d.uItemState & CDIS_HOT, pressed = d.uItemState & CDIS_SELECTED,
         focus = (d.uItemState & CDIS_FOCUS) && shown(window, UISF_HIDEFOCUS);
    UINT prefix = shown(window, UISF_HIDEACCEL) ? 0 : DT_HIDEPREFIX;
    buffered(window, d.hdc, client(window), [&](HDC dc, gp::Graphics& g, RECT r) {
        FillRect(dc, &r, backdrop(window, dc));
        auto title = text(window);
        if (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX) {
            int size = px(16), top = (r.bottom - size) / 2;
            RECT box{px(1), top, px(1) + size, top + size};
            bool on = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED;
            COLORREF inside = !on ? p.field : disabled ? p.muted : hot ? p.accentHot : p.accent;
            fill(g, box, float(px(4)), inside, on ? inside : disabled ? p.border : hot ? p.accent : p.muted);
            if (on) {
                gp::Pen pen(color(p.onAccent), float(px(2)));
                pen.SetLineJoin(gp::LineJoinRound);
                pen.SetStartCap(gp::LineCapRound);
                pen.SetEndCap(gp::LineCapRound);
                auto at = [&](float x, float y) { return gp::PointF(box.left + x * size, box.top + y * size); };
                gp::PointF mark[]{at(0.26f, 0.52f), at(0.43f, 0.68f), at(0.75f, 0.34f)};
                g.DrawLines(&pen, mark, 3);
            }
            RECT label{box.right + px(8), r.top, r.right, r.bottom};
            SetTextColor(dc, disabled ? p.muted : p.text);
            DrawTextW(dc, title.c_str(), -1, &label, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | prefix);
            if (focus) {
                DrawTextW(dc, title.c_str(), -1, &label, DT_SINGLELINE | DT_CALCRECT | prefix);
                OffsetRect(&label, 0, (r.bottom - label.bottom) / 2);
                InflateRect(&label, px(2), 0);
                DrawFocusRect(dc, &label);
            }
            return;
        }
        bool accent = primary && !disabled;
        COLORREF inside = disabled  ? p.control
                          : primary ? (hot && !pressed ? p.accentHot : p.accent)
                          : pressed ? p.pressed
                          : hot     ? p.hot
                                    : p.control,
                 ink = disabled  ? p.muted
                       : primary ? p.onAccent
                                 : p.text,
                 edge = focus    ? p.text
                        : accent ? inside
                                 : p.border;
        // High contrast has one face color and one for text and borders: the frame shows the mouse and a focus
        // rectangle the keyboard.
        if (p.contrast && (hot || pressed)) edge = accent ? p.text : p.accent;
        fill(g, r, float(px(5)), inside, edge);
        if (p.contrast && focus) {
            RECT inner = r;
            InflateRect(&inner, -px(3), -px(3));
            DrawFocusRect(dc, &inner);
        }
        if (pressed) OffsetRect(&r, 0, 1);
        SetTextColor(dc, ink);
        DrawTextW(dc, title.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | prefix);
    });
    return CDRF_SKIPDEFAULT;
}
// A rounded track filled up to a round thumb. No tick marks: the control computes the positions of automatic ticks
// only while it paints itself, and the value is shown beside every slider.
LRESULT drawTrack(const NMCUSTOMDRAW& d) {
    if (d.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    auto window = d.hdr.hwndFrom;
    auto& p = current;
    buffered(window, d.hdc, client(window), [&](HDC dc, gp::Graphics& g, RECT r) {
        FillRect(dc, &r, backdrop(window, dc));
        RECT channel{}, thumb{};
        SendMessageW(window, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
        SendMessageW(window, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        bool enabled = IsWindowEnabled(window);
        int lo = int(SendMessageW(window, TBM_GETRANGEMIN, 0, 0)),
            hi = int(SendMessageW(window, TBM_GETRANGEMAX, 0, 0)),
            pos = std::clamp(int(SendMessageW(window, TBM_GETPOS, 0, 0)), lo, hi),
            half = (thumb.right - thumb.left) / 2, from = channel.left + half, to = channel.right - half,
            cx = hi > lo ? from + MulDiv(pos - lo, to - from, hi - lo) : from,
            diameter = std::min(px(16), int(r.bottom)), cy = r.bottom / 2, bar = px(4);
        RECT track{from, cy - bar / 2, to, cy - bar / 2 + bar}, done{from, track.top, cx, track.bottom};
        fill(g, track, bar / 2.f, p.border, p.border);
        if (cx > from) fill(g, done, bar / 2.f, enabled ? p.accent : p.muted, enabled ? p.accent : p.muted);
        float radius = diameter / 2.f - 0.5f;
        bool focus = GetFocus() == window && shown(window, UISF_HIDEFOCUS);
        circle(g, cx + 0.5f, cy + 0.5f, radius, p.control, focus ? p.text : p.border);
        circle(g, cx + 0.5f, cy + 0.5f, radius * 0.45f, enabled ? p.accent : p.muted, enabled ? p.accent : p.muted);
        if (p.contrast && focus) DrawFocusRect(dc, &r);
    });
    return CDRF_SKIPDEFAULT;
}
// Text tabs over a rule; the selected one is underlined in the accent color and the one under the mouse shaded.
void paintTabs(HWND window, HDC target, int over) {
    auto& p = current;
    // The control spans the pages, which cover all but a margin of it below the tabs. Only the tabs are buffered.
    RECT strip = client(window), below = strip, first{};
    TabCtrl_GetItemRect(window, 0, &first);
    strip.bottom = below.top = first.bottom + 1;
    FillRect(target, &below, brush(p.window));
    buffered(window, target, strip, [&](HDC dc, gp::Graphics& g, RECT r) {
        FillRect(dc, &r, brush(p.window));
        int selected = TabCtrl_GetCurSel(window);
        bool focus = GetFocus() == window && shown(window, UISF_HIDEFOCUS);
        for (int i = 0, n = TabCtrl_GetItemCount(window); i < n; ++i) {
            RECT item{};
            TabCtrl_GetItemRect(window, i, &item);
            wchar_t name[64]{};
            TCITEMW t{TCIF_TEXT};
            t.pszText = name;
            t.cchTextMax = 64;
            TabCtrl_GetItem(window, i, &t);
            if (i == over && i != selected) {
                RECT back{item.left + px(1), item.top + px(2), item.right - px(1), item.bottom - px(4)};
                fill(g, back, float(px(4)), p.hot, p.hot);
            }
            SetTextColor(dc, i == selected || i == over ? p.text : p.muted);
            RECT label = item;
            label.bottom -= px(2);
            DrawTextW(dc, name, -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (i != selected) continue;
            RECT line{item.left + px(8), item.bottom - px(3), item.right - px(8), item.bottom};
            fill(g, line, px(3) / 2.f, p.accent, p.accent);
            if (focus) {
                InflateRect(&label, -px(3), -px(3));
                DrawFocusRect(dc, &label);
            }
        }
        RECT rule{0, r.bottom - 1, r.right, r.bottom};
        FillRect(dc, &rule, brush(p.border));
    });
}
// Two arrow buttons that continue the buddy edit box; over is the one under the mouse (1: up, 2: down).
void paintSpin(HWND window, HDC target, int over) {
    auto& p = current;
    buffered(window, target, client(window), [&](HDC dc, gp::Graphics& g, RECT r) {
        FillRect(dc, &r, brush(p.field));
        FrameRect(dc, &r, brush(p.border));
        bool enabled = IsWindowEnabled(window), down = GetCapture() == window && (GetKeyState(VK_LBUTTON) & 0x8000);
        int mid = r.bottom / 2, s = px(3);
        for (int part : {1, 2}) {
            RECT half{1, part == 1 ? 1 : mid, r.right - 1, part == 1 ? mid : r.bottom - 1};
            if (enabled && over == part) FillRect(dc, &half, brush(down ? p.pressed : p.hot));
            float cx = (half.left + half.right) / 2.f, cy = (half.top + half.bottom) / 2.f,
                  dy = part == 1 ? s / 2.f : -s / 2.f;
            gp::Pen pen(color(!enabled ? p.border : over == part ? p.text : p.muted), px(3) / 2.f);
            pen.SetLineJoin(gp::LineJoinRound);
            gp::PointF arrow[]{{cx - s, cy + dy}, {cx, cy - dy}, {cx + s, cy + dy}};
            g.DrawLines(&pen, arrow, 3);
        }
    });
}
// hot is the tab under the mouse plus one.
LRESULT CALLBACK tabsProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR hot) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(window, &ps);
        paintTabs(window, dc, int(hot) - 1);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        TCHITTESTINFO hit{{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}};
        hover(window, tabsProcedure, hot, TabCtrl_HitTest(window, &hit) + 1);
        break;
    }
    case WM_MOUSELEAVE: hover(window, tabsProcedure, hot, 0); break;
    }
    return DefSubclassProc(window, message, wp, lp);
}
// hot is the arrow under the mouse.
LRESULT CALLBACK spinProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR hot) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(window, &ps);
        paintSpin(window, dc, int(hot));
        EndPaint(window, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: hover(window, spinProcedure, hot, GET_Y_LPARAM(lp) < client(window).bottom / 2 ? 1 : 2); break;
    case WM_MOUSELEAVE: hover(window, spinProcedure, hot, 0); break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
        auto result = DefSubclassProc(window, message, wp, lp);
        InvalidateRect(window, nullptr, FALSE);
        return result;
    }
    }
    return DefSubclassProc(window, message, wp, lp);
}
LRESULT CALLBACK wholeProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) InvalidateRect(window, nullptr, FALSE);
    return DefSubclassProc(window, message, wp, lp);
}
LRESULT CALLBACK frameProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (message == WM_NCPAINT) {
        auto dc = GetWindowDC(window);
        RECT r{};
        GetWindowRect(window, &r);
        OffsetRect(&r, -r.left, -r.top);
        FrameRect(dc, &r, brush(GetFocus() == window ? current.accent : current.border));
        ReleaseDC(window, dc);
        return 0;
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        auto result = DefSubclassProc(window, message, wp, lp);
        RedrawWindow(window, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
        return result;
    }
    return DefSubclassProc(window, message, wp, lp);
}
}

const Palette& palette() { return current; }
void setThemeDpi(UINT value) { dpi = value; }
bool systemDark() {
    DWORD light = 1, size = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return !light;
}
bool setPalette(bool dark) {
    HIGHCONTRASTW contrast{sizeof contrast};
    Palette next = dark ? darkColors : lightColors;
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON)) {
        auto c = GetSysColor;
        next = {.dark = false,
                .contrast = true,
                .window = c(COLOR_BTNFACE),
                .card = c(COLOR_BTNFACE),
                .border = c(COLOR_BTNTEXT),
                .text = c(COLOR_BTNTEXT),
                .muted = c(COLOR_GRAYTEXT),
                .accent = c(COLOR_HIGHLIGHT),
                .accentHot = c(COLOR_HIGHLIGHT),
                .onAccent = c(COLOR_HIGHLIGHTTEXT),
                .link = c(COLOR_HOTLIGHT),
                .control = c(COLOR_BTNFACE),
                .hot = c(COLOR_BTNFACE),
                .pressed = c(COLOR_BTNFACE),
                .field = c(COLOR_WINDOW),
                .bar = c(COLOR_BTNFACE),
                .ok = c(COLOR_BTNTEXT),
                .warn = c(COLOR_BTNTEXT),
                .error = c(COLOR_BTNTEXT)};
    }
    if (next == current) return false;
    current = next;
    for (auto [c, b] : brushes) DeleteObject(b);
    brushes.clear();
    return true;
}
HBRUSH brush(COLORREF color) {
    auto& b = brushes[color];
    if (!b) b = CreateSolidBrush(color);
    return b;
}
COLORREF mix(COLORREF a, COLORREF b, double x) {
    auto c = [x](int p, int q) { return int(p + (q - p) * x + 0.5); };
    return RGB(c(GetRValue(a), GetRValue(b)), c(GetGValue(a), GetGValue(b)), c(GetBValue(a), GetBValue(b)));
}
bool gdiplus() {
    static const bool started = [] {
        ULONG_PTR token = 0;
        gp::GdiplusStartupInput input;
        return gp::GdiplusStartup(&token, &input, nullptr) == gp::Ok;
    }();
    return started;
}
void roundRect(HDC dc, const RECT& r, int radius, COLORREF inside, COLORREF border) {
    if (!gdiplus()) return;
    gp::Graphics g(dc);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    fill(g, r, float(radius), inside, border);
}
Buffer::Buffer(HDC target, const RECT& area) : dc(CreateCompatibleDC(target)), target_(target), area_(area) {
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), area.right - area.left, area.top - area.bottom, 1, 32, BI_RGB};
    void* bits = nullptr;
    bitmap_ = CreateDIBSection(target, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    old_ = SelectObject(dc, bitmap_);
    SetViewportOrgEx(dc, -area.left, -area.top, nullptr);
    SetBkMode(dc, TRANSPARENT);
}
Buffer::~Buffer() {
    BitBlt(target_, area_.left, area_.top, area_.right - area_.left, area_.bottom - area_.top, dc, area_.left,
           area_.top, SRCCOPY);
    SelectObject(dc, old_);
    DeleteObject(bitmap_);
    DeleteDC(dc);
}
LRESULT drawControl(const NMCUSTOMDRAW& draw, bool primary) {
    wchar_t type[32]{};
    GetClassNameW(draw.hdr.hwndFrom, type, 32);
    if (!gdiplus()) return CDRF_DODEFAULT;
    if (!_wcsicmp(type, WC_BUTTONW)) return drawButton(draw, primary);
    if (!_wcsicmp(type, TRACKBAR_CLASSW)) return drawTrack(draw);
    return CDRF_DODEFAULT;
}
void styleTabs(HWND tabs) { SetWindowSubclass(tabs, tabsProcedure, 2, 0); }
void styleSpin(HWND spin) { SetWindowSubclass(spin, spinProcedure, 2, 0); }
void styleFrame(HWND child) { SetWindowSubclass(child, frameProcedure, 2, 0); }
void paintWhole(HWND child) { SetWindowSubclass(child, wholeProcedure, 3, 0); }
void systemTheme(HWND window, const wchar_t* darkClass) {
    SetWindowTheme(window, current.dark ? darkClass : nullptr, nullptr);
}
void titleBar(HWND window) {
    BOOL dark = current.dark;
    // DWMWA_USE_IMMERSIVE_DARK_MODE, which the 19041 SDK does not name. Windows honors it from that build on.
    DwmSetWindowAttribute(window, 20, &dark, sizeof dark);
}
}
