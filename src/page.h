#pragma once
#include <windows.h>
#include <commctrl.h>
#include "theme.h"
#include <climits>
#include <functional>
#include <string>
#include <vector>

namespace mech {
// The page code of every tab: a scrolling window of cards with titles, painted texts and native controls, laid out in
// one pass that measures it and, while placing, also moves the controls.
struct PageFonts {
    HFONT font = nullptr, bold = nullptr, title = nullptr;
    UINT dpi = 96;
};
// Painted text; no font: the normal one.
struct PageText {
    RECT rect;
    std::wstring text;
    COLORREF color;
    HFONT font = nullptr;
    UINT format = DT_WORDBREAK | DT_NOPREFIX;
    bool operator==(const PageText& o) const;
};
// A card with its title inside; an alert (a banner, without a title) is tinted with the error color.
struct PageFrame {
    RECT rect;
    std::wstring title;
    bool alert = false;
    bool operator==(const PageFrame& o) const;
};
struct Page {
    HWND window = nullptr;
    int scroll = 0;
    std::vector<PageFrame> frames;
    std::vector<PageText> texts;
    SCROLLINFO shown{}; // what the scroll bar was last given

    // The background, cards and texts within area, into a buffer's dc.
    void paint(HDC dc, const RECT& area, const PageFonts& fonts) const;
    // The color painted at a point, or behind a child control.
    COLORREF backdrop(POINT at) const;
    COLORREF backdrop(HWND child) const;
    void printClient(HDC dc) const; // WM_PRINTCLIENT: links, sliders and check boxes ask for what is behind them
    // Sets the scroll bar for the content's height and keeps the scroll within it; returns whether the scroll changed.
    bool range(int content);
    // Where WM_MOUSEWHEEL or WM_VSCROLL asks to scroll to.
    int target(UINT message, WPARAM wp, UINT dpi) const;
    // Moves the painted bits and the controls in one step, so only the strip scrolled into view repaints and no control
    // is shown for a frame where others have not moved yet; the caller lays out again. Returns whether it moved.
    bool scrollTo(int target);
    // Moves the bits in from (default: all) by `by` pixels, clipped to into, with the controls there.
    void shift(int by, const RECT* from = nullptr, const RECT* into = nullptr);
};
int pageRef(UINT dpi, int n); // the reference's 144-DPI pixel constants at a DPI
// WM_CTLCOLOR*: edit boxes and lists have the field color, other controls the color behind them.
LRESULT controlColors(UINT message, HDC dc, COLORREF back);
// NM_CUSTOMDRAW for a link: links in the link color, the other text in `text`.
LRESULT drawLink(const NMCUSTOMDRAW& draw, COLORREF text);
// WM_PAINT: a few small parts far apart (a text at the top, a label further down) are painted each by itself, not the
// whole area that holds them.
void paintParts(HWND window, const std::function<void(HDC, const RECT&)>& paint);
// A tooltip of a control (TTF_IDISHWND, id = the control) or of a numbered rectangle of `owner`; the text is copied.
// TTM_UPDATETIPTEXTW changes the text of one added before.
void addTip(HWND tooltip, HWND owner, UINT_PTR id, const std::wstring& text, UINT flags = 0,
            UINT message = TTM_ADDTOOLW);
HWND pageControl(HINSTANCE instance, HWND parent, const wchar_t* type, const std::wstring& title, DWORD style,
                 int id = 0);

// One pass over a page: it measures the page and, with place, also places the controls. y runs down the scrolled
// page. finish() keeps what it painted in the page.
struct PageLayout {
    Page& target;
    PageFonts fonts;
    int width, scroll;
    bool place;
    HDC dc;
    HGDIOBJ previous;
    int titleHeight, line;
    // pad: the reference's labels inset their text by about a third of the font's em on each side.
    int x, right, left, end, y, pad, slider, box, button;
    std::vector<PageFrame> frames;
    std::vector<PageText> texts;

    PageLayout(Page& page, PageFonts fonts, int pageWidth, bool placing);
    ~PageLayout();
    PageLayout(const PageLayout&) = delete;
    PageLayout& operator=(const PageLayout&) = delete;
    int ref(int n) const { return pageRef(fonts.dpi, n); }
    int scale(int n) const { return MulDiv(n, fonts.dpi, 96); }
    int height(HFONT font);
    int extent(HFONT font, const std::wstring& s);
    // Moves a control only where it is not already, and repaints it there. A drop-down list's height includes its
    // list, which its window takes but does not keep.
    void put(HWND child, int cx, int cy, int w, int h, bool shown = true, bool list = false);
    void open(const std::wstring& title) { open(title, x, right); }
    // A section is a card with its title inside; Page::paint draws it.
    void open(const std::wstring& title, int from, int to);
    void close();
    // The right edge of a section that fits its content (to contentRight) or its title, whichever is wider.
    int fit(const std::wstring& title, int from, int contentRight);
    // A check box sized to its text on its own row from `from`, at most widest wide.
    void check(HWND box, int from, bool shown, HFONT font, int widest = INT_MAX);
    // Painted text wrapped from 'from' to 'to'. Returns its bottom.
    int paragraph(const std::wstring& s, int from, int top, int to, COLORREF color, HFONT font);
    // Painted text on one line (height 0: the normal font's line). Returns its right edge.
    int say(const std::wstring& s, int cx, int cy, COLORREF color = palette().text, HFONT font = nullptr, int h = 0);
    // A link wrapped to width, top below y; moves y to its bottom.
    void link(HWND control, int from, int top, int w);
    // Keeps what the pass painted in the page (the frames and texts before stay in this pass) and returns the content
    // height.
    int finish();
};
}
