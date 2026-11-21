#include "vf_chart.h"
#include "config_text.h"
#include "theme.h"
#include <objidl.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
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
constexpr UINT_PTR canvasTimer = 1;
gp::Color color(COLORREF c, BYTE alpha = 255) { return {alpha, GetRValue(c), GetGValue(c), GetBValue(c)}; }
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

std::wstring number(double v) { return config::wide(config::decimal_text(v, 3, true)); }
std::wstring sign(double v) { return (v > 0 ? L"+" : L"") + number(v); }
std::wstring runs(const std::vector<double>& mv, const std::vector<int>& points) {
    std::wstring s;
    for (size_t k = 0; k < points.size(); ++k) {
        size_t j = k;
        while (j + 1 < points.size() && points[j + 1] == points[j] + 1) ++j;
        s += (s.empty() ? L"" : L", ") + number(mv[points[k]]) + (j > k ? L"–" + number(mv[points[j]]) : L"");
        k = j;
    }
    return s + L" mV";
}

// What paint() keeps: the fonts and, while the chart is being redrawn (a slider drag repaints it at every step), the
// bitmap it draws on, the marks and the sizes of the axis labels. A fresh bitmap of the chart's size alone costs a
// fifth of a repaint. All but the fonts are let go half a second after the last repaint, so a tab that was hidden is
// soon at rest.
struct VfChart::Kept {
    // A series' mark as GDI+ draws it, copied to each point: drawing the few hundred marks of a chart one by one
    // takes GDI+ a third of the repaint. One per quarter-pixel position, so a copy is at most 1/8 px off its point.
    struct Mark {
        int half = 0;                 // the bitmap is 2 * half + 1 pixels each way, the point in its middle one
        std::vector<uint32_t> pixels; // premultiplied
    };
    UINT dpi = 0;
    std::unique_ptr<gp::Font> font, bold, symbol;
    std::map<std::wstring, gp::RectF> labels;
    std::map<std::tuple<COLORREF, char, int>, Mark> marks; // by color, kind and position within the pixel
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;
    uint32_t* bits = nullptr;
    SIZE size{};
    ~Kept() { release(); }
    const Mark& mark(COLORREF c, char kind, int place, float scale) {
        if (marks.size() > 400) marks.clear();
        auto [known, fresh] = marks.try_emplace({c, kind, place});
        auto& m = known->second;
        if (!fresh) return m;
        m.half = int(std::ceil(5.5f * scale)) + 1;
        int side = 2 * m.half + 1;
        m.pixels.resize(size_t(side) * side);
        gp::Bitmap image(side, side, side * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(m.pixels.data()));
        gp::Graphics g(&image);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        float x = m.half + (place % 4 + 0.5f) / 4, y = m.half + (place / 4 + 0.5f) / 4;
        gp::SolidBrush fill(color(c));
        gp::Pen pen(color(c), 1.3f * scale);
        if (kind == 'f')
            g.FillEllipse(&fill, x - 2.3f * scale, y - 2.3f * scale, 4.6f * scale, 4.6f * scale);
        else if (kind == 'o')
            g.DrawEllipse(&pen, x - 3.8f * scale, y - 3.8f * scale, 7.6f * scale, 7.6f * scale);
        else {
            g.DrawLine(&pen, x - 3.5f * scale, y, x + 3.5f * scale, y);
            g.DrawLine(&pen, x, y - 3.5f * scale, x, y + 3.5f * scale);
        }
        return m;
    }
    // Copies a mark over the bitmap within `clip`, its point at (x, y).
    void stamp(const Mark& m, float x, float y, const RECT& clip) {
        int left = int(std::floor(x)) - m.half, top = int(std::floor(y)) - m.half, side = 2 * m.half + 1;
        for (int j = std::max(0, int(clip.top) - top); j < side && top + j < clip.bottom; ++j) {
            auto from = m.pixels.data() + size_t(j) * side;
            auto to = bits + size_t(top + j) * size.cx + left;
            for (int i = std::max(0, int(clip.left) - left); i < side && left + i < clip.right; ++i) {
                uint32_t s = from[i], keep = 255 - (s >> 24);
                if (keep == 255) continue;
                // Each color of the bitmap, scaled by keep / 255, with the mark's added.
                uint32_t rb = (to[i] & 0xff00ff) * keep + 0x800080, g = (to[i] & 0x00ff00) * keep + 0x008000;
                rb = ((rb + ((rb >> 8) & 0xff00ff)) >> 8) & 0xff00ff;
                g = ((g + ((g >> 8) & 0x00ff00)) >> 8) & 0x00ff00;
                to[i] = (rb | g) + (s & 0xffffff);
            }
        }
    }
    void release() {
        if (!dc) return;
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
        dc = nullptr;
    }
    HDC canvas(HDC target, SIZE wanted) {
        if (dc && (size.cx != wanted.cx || size.cy != wanted.cy)) release();
        if (!dc) {
            BITMAPINFO info{};
            info.bmiHeader = {sizeof(BITMAPINFOHEADER), wanted.cx, -wanted.cy, 1, 32, BI_RGB};
            dc = CreateCompatibleDC(target);
            bitmap = CreateDIBSection(target, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
            old = SelectObject(dc, bitmap);
            SetBkMode(dc, TRANSPARENT);
            size = wanted;
        }
        return dc;
    }
};

VfChart::VfChart(HINSTANCE instance, HWND parent) : kept_(std::make_unique<Kept>()) {
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
    // Left: the clocks, headed by their unit. Below: the voltages, which end with theirs.
    return {r.left + LONG(px(42)), r.top + LONG(px(12)), r.right - LONG(px(12)), r.bottom - LONG(px(22))};
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
bool VfChart::onPlot(POINT at) const {
    auto r = plot();
    return PtInRect(&r, at) && !hit(at) && !PtInRect(&buttons_[0], at);
}
// A click selects its point; Shift: the range from the last one; Ctrl: adds or removes one.
void VfChart::pick(POINT at, bool shift, bool control) {
    auto r = plot();
    if (!onPlot(at) || plot_.mv.empty()) return;
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
    if (onPlot(at) && !plot_.mv.empty())
        p = nearest(shown_.x0 + double(at.x - r.left) / (r.right - r.left) * (shown_.x1 - shown_.x0));
    if (p == hover_ && (p < 0 || (mouse_.x == at.x && mouse_.y == at.y))) return;
    hover_ = p;
    mouse_ = at;
    InvalidateRect(window_, nullptr, FALSE);
}
// "750–762.5, 800 mV: +15 MHz" for the selected points and their offsets.
std::wstring VfChart::selection_text() const {
    std::vector<int> ps(selected_.begin(), selected_.end());
    double lo = INFINITY, hi = -INFINITY;
    for (int p : ps) {
        double o = p < int(plot_.offsets.size()) ? plot_.offsets[p] : 0;
        lo = std::min(lo, o);
        hi = std::max(hi, o);
    }
    return runs(plot_.mv, ps) + L": " + (lo == hi ? sign(lo) : sign(lo) + L" to " + sign(hi)) + L" MHz";
}
void VfChart::paint(HDC target) {
    RECT client{};
    GetClientRect(window_, &client);
    if (IsRectEmpty(&client)) return;
    auto dc = kept_->canvas(target, {client.right, client.bottom});
    SetTimer(window_, canvasTimer, 500, nullptr);
    FillRect(dc, &client, brush(palette().window));
    roundRect(dc, client, int(px(8)), palette().card, palette().border);
    if (gdiplus()) draw(dc, client);
    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
}
void VfChart::draw(HDC dc, const RECT& client) {
    auto& c = palette();
    auto& k = *kept_;
    if (k.dpi != dpi_) {
        k.dpi = dpi_;
        k.font = std::make_unique<gp::Font>(L"Segoe UI", px(11), gp::FontStyleRegular, gp::UnitPixel);
        k.bold = std::make_unique<gp::Font>(L"Segoe UI", px(11), gp::FontStyleBold, gp::UnitPixel);
        k.symbol = std::make_unique<gp::Font>(L"Segoe UI Symbol", px(10), gp::FontStyleRegular, gp::UnitPixel);
        k.labels.clear();
        k.marks.clear();
    }
    auto &font = *k.font, &bold = *k.bold, &symbol = *k.symbol;
    gp::Graphics g(dc);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);
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
    // An axis label's size; the same labels come back at every repaint.
    auto label = [&](const std::wstring& s) {
        if (k.labels.size() > 500) k.labels.clear();
        auto [known, fresh] = k.labels.try_emplace(s);
        if (fresh) known->second = measure(s, font);
        return known->second;
    };
    gp::Pen grid(color(mix(c.card, c.border, 0.55))), zero(color(c.muted));
    // "mV" ends the voltage row; a voltage that would run into it is left out.
    auto unit = label(L"mV");
    float unitLeft = R - unit.Width;
    g.DrawString(L"mV", -1, &font, gp::PointF(unitLeft, B + px(3)), &muted);
    for (double t : ticks(v.x0, v.x1, 12)) {
        g.DrawLine(&grid, X(t), T, X(t), B);
        auto s = number(t);
        auto box = label(s);
        if (X(t) + box.Width / 2 < unitLeft - px(6))
            g.DrawString(s.c_str(), -1, &font, gp::PointF(X(t) - box.Width / 2, B + px(3)), &muted);
    }
    // "MHz" heads the clocks; a clock that would run into it is left out.
    auto unitTop = label(L"MHz");
    float headBottom = float(client.top) + px(3) + unitTop.Height;
    g.DrawString(L"MHz", -1, &font, gp::PointF(L - px(5) - unitTop.Width, float(client.top) + px(3)), &muted);
    for (double t : ticks(v.y0, v.y1, 10)) {
        g.DrawLine(t == 0 ? &zero : &grid, L, Y(t), R, Y(t));
        auto s = number(t);
        auto box = label(s);
        if (Y(t) - box.Height / 2 > headBottom)
            g.DrawString(s.c_str(), -1, &font, gp::PointF(L - px(5) - box.Width, Y(t) - box.Height / 2), &muted);
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
    // What lies wholly outside the plot is not handed to GDI+: most of a steep line does, and clipping it there costs
    // as much as drawing it.
    float room = px(6);
    auto outside = [&](const gp::PointF& a, const gp::PointF& b) {
        return (a.X < L - room && b.X < L - room) || (a.X > R + room && b.X > R + room) ||
               (a.Y < T - room && b.Y < T - room) || (a.Y > B + room && b.Y > B + room);
    };
    std::vector<gp::PointF> run;
    for (auto& l : plot_.lines) {
        gp::Pen pen(color(l.color, l.hot ? 242 : 77), l.hot ? px(1.8f) : px(1));
        auto flush = [&] {
            if (run.size() > 1) g.DrawLines(&pen, run.data(), int(run.size()));
            run.clear();
        };
        std::optional<gp::PointF> last;
        for (int p = 0; p < n && p < int(l.y.size()); ++p) {
            auto point = l.y[p] ? std::optional(gp::PointF(X(mv(p)), clampY(*l.y[p]))) : std::nullopt;
            if (!point || !last || outside(*last, *point))
                flush();
            else if (run.empty())
                run.push_back(*last);
            if (point && last && !outside(*last, *point)) run.push_back(*point);
            last = point;
        }
        flush();
    }
    g.Flush(gp::FlushIntentionSync); // the marks go straight to the bitmap
    for (auto& s : plot_.series)
        for (int p = 0; p < n && p < int(s.y.size()); ++p) {
            if (!s.y[p]) continue;
            gp::PointF at(X(mv(p)), clampY(*s.y[p]));
            if (outside(at, at)) continue;
            int place = int((at.X - std::floor(at.X)) * 4) + 4 * int((at.Y - std::floor(at.Y)) * 4);
            k.stamp(k.mark(s.color, s.mark, place, px(1)), at.X, at.Y, r);
        }
    g.ResetClip();
    // A box with text at a corner of the plot or by the mouse.
    auto box = [&](const gp::RectF& b, COLORREF fill, COLORREF line) {
        RECT rr{LONG(b.X), LONG(b.Y), LONG(std::ceil(b.X + b.Width)), LONG(std::ceil(b.Y + b.Height))};
        roundRect(dc, rr, int(px(4)), fill, line);
    };
    // Over the bottom right of the plot, where a rising curve leaves room, from the right: Reset view while the view is
    // zoomed or moved, then the selection with its up, down and clear buttons.
    COLORREF amber = c.dark ? RGB(92, 74, 20) : RGB(255, 244, 200), amberEdge = RGB(217, 184, 74);
    for (auto& b : buttons_) b = {};
    float strip = B - px(30);
    auto button = [&](int i, const std::wstring& s, float right, const gp::Font& f) {
        auto m = measure(s, f);
        float w = m.Width + px(12), h = px(20);
        gp::RectF b(right - w, strip + px(2), w, h);
        box(b, c.control, c.border);
        g.DrawString(s.c_str(), -1, &f, gp::PointF(b.X + px(6), b.Y + (h - m.Height) / 2), &ink);
        buttons_[i] = {LONG(b.X), LONG(b.Y), LONG(b.X + b.Width), LONG(b.Y + b.Height)};
        return b.X;
    };
    float x = view_ ? button(1, L"Reset view", R - px(6), font) - px(10) : R - px(6);
    if (!selected_.empty()) {
        float buttons = px(9);
        for (auto s : {L"✕", L"▼", L"▲"}) buttons += measure(s, symbol).Width + px(12);
        auto s = selection_text();
        auto m = measure(s, font);
        gp::RectF bar(x - buttons - m.Width - px(12), strip, buttons + m.Width + px(12), px(24));
        box(bar, amber, amberEdge);
        buttons_[0] = {LONG(bar.X), LONG(bar.Y), LONG(bar.X + bar.Width), LONG(bar.Y + bar.Height)};
        g.DrawString(s.c_str(), -1, &font, gp::PointF(bar.X + px(6), bar.Y + (bar.Height - m.Height) / 2), &ink);
        float bx = button(4, L"✕", x - px(3), symbol) - px(3);
        bx = button(3, L"▼", bx, symbol) - px(3);
        button(2, L"▲", bx, symbol);
    }
    // Over the top left of the plot, where a rising curve leaves room: an existing overclock was found.
    if (!plot_.notice.empty()) {
        const wchar_t* head = L"Existing overclock detected";
        float width = std::min(R - L - px(16), px(320)), inner = width - px(16);
        gp::RectF layout(0, 0, inner, 1e4f), top, body;
        g.MeasureString(head, -1, &bold, layout, &top);
        g.MeasureString(plot_.notice.c_str(), -1, &font, layout, &body);
        gp::RectF b(L + px(8), T + px(8), width, top.Height + body.Height + px(12));
        box(b, amber, amberEdge);
        g.DrawString(head, -1, &bold, gp::RectF(b.X + px(8), b.Y + px(6), inner, top.Height), nullptr, &ink);
        g.DrawString(plot_.notice.c_str(), -1, &font,
                     gp::RectF(b.X + px(8), b.Y + px(6) + top.Height, inner, body.Height + px(2)), nullptr, &ink);
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
        if (plot_.memory) lines.push_back({L"Memory OC: " + sign(plot_.memory) + L" MHz", false, {}});
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
    case WM_TIMER:
        if (wp == canvasTimer) {
            KillTimer(window_, canvasTimer);
            kept_->release();
            kept_->marks.clear();
            kept_->labels.clear();
        }
        return 0;
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
            SetCursor(LoadCursorW(nullptr, hit(p) ? IDC_HAND : onPlot(p) ? IDC_CROSS : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        SetFocus(window_);
        int b = hit(at);
        if (!b && PtInRect(&buttons_[0], at)) return 0; // the selection's text
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
