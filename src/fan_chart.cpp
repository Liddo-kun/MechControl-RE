#include "fan_chart.h"
#include <commctrl.h>
#include <objidl.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>
namespace Gdiplus {
using std::min;
using std::max;
} // gdiplus.h expects the min/max macros that NOMINMAX removes.
#include <gdiplus.h>

namespace mech {
namespace {
namespace gp = Gdiplus;
// The reference drew its curves with GDI+. It starts on the first paint, so the hidden tray app never loads it.
bool gdiplus() {
    static const bool started = [] {
        ULONG_PTR token = 0;
        gp::GdiplusStartupInput input;
        return gp::GdiplusStartup(&token, &input, nullptr) == gp::Ok;
    }();
    return started;
}
// The reference FanCurvePanel.
struct Chart {
    config::Curve curve; // Empty until the owner supplies real data. Fixture seeds live only in the fixture.
    ChartOptions options;
    int selected = -1, hover = -1, temperature = -1;
    bool dragging = false;
    UINT dpi = 96;
    std::wstring title, tip;
    HWND tooltip = nullptr;
    // The reference drew its pixel constants unscaled on a 144 DPI screen; scaling from 144 DPI keeps its proportions.
    float ref(float n) const { return n * dpi / 144; }
    int count() const { return int(curve.temp_up.size()); }
    // Step mode shows how the EC reads the table: each handle sits at its step-up corner and edits the next duty.
    // The last handle has no next zone, so it shares the top plateau's duty.
    int duty_index(int i) const { return options.interpolated ? i : std::min(i + 1, count() - 1); }
    float duty(int i) const { return float(curve.duty[i] / 2); }
    // The reference measured its margins from the outer edge, border included.
    gp::RectF plot(HWND window) const {
        RECT r{};
        GetClientRect(window, &r);
        float left = ref(44), top = ref(24);
        return {left, top, r.right + 2 - left - ref(12), r.bottom + 2 - top - ref(28)};
    }
    gp::PointF pixel(const gp::RectF& p, int temp, float percent) const {
        return {p.X + float(temp) / 105 * p.Width, p.Y + (1 - percent / 100) * p.Height};
    }
    gp::PointF handle(const gp::RectF& p, int i) const { return pixel(p, curve.temp_up[i], duty(duty_index(i))); }
    // The last handle wins where the top plateau's two handles overlap.
    int hit(HWND window, int x, int y) const {
        auto p = plot(window);
        float radius = ref(10);
        for (int i = count() - 1; i >= 0; --i) {
            auto h = handle(p, i);
            float dx = x - h.X, dy = y - h.Y;
            if (dx * dx + dy * dy <= radius * radius) return i;
        }
        return -1;
    }
    void normalize() {
        curve.temp_down.clear();
        for (int i = 0; i < count(); ++i) curve.temp_down.push_back(i ? std::max(0, curve.temp_up[i - 1] - 4) : 0);
    }
    // Points stay 4 °C apart; duty 0 stays 0 (the EC requires it) and the rest stay 1-100% and never fall.
    void drag(HWND window, int x, int y) {
        auto p = plot(window);
        if (p.Width <= 0 || p.Height <= 0) return;
        int i = selected, n = count(), index = duty_index(i);
        int temp = int(std::nearbyint((x - p.X) / p.Width * 105)),
            percent = int(std::nearbyint(100 - (y - p.Y) / p.Height * 100));
        int lo = i ? curve.temp_up[i - 1] + 4 : 0, hi = i < n - 1 ? curve.temp_up[i + 1] - 4 : 105;
        if (lo <= hi) curve.temp_up[i] = std::clamp(temp, lo, hi);
        curve.duty[index] = index ? std::clamp(percent, 1, 100) * 2 : 0;
        for (int j = index + 1; j < n && curve.duty[j] < curve.duty[j - 1]; ++j) curve.duty[j] = curve.duty[j - 1];
        for (int j = index - 1; j >= 0 && curve.duty[j] > curve.duty[j + 1]; --j)
            curve.duty[j] = j ? std::max(2, curve.duty[j + 1]) : 0;
        normalize();
        InvalidateRect(window, nullptr, FALSE);
    }
    void changed(HWND window) {
        SendMessageW(GetParent(window), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(window), EN_CHANGE),
                     reinterpret_cast<LPARAM>(window));
    }
    void hover_tip(HWND window, int x, int y) {
        int index = hit(window, x, y);
        if (index == hover) return;
        hover = index;
        tip.clear();
        if (index >= 0) {
            int d = duty_index(index);
            wchar_t address[40]{};
            if (options.duty_base) swprintf_s(address, L" | Register 0x%03X", options.duty_base + d);
            tip = std::to_wstring(curve.temp_up[index]) + L"°C → " + std::to_wstring(curve.duty[d] / 2) + L"%" +
                  address + L" (Raw " + std::to_wstring(curve.duty[d]) + L")";
        }
        TOOLINFOW tool{sizeof tool};
        tool.hwnd = window;
        tool.uId = reinterpret_cast<UINT_PTR>(window);
        tool.lpszText = tip.data();
        SendMessageW(tooltip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
        if (index < 0) SendMessageW(tooltip, TTM_POP, 0, 0);
    }
    void paint(HWND window, HDC target) const {
        RECT client{};
        GetClientRect(window, &client);
        auto dc = CreateCompatibleDC(target);
        auto bitmap = CreateCompatibleBitmap(target, std::max(1L, client.right), std::max(1L, client.bottom));
        auto oldBitmap = SelectObject(dc, bitmap);
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        if (gdiplus()) draw(window, dc);
        BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
    void draw(HWND window, HDC dc) const {
        gp::Graphics g(dc);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        gp::Font bold(L"Segoe UI", 9.0f * dpi / 72, gp::FontStyleBold, gp::UnitPixel),
            tiny(L"Segoe UI", 7.0f * dpi / 72, gp::FontStyleRegular, gp::UnitPixel);
        gp::SolidBrush black(gp::Color(0, 0, 0)), gray(gp::Color(105, 105, 105)), smoke(gp::Color(245, 245, 245)),
            fill(gp::Color(30, 65, 105, 225)), blue(gp::Color(65, 105, 225)), orange(gp::Color(255, 69, 0));
        gp::Pen grid(gp::Color(190, 195, 200)), axis(gp::Color(0, 0, 0)), line(gp::Color(65, 105, 225), ref(2)),
            hysteresis(gp::Color(130, 65, 105, 225), ref(1.5f)), current(gp::Color(200, 220, 80, 80), ref(1.5f));
        hysteresis.SetDashStyle(gp::DashStyleDash);
        current.SetDashStyle(gp::DashStyleDot);
        auto label = [&](const std::wstring& s, float x, float y, bool center) {
            gp::RectF size;
            g.MeasureString(s.c_str(), -1, &tiny, gp::PointF(0, 0), &size);
            g.DrawString(s.c_str(), -1, &tiny,
                         center ? gp::PointF(x - size.Width / 2, y) : gp::PointF(x - size.Width, y - size.Height / 2),
                         &gray);
        };
        auto p = plot(window);
        float bottom = p.Y + p.Height, right = p.X + p.Width;
        g.DrawString(title.c_str(), -1, &bold, gp::PointF(p.X, ref(4)), &black);
        g.FillRectangle(&smoke, p);
        for (int t = 0; t <= 105; t += 10) {
            auto a = pixel(p, t, 0), b = pixel(p, t, 100);
            g.DrawLine(&grid, a.X, b.Y, a.X, a.Y);
            label(std::to_wstring(t), a.X, a.Y + ref(2), true);
        }
        for (int d = 0; d <= 100; d += 10) {
            auto a = pixel(p, 0, float(d)), b = pixel(p, 105, float(d));
            g.DrawLine(&grid, a.X, a.Y, b.X, a.Y);
            label(std::to_wstring(d) + L"%", p.X - ref(3), a.Y, false);
        }
        g.DrawLine(&axis, p.X, p.Y, p.X, bottom);
        g.DrawLine(&axis, p.X, bottom, right, bottom);
        g.DrawString(L"°C", -1, &tiny, gp::PointF(right - ref(12), bottom + ref(10)), &gray);
        int n = count();
        if (!n) return;
        std::vector<gp::PointF> area;
        if (options.interpolated) {
            // Straight lines are honest only under Link Fans, where software interpolates the curve.
            for (int i = 0; i < n; ++i) area.push_back(pixel(p, curve.temp_up[i], duty(i)));
            area.push_back(pixel(p, curve.temp_up[n - 1], 0));
            area.push_back(pixel(p, curve.temp_up[0], 0));
            g.FillPolygon(&fill, area.data(), INT(area.size()));
            for (int i = 0; i + 1 < n; ++i) g.DrawLine(&line, area[i], area[i + 1]);
        } else {
            // Duty i applies above temperature i-1 up to temperature i.
            area = {pixel(p, curve.temp_up[0], 0), pixel(p, curve.temp_up[0], duty(0))};
            for (int i = 0; i + 1 < n; ++i) {
                area.push_back(pixel(p, curve.temp_up[i], duty(i + 1)));
                area.push_back(pixel(p, curve.temp_up[i + 1], duty(i + 1)));
            }
            area.push_back(pixel(p, curve.temp_up[n - 1], 0));
            g.FillPolygon(&fill, area.data(), INT(area.size()));
            for (int i = 0; i + 1 < n; ++i) {
                auto corner = pixel(p, curve.temp_up[i], duty(i + 1));
                g.DrawLine(&line, pixel(p, curve.temp_up[i], duty(i)), corner);
                g.DrawLine(&line, corner, pixel(p, curve.temp_up[i + 1], duty(i + 1)));
            }
            // The dashed return path: cooling below the down threshold i drops to duty i-1.
            auto down = pixel(p, curve.temp_up[n - 1], duty(n - 1));
            for (int i = n - 1; i >= 1; --i) {
                auto corner = pixel(p, curve.temp_down[i], duty(i)), next = pixel(p, curve.temp_down[i], duty(i - 1));
                g.DrawLine(&hysteresis, down, corner);
                g.DrawLine(&hysteresis, corner, next);
                down = next;
            }
            if (n >= 2) g.DrawLine(&hysteresis, down, pixel(p, curve.temp_up[0], 0));
        }
        // The current temperature, under the points so they stay legible.
        if (temperature > 0 && temperature <= 105)
            g.DrawLine(&current, pixel(p, temperature, 100), pixel(p, temperature, 0));
        float r = ref(5);
        for (int i = 0; i < n; ++i) {
            auto h = handle(p, i);
            g.FillEllipse(i == selected ? &orange : &blue, h.X - r, h.Y - r, 2 * r, 2 * r);
            g.DrawEllipse(&axis, h.X - r, h.Y - r, 2 * r, 2 * r);
        }
    }
};
LRESULT CALLBACK chartProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto c = reinterpret_cast<Chart*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        c = new Chart;
        c->title = reinterpret_cast<CREATESTRUCTW*>(lp)->lpszName;
        c->dpi = GetDpiForWindow(window);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(c));
    }
    if (!c) return DefWindowProcW(window, message, wp, lp);
    switch (message) {
    case WM_CREATE: {
        c->tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                     CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
        TOOLINFOW tool{sizeof tool};
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = window;
        tool.uId = reinterpret_cast<UINT_PTR>(window);
        tool.lpszText = c->tip.data();
        SendMessageW(c->tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        auto dc = BeginPaint(window, &ps);
        c->paint(window, dc);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_SIZE: InvalidateRect(window, nullptr, FALSE); return 0;
    case chartDpiMessage:
        c->dpi = UINT(wp);
        c->hover = -1;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case chartPointMessage: {
        int i = int(wp);
        if (i < 0 || i >= c->count()) return 0;
        if (lp) {
            auto h = c->handle(c->plot(window), i);
            return MAKELRESULT(std::lround(h.X), std::lround(h.Y));
        }
        return MAKELRESULT(c->curve.temp_up[i], c->curve.duty[c->duty_index(i)] / 2);
    }
    case chartOptionsMessage: {
        auto o = *reinterpret_cast<const ChartOptions*>(lp);
        if (o != c->options) {
            c->options = o;
            c->hover = -1;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case chartTemperatureMessage: {
        int temp = int(wp) - 1;
        if (c->temperature != temp) {
            c->temperature = temp;
            if (IsWindowVisible(window)) InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case chartSetMessage: {
        config::Curve value;
        if (lp) value = *reinterpret_cast<const config::Curve*>(lp);
        if (value.temp_up == c->curve.temp_up && value.duty == c->curve.duty) return 0;
        c->curve = std::move(value);
        c->selected = c->hover = -1;
        c->normalize();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case chartGetMessage: *reinterpret_cast<config::Curve*>(lp) = c->curve; return 0;
    case WM_LBUTTONDOWN: {
        if (c->options.read_only) return 0;
        c->selected = c->hit(window, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        c->dragging = c->selected >= 0;
        if (c->dragging) SetCapture(window);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (c->dragging)
            c->drag(window, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        else
            c->hover_tip(window, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_LBUTTONUP:
        if (c->dragging) {
            c->dragging = false;
            ReleaseCapture();
            c->changed(window);
        }
        return 0;
    case WM_CAPTURECHANGED: c->dragging = false; return 0;
    case WM_NCDESTROY:
        if (c->tooltip) DestroyWindow(c->tooltip);
        delete c;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window, message, wp, lp);
}
}
void registerFanChart(HINSTANCE instance) {
    WNDCLASSW type{};
    type.hInstance = instance;
    type.lpfnWndProc = chartProc;
    type.lpszClassName = chartClass;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&type)) throw std::runtime_error("Cannot register fan chart");
}
// The reference LinkedFanStatusBar.
void paintLinkedStatus(HDC dc, const RECT& bar, const config::Json& status, UINT dpi) {
    FrameRect(dc, &bar, GetSysColorBrush(COLOR_WINDOWFRAME));
    if (!gdiplus()) return;
    auto ref = [dpi](float n) { return n * dpi / 144; };
    auto number = [&](const char* key) { return status.value(key, 0); };
    gp::Graphics g(dc);
    float x = float(bar.left + 1), y = float(bar.top + 1), width = float(bar.right - bar.left - 2),
          height = float(bar.bottom - bar.top - 2), at = x + ref(4);
    gp::SolidBrush back(gp::Color(245, 245, 245)), fill(gp::Color(80, 65, 105, 225)), black(gp::Color(0, 0, 0)),
        gray(gp::Color(105, 105, 105));
    g.FillRectangle(&back, x, y, width, height);
    g.FillRectangle(&fill, x, y, width * float(number("duty")) / 100, height);
    gp::Font font(L"Segoe UI", 9.0f * dpi / 72, gp::FontStyleRegular, gp::UnitPixel),
        tiny(L"Segoe UI", 7.0f * dpi / 72, gp::FontStyleRegular, gp::UnitPixel);
    // Each piece starts where the previous one's measured width ends, less a little of its padding.
    auto draw = [&](const std::wstring& s, const gp::Font& face, const gp::Brush& brush, float overlap) {
        g.DrawString(s.c_str(), -1, &face, gp::PointF(at, y + (height - std::ceil(face.GetHeight(&g))) / 2), &brush);
        gp::RectF size;
        g.MeasureString(s.c_str(), -1, &face, gp::PointF(0, 0), &size);
        at += size.Width - ref(overlap);
    };
    auto fan = [&](const wchar_t* name, const char* temp, const char* duty, const char* ramp,
                   const std::wstring& before) {
        draw(before + name + std::to_wstring(number(temp)) + L"°→" + std::to_wstring(number(duty)) + L"% ", font, black,
             4);
        if (number(ramp) > 0) draw(L"(safety ramp " + std::to_wstring(number(ramp)) + L"°C)", tiny, gray, 2);
    };
    fan(L"CPU ", "cpuTemp", "cpuDuty", "cpuRamp", std::to_wstring(number("duty")) + L"%  │  ");
    fan(L"GPU ", "gpuTemp", "gpuDuty", "gpuRamp", L"  │  ");
}
}
