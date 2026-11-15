#pragma once
#include <windows.h>
#include <commctrl.h>

namespace mech {
// The window's colors. Windows' app mode chooses light or dark; with high contrast on, the system colors are used.
struct Palette {
    bool dark = false, contrast = false;
    COLORREF window, card, border, text, muted, accent, accentHot, onAccent, link, control, hot, pressed, field, bar,
        ok, warn, error;
    bool operator==(const Palette&) const = default;
};
const Palette& palette();
// The window's DPI, which scales what these functions draw.
void setThemeDpi(UINT dpi);
// Whether Windows asks apps for dark mode.
bool systemDark();
// Returns whether the palette changed.
bool setPalette(bool dark);
// A brush of the color, kept until the palette changes.
HBRUSH brush(COLORREF color);
// GDI+ starts on the first draw, so the hidden tray app never loads it.
bool gdiplus();
// An antialiased rounded rectangle; a border of another color is drawn on r's edge pixels.
void roundRect(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border);
// Paints an area off screen and copies it to the target when destroyed. Text is drawn transparently. The bitmap is a
// DIB section, which GDI+ draws into about twice as fast as into a compatible bitmap.
struct Buffer {
    Buffer(HDC target, const RECT& area);
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    const HDC dc;

private:
    HDC target_;
    RECT area_;
    HBITMAP bitmap_;
    HGDIOBJ old_;
};
// NM_CUSTOMDRAW for a push button, check box or trackbar: draws it in the palette over the brush its parent returns
// for WM_CTLCOLORBTN. Other controls draw themselves.
LRESULT drawControl(const NMCUSTOMDRAW& draw, bool primary);
// Subclasses that paint a tab control, an up-down control and a WS_BORDER frame in the palette.
void styleTabs(HWND tabs);
void styleSpin(HWND spin);
void styleFrame(HWND child);
// Trackbars and links clear their background to a light system color before they paint all of it themselves, and a
// moved trackbar repaints only the narrow thumb of its own design, not the drawn one.
void paintWhole(HWND child);
// The parts Windows draws: in dark mode, the window gets the named dark theme class (scroll bars, drop-down lists,
// tooltips) and a top-level window the dark title bar.
void systemTheme(HWND window, const wchar_t* darkClass);
void titleBar(HWND window);
}
