#include "page.h"
#include <algorithm>
#include <stdexcept>

namespace mech {
namespace {
// Where a section's title sits in its card: below the top edge, and right of the accent mark that starts at the
// content's inset.
constexpr int titleTop = 10, titleLeft = 29;
COLORREF fill(const PageFrame& f) {
    auto& p = palette();
    return f.alert ? mix(p.card, p.error, p.dark ? 0.22 : 0.10) : p.card;
}
}

bool PageText::operator==(const PageText& o) const {
    return EqualRect(&rect, &o.rect) && text == o.text && color == o.color && font == o.font && format == o.format;
}
bool PageFrame::operator==(const PageFrame& o) const {
    return EqualRect(&rect, &o.rect) && title == o.title && alert == o.alert;
}
int pageRef(UINT dpi, int n) { return MulDiv(n, dpi, 144); }

void Page::paint(HDC dc, const RECT& area, const PageFonts& fonts) const {
    auto& p = palette();
    auto ref = [&](int n) { return pageRef(fonts.dpi, n); };
    FillRect(dc, &area, brush(p.window));
    for (auto& f : frames) {
        RECT visible{};
        if (!IntersectRect(&visible, &f.rect, &area)) continue;
        roundRect(dc, f.rect, ref(12), fill(f), f.alert ? p.error : p.border);
        if (f.title.empty()) continue;
        // A card with a short accent mark before its bold title.
        SelectObject(dc, fonts.title);
        SIZE size{};
        GetTextExtentPoint32W(dc, f.title.c_str(), int(f.title.size()), &size);
        int top = f.rect.top + ref(titleTop);
        RECT mark{f.rect.left + ref(15), top + size.cy / 5, f.rect.left + ref(20), top + size.cy - size.cy / 5};
        roundRect(dc, mark, ref(3), p.accent, p.accent);
        SetTextColor(dc, p.text);
        TextOutW(dc, f.rect.left + ref(titleLeft), top, f.title.c_str(), int(f.title.size()));
    }
    for (auto& t : texts) {
        RECT visible{};
        if (!IntersectRect(&visible, &t.rect, &area)) continue;
        SelectObject(dc, t.font ? t.font : fonts.font);
        SetTextColor(dc, t.color);
        RECT r = t.rect;
        DrawTextW(dc, t.text.c_str(), -1, &r, t.format);
    }
}
COLORREF Page::backdrop(POINT at) const {
    for (auto& f : frames)
        if (PtInRect(&f.rect, at)) return fill(f);
    return palette().window;
}
COLORREF Page::backdrop(HWND child) const {
    RECT r{};
    GetWindowRect(child, &r);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&r), 2);
    return backdrop(POINT{(r.left + r.right) / 2, (r.top + r.bottom) / 2});
}
// The clip region gives the asking control's area, which lies in one section or outside all of them.
void Page::printClient(HDC dc) const {
    RECT r{};
    GetClipBox(dc, &r);
    FillRect(dc, &r, brush(backdrop(POINT{(r.left + r.right) / 2, (r.top + r.bottom) / 2})));
}
bool Page::range(int content) {
    RECT rc{};
    GetClientRect(window, &rc);
    int top = std::clamp(scroll, 0, std::max(0, content - int(rc.bottom)));
    bool moved = top != scroll;
    scroll = top;
    SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, content - 1), UINT(rc.bottom), scroll};
    if (si.nMax != shown.nMax || si.nPage != shown.nPage || si.nPos != shown.nPos || !shown.cbSize)
        SetScrollInfo(window, SB_VERT, &si, TRUE);
    shown = si;
    return moved;
}
int Page::target(UINT message, WPARAM wp, UINT dpi) const {
    int to = scroll;
    if (message == WM_MOUSEWHEEL) return to - GET_WHEEL_DELTA_WPARAM(wp) * MulDiv(54, dpi, 96) / WHEEL_DELTA;
    SCROLLINFO si{sizeof si, SIF_ALL};
    GetScrollInfo(window, SB_VERT, &si);
    switch (LOWORD(wp)) {
    case SB_LINEUP: return to - MulDiv(28, dpi, 96);
    case SB_LINEDOWN: return to + MulDiv(28, dpi, 96);
    case SB_PAGEUP: return to - int(si.nPage);
    case SB_PAGEDOWN: return to + int(si.nPage);
    case SB_THUMBTRACK: return si.nTrackPos;
    case SB_TOP: return 0;
    case SB_BOTTOM: return si.nMax;
    }
    return to;
}
bool Page::scrollTo(int to) {
    SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE};
    GetScrollInfo(window, SB_VERT, &si);
    to = std::clamp(to, 0, std::max(0, si.nMax + 1 - int(si.nPage)));
    if (to == scroll) return false;
    shift(scroll - to);
    scroll = to;
    return true;
}
void Page::shift(int by, const RECT* from, const RECT* into) {
    RECT exposed{};
    ScrollWindowEx(window, 0, by, from, into, nullptr, &exposed, SW_SCROLLCHILDREN | SW_INVALIDATE);
    // The scroll leaves the border of a number box it brings into view partly unpainted.
    for (auto child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT r{};
        GetWindowRect(child, &r);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&r), 2);
        if (!IntersectRect(&r, &r, &exposed) || !IsWindowVisible(child)) continue;
        wchar_t type[8]{};
        GetClassNameW(child, type, 8);
        if (!_wcsicmp(type, L"Edit")) RedrawWindow(child, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
    }
}

