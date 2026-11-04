#pragma once
#include <windows.h>
#include "profiles.h"

namespace mech {
inline constexpr UINT chartDpiMessage = WM_APP + 20;
inline constexpr UINT chartPointMessage = WM_APP + 21; // Selected temperature/duty packed for UI smoke checks.
inline constexpr UINT chartSetMessage = WM_APP + 22; // lparam points to a Curve, copied synchronously; null clears.
inline constexpr UINT chartGetMessage = WM_APP + 23; // lparam points to caller-owned Curve.
struct ChartOptions {bool read_only=false,interpolated=false;int duty_base=0;bool operator==(const ChartOptions&) const=default;};
inline constexpr UINT chartOptionsMessage = WM_APP + 24; // lparam points to ChartOptions.
inline constexpr UINT chartTemperatureMessage = WM_APP + 25; // wparam=temperature+1; zero means unavailable.
void registerFanChart(HINSTANCE instance);
inline constexpr wchar_t chartClass[] = L"MechControl.Native.FanChart";
}
