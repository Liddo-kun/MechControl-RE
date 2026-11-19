#include "vf_chart.h"
#include "theme.h"
#include <objidl.h>
#include <windowsx.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>
// gdiplus.h expects the min/max macros that NOMINMAX removes.
namespace Gdiplus {
using std::min;
using std::max;
}
#include <gdiplus.h>

namespace mech {
namespace {
namespace gp = Gdiplus;
constexpr wchar_t chartClass[] = L"MechControl.Native.VfChart";
gp::Color color(COLORREF c, BYTE alpha = 255) { return {alpha, GetRValue(c), GetGValue(c), GetBValue(c)}; }
COLORREF mix(COLORREF a, COLORREF b, double x) {
    auto c = [x](int p, int q) { return int(p + (q - p) * x + 0.5); };
    return RGB(c(GetRValue(a), GetRValue(b)), c(GetGValue(a), GetGValue(b)), c(GetBValue(a), GetBValue(b)));
}
// A number as the page shows it: the shortest text that reads back as the same value.
std::wstring number(double v) {
    char b[32];
    auto end = std::to_chars(b, b + sizeof b, v).ptr;
    return std::wstring(b, end);
}
std::wstring sign(double v) { return (v > 0 ? L"+" : L"") + number(v); }
// Tick values from a to b, about n of them, on steps of 1, 2, 2.5 or 5 times a power of ten.
std::vector<double> ticks(double a, double b, int n) {
    double raw = (b - a) / n, mag = std::pow(10, std::floor(std::log10(raw))), step = 10 * mag;
    for (double x : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (x * mag >= raw) {
            step = x * mag;
            break;
        }
    std::vector<double> out;
    for (double v = std::ceil(a / step) * step; v <= b + 1e-9 && out.size() < 100; v += step)
        out.push_back(std::round(v * 1e6) / 1e6);
    return out;
}
}

VfChart::VfChart(HINSTANCE instance, HWND parent) {
    WNDCLASSW c{};
    if (!GetClassInfoW(instance, chartClass, &c)) {
        c.style = CS_DBLCLKS;
        c.lpfnWndProc = procedure;
        c.hInstance = instance;
        c.lpszClassName = chartClass;
        RegisterClassW(&c);
    }
    window_ = CreateWindowExW(0, chartClass, L"V/F curve chart", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, parent,
                              nullptr, instance, this);
    if (!window_) throw std::runtime_error("Cannot create the V/F curve chart");
}
VfChart::~VfChart() {
    if (window_) DestroyWindow(window_);
}
void VfChart::set(VfPlot p) {
    if (p.mv != plot_.mv) {
        selected_.clear();
        anchor_ = hover_ = -1;
        view_.reset();
    }
    plot_ = std::move(p);
    InvalidateRect(window_, nullptr, FALSE);
}
void VfChart::reset_view() {
    view_.reset();
    InvalidateRect(window_, nullptr, FALSE);
}
void VfChart::clear_selection() {
    selected_.clear();
    anchor_ = -1;
    InvalidateRect(window_, nullptr, FALSE);
}
void VfChart::dpi(UINT value) {
    dpi_ = value;
    InvalidateRect(window_, nullptr, FALSE);
}
RECT VfChart::plot() const {
    RECT r{};
    GetClientRect(window_, &r);
    // Above: a strip for Reset view and the selection, so they never cover the curve. Below: the voltages, then the
    // axis title on its own line.
    return {r.left + LONG(px(58)), r.top + LONG(px(34)), r.right - LONG(px(12)), r.bottom - LONG(px(46))};
}
// Fitted to what is shown: every point voltage, and the clocks shown with 5 % room.
VfChart::View VfChart::view() const {
    if (view_) return *view_;
    double lo = INFINITY, hi = -INFINITY;
    for (auto& s : plot_.series)
        for (auto& v : s.y)
            if (v) {
                lo = std::min(lo, *v);
                hi = std::max(hi, *v);
            }
    if (lo > hi) lo = 0, hi = 3500;
    double pad = std::max(10.0, (hi - lo) * 0.05);
    double x0 = plot_.mv.empty() ? 0 : plot_.mv.front(), x1 = plot_.mv.empty() ? 1 : plot_.mv.back(),
           room = std::max(1.0, (x1 - x0) * 0.015);
    return {x0 - room, x1 + room, lo - pad, hi + pad};
}
int VfChart::nearest(double mv) const {
    int best = 0;
    for (int p = 1; p < int(plot_.mv.size()); ++p)
        if (std::abs(plot_.mv[p] - mv) < std::abs(plot_.mv[best] - mv)) best = p;
    return best;
}
int VfChart::hit(POINT at) const {
    for (int i = 1; i < 5; ++i)
        if (PtInRect(&buttons_[i], at)) return i;
    return 0;
}
// A click selects its point; Shift: the range from the last one; Ctrl: adds or removes one.
void VfChart::pick(POINT at, bool shift, bool control) {
    auto r = plot();
    if (!PtInRect(&r, at) || plot_.mv.empty()) return;
    int p = nearest(shown_.x0 + double(at.x - r.left) / (r.right - r.left) * (shown_.x1 - shown_.x0));
    if (shift && anchor_ >= 0) {
        if (!control) selected_.clear();
        for (int i = std::min(p, anchor_); i <= std::max(p, anchor_); ++i) selected_.insert(i);
    } else if (control) {
        if (!selected_.erase(p)) selected_.insert(p);
        anchor_ = p;
    } else {
        bool only = selected_.size() == 1 && selected_.contains(p);
        selected_.clear();
        if (!only) selected_.insert(p);
        anchor_ = p;
    }
    InvalidateRect(window_, nullptr, FALSE);
}
void VfChart::hover(POINT at) {
    auto r = plot();
    int p = -1;
    if (PtInRect(&r, at) && !plot_.mv.empty() && !hit(at))
        p = nearest(shown_.x0 + double(at.x - r.left) / (r.right - r.left) * (shown_.x1 - shown_.x0));
    if (p == hover_ && (p < 0 || (mouse_.x == at.x && mouse_.y == at.y))) return;
    hover_ = p;
    mouse_ = at;
    InvalidateRect(window_, nullptr, FALSE);
}
// "750–762.5, 800 mV: +15 MHz" for the selected points and their offsets.
std::wstring VfChart::selection_text() const {
    std::vector<int> ps(selected_.begin(), selected_.end());
    std::wstring s;
    for (size_t k = 0; k < ps.size(); ++k) {
        size_t j = k;
        while (j + 1 < ps.size() && ps[j + 1] == ps[j] + 1) ++j;
        s += (s.empty() ? L"" : L", ") + number(plot_.mv[ps[k]]) + (j > k ? L"–" + number(plot_.mv[ps[j]]) : L"");
        k = j;
    }
    double lo = INFINITY, hi = -INFINITY;
    for (int p : ps) {
        double o = p < int(plot_.offsets.size()) ? plot_.offsets[p] : 0;
        lo = std::min(lo, o);
        hi = std::max(hi, o);
    }
    return s + L" mV: " + (lo == hi ? sign(lo) : sign(lo) + L" to " + sign(hi)) + L" MHz";
}
void VfChart::paint(HDC target) {
    RECT client{};
    GetClientRect(window_, &client);
    if (IsRectEmpty(&client)) return;
    Buffer buffer(target, client);
    auto dc = buffer.dc;
    auto& c = palette();
    FillRect(dc, &client, brush(c.window));
    roundRect(dc, client, int(px(8)), c.card, c.border);
    if (!gdiplus()) return;
    gp::Graphics g(dc);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);
    gp::Font font(L"Segoe UI", px(11), gp::FontStyleRegular, gp::UnitPixel),
        bold(L"Segoe UI", px(11), gp::FontStyleBold, gp::UnitPixel),
        symbol(L"Segoe UI Symbol", px(10), gp::FontStyleRegular, gp::UnitPixel);
    gp::SolidBrush ink(color(c.text)), muted(color(c.muted));
    auto r = plot();
    auto v = shown_ = view();
    float L = float(r.left), T = float(r.top), R = float(r.right), B = float(r.bottom);
    auto X = [&](double x) { return float(L + (x - v.x0) / (v.x1 - v.x0) * (R - L)); };
    auto Y = [&](double y) { return float(B - (y - v.y0) / (v.y1 - v.y0) * (B - T)); };
    auto measure = [&](const std::wstring& s, const gp::Font& f) {
        gp::RectF box;
        g.MeasureString(s.c_str(), -1, &f, gp::PointF(0, 0), &box);
        return box;
    };
    gp::Pen grid(color(mix(c.card, c.border, 0.55))), zero(color(c.muted));
    for (double t : ticks(v.x0, v.x1, 12)) {
        g.DrawLine(&grid, X(t), T, X(t), B);
        auto s = number(t);
        auto box = measure(s, font);
        g.DrawString(s.c_str(), -1, &font, gp::PointF(X(t) - box.Width / 2, B + px(3)), &muted);
    }
    for (double t : ticks(v.y0, v.y1, 10)) {
        g.DrawLine(t == 0 ? &zero : &grid, L, Y(t), R, Y(t));
        auto s = number(t);
        auto box = measure(s, font);
        g.DrawString(s.c_str(), -1, &font, gp::PointF(L - px(5) - box.Width, Y(t) - box.Height / 2), &muted);
    }
    {
        std::wstring s = L"real point voltage (mV)";
        auto box = measure(s, font);
        g.DrawString(s.c_str(), -1, &font,
                     gp::PointF((L + R - box.Width) / 2, float(client.bottom) - box.Height - px(5)), &muted);
        box = measure(plot_.axis, font);
        auto state = g.Save();
        g.TranslateTransform(px(4), (T + B + box.Width) / 2);
        g.RotateTransform(-90);
        g.DrawString(plot_.axis.c_str(), -1, &font, gp::PointF(0, 0), &muted);
        g.Restore(state);
    }
    int n = int(plot_.mv.size());
    auto mv = [&](int p) { return plot_.mv[p]; };
    g.SetClip(gp::RectF(L, T, R - L, B - T));
    gp::SolidBrush band(gp::Color(77, 255, 196, 0));
    for (int p : selected_) {
        if (p >= n) continue;
        float x = X(mv(p)), a = p ? (X(mv(p - 1)) + x) / 2 : x - px(3),
              b = p < n - 1 ? (x + X(mv(p + 1))) / 2 : x + px(3);
        g.FillRectangle(&band, a, T, std::max(b - a, px(2)), B - T);
    }
    if (hover_ >= 0 && hover_ < n) {
        gp::Pen line(color(c.muted, 110));
        g.DrawLine(&line, X(mv(hover_)), T, X(mv(hover_)), B);
    }
    auto clampY = [&](double y) { return std::clamp(Y(y), -1e5f, 1e5f); };
    for (auto& l : plot_.lines) {
        gp::Pen pen(color(l.color, l.hot ? 242 : 77), l.hot ? px(1.8f) : px(1));
        std::vector<gp::PointF> run;
        auto flush = [&] {
            if (run.size() > 1) g.DrawLines(&pen, run.data(), int(run.size()));
            run.clear();
        };
        for (int p = 0; p < n && p < int(l.y.size()); ++p)
            if (l.y[p])
                run.push_back({X(mv(p)), clampY(*l.y[p])});
            else
                flush();
        flush();
    }
    for (auto& s : plot_.series) {
        gp::SolidBrush fill(color(s.color));
        gp::Pen pen(color(s.color), px(1.3f));
        for (int p = 0; p < n && p < int(s.y.size()); ++p) {
            if (!s.y[p]) continue;
            float x = X(mv(p)), y = clampY(*s.y[p]);
            if (s.mark == 'f')
                g.FillEllipse(&fill, x - px(2.3f), y - px(2.3f), px(4.6f), px(4.6f));
            else if (s.mark == 'o')
                g.DrawEllipse(&pen, x - px(3.8f), y - px(3.8f), px(7.6f), px(7.6f));
            else {
                g.DrawLine(&pen, x - px(3.5f), y, x + px(3.5f), y);
                g.DrawLine(&pen, x, y - px(3.5f), x, y + px(3.5f));
            }
        }
    }
    g.ResetClip();
    // A box with text at a corner of the plot or by the mouse.
    auto box = [&](const gp::RectF& b, COLORREF fill, COLORREF line) {
        RECT rr{LONG(b.X), LONG(b.Y), LONG(std::ceil(b.X + b.Width)), LONG(std::ceil(b.Y + b.Height))};
        roundRect(dc, rr, int(px(4)), fill, line);
    };
    if (!plot_.notes.empty()) {
        float y = T + px(4), w = 0, h = 0;
        for (auto& s : plot_.notes) {
            auto m = measure(s, bold);
            w = std::max(w, m.Width);
            h += m.Height;
        }
        box({L + px(6), y, w + px(10), h + px(4)}, c.card, c.border);
        for (auto& s : plot_.notes) {
            g.DrawString(s.c_str(), -1, &bold, gp::PointF(L + px(11), y + px(2)), &ink);
            y += measure(s, bold).Height;
        }
    }
    // In the strip above the plot, from the right: Reset view while the view is zoomed or moved, then the selection
    // with its up, down and clear buttons.
    for (auto& b : buttons_) b = {};
    float strip = float(client.top) + px(5);
    auto button = [&](int i, const std::wstring& s, float right, const gp::Font& f) {
        auto m = measure(s, f);
        float w = m.Width + px(12), h = px(20);
        gp::RectF b(right - w, strip + px(2), w, h);
        box(b, c.control, c.border);
        g.DrawString(s.c_str(), -1, &f, gp::PointF(b.X + px(6), b.Y + (h - m.Height) / 2), &ink);
        buttons_[i] = {LONG(b.X), LONG(b.Y), LONG(b.X + b.Width), LONG(b.Y + b.Height)};
        return b.X;
    };
    float x = view_ ? button(1, L"Reset view", R, font) - px(10) : R;
    if (!selected_.empty()) {
        COLORREF amber = c.dark ? RGB(92, 74, 20) : RGB(255, 244, 200), amberEdge = RGB(217, 184, 74);
        float buttons = px(9);
        for (auto s : {L"✕", L"▼", L"▲"}) buttons += measure(s, symbol).Width + px(12);
        auto s = selection_text();
        auto m = measure(s, font);
        gp::RectF bar(x - buttons - m.Width - px(12), strip, buttons + m.Width + px(12), px(24));
        box(bar, amber, amberEdge);
        g.DrawString(s.c_str(), -1, &font, gp::PointF(bar.X + px(6), bar.Y + (bar.Height - m.Height) / 2), &ink);
        float bx = button(4, L"✕", x - px(3), symbol) - px(3);
        bx = button(3, L"▼", bx, symbol) - px(3);
        button(2, L"▲", bx, symbol);
    }
    if (hover_ >= 0 && hover_ < n) {
        struct Line {
            std::wstring text;
            bool bold;
            std::optional<COLORREF> dot;
        };
        std::vector<Line> lines;
        std::wstring head = number(mv(hover_)) + L" mV";
        std::wstring rest =
            L" (point " + std::to_wstring(hover_) +
            (hover_ < int(plot_.listed.size()) ? L"; the driver lists " + number(plot_.listed[hover_]) + L" mV)"
                                               : L")");
        double o = hover_ < int(plot_.offsets.size()) ? plot_.offsets[hover_] : 0;
        float w = measure(head, bold).Width + measure(rest, font).Width, h = 0, line = measure(L"0", font).Height;
        h += line;
        if (o) lines.push_back({L"Afterburner Style OC: " + sign(o) + L" MHz", false, {}});
        for (auto& s : plot_.series)
            if (hover_ < int(s.y.size()) && s.y[hover_])
                lines.push_back(
                    {s.name + L": " + (plot_.relative ? sign(*s.y[hover_]) : number(*s.y[hover_])), false, s.color});
        for (auto& l : lines) {
            w = std::max(w, measure(l.text, font).Width + (l.dot ? px(14) : 0));
            h += line;
        }
        float bw = w + px(14), bh = h + px(10), bx = mouse_.x + px(14), by = mouse_.y + px(10);
        if (bx + bw > client.right) bx = mouse_.x - bw - px(14);
        bx = std::max(px(2), bx);
        by = std::max(px(2), std::min(by, client.bottom - bh - px(4)));
        box({bx, by, bw, bh}, c.card, c.muted);
        float ty = by + px(5);
        g.DrawString(head.c_str(), -1, &bold, gp::PointF(bx + px(7), ty), &ink);
        g.DrawString(rest.c_str(), -1, &font, gp::PointF(bx + px(7) + measure(head, bold).Width, ty), &ink);
        ty += line;
        for (auto& l : lines) {
            float tx = bx + px(7);
            if (l.dot) {
                gp::SolidBrush dot(color(*l.dot));
                g.FillEllipse(&dot, tx, ty + (line - px(9)) / 2, px(9), px(9));
                tx += px(14);
            }
            g.DrawString(l.text.c_str(), -1, &font, gp::PointF(tx, ty), &ink);
            ty += line;
        }
    }
    if (GetFocus() == window_ && palette().contrast) DrawFocusRect(dc, &client);
}
LRESULT CALLBACK VfChart::procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto chart = reinterpret_cast<VfChart*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        chart = static_cast<VfChart*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        chart->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(chart));
    }
    if (message == WM_NCDESTROY && chart) {
        chart->window_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }
    return chart && chart->window_ ? chart->message(message, wp, lp) : DefWindowProcW(window, message, wp, lp);
}
LRESULT VfChart::message(UINT message, WPARAM wp, LPARAM lp) {
    POINT at{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    bool shift = GetKeyState(VK_SHIFT) < 0, control = GetKeyState(VK_CONTROL) < 0;
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(window_, &ps);
        paint(dc);
        EndPaint(window_, &ps);
        return 0;
    }
    case WM_SIZE: InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_GETDLGCODE: {
        auto m = reinterpret_cast<MSG*>(lp);
        return DLGC_WANTARROWS |
               (m && m->message == WM_KEYDOWN && m->wParam == VK_ESCAPE && !selected_.empty() ? DLGC_WANTMESSAGE : 0);
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS: InvalidateRect(window_, nullptr, FALSE); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p{};
            GetCursorPos(&p);
            ScreenToClient(window_, &p);
            auto r = plot();
            SetCursor(LoadCursorW(nullptr, hit(p) ? IDC_HAND : PtInRect(&r, p) ? IDC_CROSS : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        SetFocus(window_);
        int b = hit(at);
        if (message == WM_LBUTTONDBLCLK && !b) {
            reset_view();
            return 0;
        }
        if (b == 1)
            reset_view();
        else if (b == 4) {
            selected_.clear();
            InvalidateRect(window_, nullptr, FALSE);
        } else if (b && bump)
            bump(selected_, (b == 2 ? 1 : -1) * (shift ? 4 : 1));
        else if (!b) {
            SetCapture(window_);
            drag_ = Drag{at, shown_};
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, window_};
        TrackMouseEvent(&t);
        if (drag_ && (wp & MK_LBUTTON)) {
            int dx = at.x - drag_->from.x, dy = at.y - drag_->from.y;
            if (!drag_->moved && dx * dx + dy * dy < int(px(5) * px(5))) return 0; // a click, not a drag
            drag_->moved = true;
            hover_ = -1;
            auto r = plot();
            auto& v = drag_->view;
            double mx = double(dx) / std::max(1L, r.right - r.left) * (v.x1 - v.x0),
                   my = double(dy) / std::max(1L, r.bottom - r.top) * (v.y1 - v.y0);
            view_ = View{v.x0 - mx, v.x1 - mx, v.y0 + my, v.y1 + my};
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        hover(at);
        return 0;
    }
    case WM_LBUTTONUP:
        if (drag_ && !drag_->moved) pick(at, shift, control);
        drag_.reset();
        if (GetCapture() == window_) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED: drag_.reset(); return 0;
    case WM_MOUSELEAVE:
        if (hover_ >= 0) {
            hover_ = -1;
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        // Zooms around the mouse; Shift: the voltage only, Alt: the clock only.
        POINT p = at;
        ScreenToClient(window_, &p);
        auto r = plot();
        auto v = shown_;
        double k = GET_WHEEL_DELTA_WPARAM(wp) < 0 ? 1.2 : 1 / 1.2,
               cx = v.x0 + double(p.x - r.left) / std::max(1L, r.right - r.left) * (v.x1 - v.x0),
               cy = v.y0 + double(r.bottom - p.y) / std::max(1L, r.bottom - r.top) * (v.y1 - v.y0);
        if (GetKeyState(VK_MENU) >= 0) v.x0 = cx + (v.x0 - cx) * k, v.x1 = cx + (v.x1 - cx) * k;
        if (!shift) v.y0 = cy + (v.y0 - cy) * k, v.y1 = cy + (v.y1 - cy) * k;
        view_ = v;
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        // Up and down change the selected points' offsets (Shift: 4 steps), left and right move the selection.
        if (selected_.empty()) break;
        if (wp == VK_UP || wp == VK_DOWN) {
            if (bump) bump(selected_, (wp == VK_UP ? 1 : -1) * (shift ? 4 : 1));
            return 0;
        }
        if (wp == VK_LEFT || wp == VK_RIGHT) {
            int d = wp == VK_LEFT ? -1 : 1, n = int(plot_.mv.size());
            if (std::ranges::any_of(selected_, [&](int p) { return p + d < 0 || p + d >= n; })) return 0;
            std::set<int> moved;
            for (int p : selected_) moved.insert(p + d);
            selected_ = std::move(moved);
            if (anchor_ >= 0) anchor_ += d;
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            selected_.clear();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        break;
    }
    return DefWindowProcW(window_, message, wp, lp);
}
}
