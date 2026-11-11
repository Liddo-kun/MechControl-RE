#pragma once
#include <windows.h>
#include "profiles.h"

namespace mech {
inline constexpr UINT chartDpiMessage = WM_APP + 20;
inline constexpr UINT chartPointMessage =
    WM_APP + 21; // UI checks: wparam is a handle index; lparam 0 packs its temperature/duty, 1 its client pixel.
inline constexpr UINT chartSetMessage = WM_APP + 22; // lparam points to a Curve, copied synchronously; null clears.
inline constexpr UINT chartGetMessage = WM_APP + 23; // lparam points to caller-owned Curve.
struct ChartOptions {
    bool read_only = false, interpolated = false;
    int duty_base = 0;
    bool operator==(const ChartOptions&) const = default;
};
inline constexpr UINT chartOptionsMessage = WM_APP + 24;     // lparam points to ChartOptions.
inline constexpr UINT chartTemperatureMessage = WM_APP + 25; // wparam=temperature+1; zero means unavailable.
void registerFanChart(HINSTANCE instance);
// The Link Fans status: the linked duty as a fill, then each fan's temperature, duty and safety ramp.
void paintLinkedStatus(HDC dc, const RECT& bar, const config::Json& status, UINT dpi);
inline constexpr wchar_t chartClass[] = L"MechControl.Native.FanChart";
}