LRESULT controlColors(UINT message, HDC dc, COLORREF back) {
    auto& p = palette();
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) back = p.field;
    SetTextColor(dc, p.text);
    SetBkColor(dc, back);
    return reinterpret_cast<LRESULT>(brush(back));
}
LRESULT drawLink(const NMCUSTOMDRAW& draw, COLORREF text) {
    if (draw.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (draw.dwDrawStage == CDDS_ITEMPREPAINT)
        SetTextColor(draw.hdc, draw.dwItemSpec != UINT_PTR(-1) ? palette().link : text);
    return CDRF_DODEFAULT;
}
void paintParts(HWND window, const std::function<void(HDC, const RECT&)>& paint) {
    std::vector<RECT> parts;
    auto region = CreateRectRgn(0, 0, 0, 0);
    if (GetUpdateRgn(window, region, FALSE) == COMPLEXREGION) {
        std::vector<char> data(GetRegionData(region, 0, nullptr));
        auto list = reinterpret_cast<RGNDATA*>(data.data());
        if (!data.empty() && GetRegionData(region, DWORD(data.size()), list) && list->rdh.nCount <= 64)
            parts.assign(reinterpret_cast<RECT*>(list->Buffer),
                         reinterpret_cast<RECT*>(list->Buffer) + list->rdh.nCount);
    }
    DeleteObject(region);
    PAINTSTRUCT ps{};
    auto dc = BeginPaint(window, &ps);
    LONGLONG area = 0;
    for (auto& r : parts) area += LONGLONG(r.right - r.left) * (r.bottom - r.top);
    if (area * 2 > LONGLONG(ps.rcPaint.right - ps.rcPaint.left) * (ps.rcPaint.bottom - ps.rcPaint.top)) parts.clear();
    if (parts.empty()) parts.push_back(ps.rcPaint);
    for (auto& r : parts)
        if (!IsRectEmpty(&r)) paint(dc, r);
    EndPaint(window, &ps);
}
void addTip(HWND tooltip, HWND owner, UINT_PTR id, const std::wstring& text, UINT flags, UINT message) {
    TOOLINFOW tool{sizeof tool};
    tool.uFlags = flags | TTF_SUBCLASS;
    tool.hwnd = owner;
    tool.uId = id;
    tool.lpszText = const_cast<wchar_t*>(text.c_str());
    SendMessageW(tooltip, message, 0, reinterpret_cast<LPARAM>(&tool));
}
HWND pageControl(HINSTANCE instance, HWND parent, const wchar_t* type, const std::wstring& title, DWORD style, int id) {
    auto child = CreateWindowExW(0, type, title.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
                                 reinterpret_cast<HMENU>(INT_PTR(id)), instance, nullptr);
    if (!child) throw std::runtime_error("Cannot create native control");
    if (!wcscmp(type, TRACKBAR_CLASSW) || !wcscmp(type, WC_LINK)) paintWhole(child);
    return child;
}

PageLayout::PageLayout(Page& page, PageFonts f, int pageWidth, bool placing)
    : target(page), fonts(f), width(pageWidth), scroll(page.scroll), place(placing), dc(GetDC(page.window)),
      previous(SelectObject(dc, f.font)), titleHeight(height(f.title)), line(height(f.font)), x(ref(16)),
      right(width - ref(16)), left(x + ref(15)), end(right - ref(15)), y(ref(6) - scroll), pad(ref(6)),
      slider(std::max(ref(30), ref(8) + line)), box(line + ref(4)), button(ref(32)) {}
PageLayout::~PageLayout() {
    SelectObject(dc, previous);
    ReleaseDC(target.window, dc);
}
int PageLayout::height(HFONT font) {
    SelectObject(dc, font);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    return metrics.tmHeight;
}
int PageLayout::extent(HFONT font, const std::wstring& s) {
    SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, s.c_str(), int(s.size()), &size);
    return int(size.cx);
}
void PageLayout::put(HWND child, int cx, int cy, int w, int h, bool shown, bool list) {
    if (!place || !child) return;
    bool visible = (GetWindowLongW(child, GWL_STYLE) & WS_VISIBLE) != 0;
    if (!shown) {
        if (visible) ShowWindow(child, SW_HIDE);
        return;
    }
    w = std::max(0, w);
    h = std::max(0, h);
    RECT now{};
    GetWindowRect(child, &now);
    MapWindowPoints(nullptr, target.window, reinterpret_cast<POINT*>(&now), 2);
    // A moved control repaints: copying its old pixels can copy those of a control moved there before it.
    if (now.left != cx || now.top != cy || now.right - now.left != w || (!list && now.bottom - now.top != h))
        SetWindowPos(child, nullptr, cx, cy, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    if (!visible) ShowWindow(child, SW_SHOW);
}
void PageLayout::open(const std::wstring& title, int from, int to) {
    frames.push_back({{from, y, to, 0}, title});
    y += ref(titleTop) + titleHeight + ref(8);
}
void PageLayout::close() {
    y += ref(14);
    frames.back().rect.bottom = y;
    y += ref(12);
}
int PageLayout::fit(const std::wstring& title, int from, int contentRight) {
    return std::max(contentRight, from + ref(titleLeft) + extent(fonts.title, title) + ref(15));
}
void PageLayout::check(HWND control, int from, bool shown, HFONT font, int widest) {
    if (shown) y += ref(6);
    int n = GetWindowTextLengthW(control);
    std::wstring s(n + 1, L'\0');
    s.resize(GetWindowTextW(control, s.data(), n + 1));
    put(control, from, y, std::min(extent(font, s) + ref(40), widest), box, shown);
    if (shown) y += box;
}
int PageLayout::paragraph(const std::wstring& s, int from, int top, int to, COLORREF color, HFONT font) {
    RECT r{from, top, to, top};
    // Most texts are those of the last pass, which measured them already (some notes are long).
    auto known = std::ranges::find_if(target.texts, [&](const PageText& t) {
        return t.font == font && t.rect.right - t.rect.left == to - from && t.format == (DT_WORDBREAK | DT_NOPREFIX) &&
               t.text == s;
    });
    if (known != target.texts.end())
        r.bottom = top + known->rect.bottom - known->rect.top;
    else {
        SelectObject(dc, font);
        DrawTextW(dc, s.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    }
    r.right = to;
    texts.push_back({r, s, color, font});
    return int(r.bottom);
}
int PageLayout::say(const std::wstring& s, int cx, int cy, COLORREF color, HFONT font, int h) {
    int to = cx + extent(font ? font : fonts.font, s);
    texts.push_back({{cx, cy, to, cy + (h ? h : line)}, s, color, font});
    return to;
}
void PageLayout::link(HWND control, int from, int top, int w) {
    SIZE ideal{};
    int h = int(SendMessageW(control, LM_GETIDEALSIZE, w, reinterpret_cast<LPARAM>(&ideal)));
    put(control, from, y + top, w, h);
    y += top + h;
}
int PageLayout::finish() {
    std::swap(target.frames, frames);
    std::swap(target.texts, texts);
    return y + scroll + ref(6);
}
}
