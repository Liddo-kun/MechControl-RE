#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace mech {
// The V/F Curve tab's read-only tables of every equation number, painted. It scrolls sideways where it is wider than
// the page.
struct VfItem {
    std::wstring text;
    bool muted = false;   // secondary text
    bool line = false;    // starts a line and wraps within its column
    bool value = false;   // a number, in monospace
    bool changed = false; // a number that differs from stock, highlighted
};
struct VfRow {
    std::vector<std::pair<int, std::vector<VfItem>>> spans; // the column each starts at, and what it shows
    bool header = false;
};
struct VfTable {
    std::wstring heading; // above the table; may be empty
    std::string columns;  // per column: 'e' equation number, 'w' description, 'n' number, 's' setting
    std::vector<VfRow> rows;
};
class VfNumbers {
public:
    VfNumbers(HINSTANCE, HWND parent);
    ~VfNumbers();
    VfNumbers(const VfNumbers&) = delete;
    VfNumbers& operator=(const VfNumbers&) = delete;
    HWND window() const { return window_; }
    void set(std::vector<VfTable>);
    void style(HFONT font, HFONT bold, UINT dpi);
    int height(int width); // the window's height at this width

private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT message(UINT, WPARAM, LPARAM);
    int scale(int n) const { return MulDiv(n, dpi_, 96); }
    void layout(int width);
    void paint(HDC);
    void scrolled();
    struct Box {
        RECT r;
        const VfItem* item;
        bool header;
    };
    HWND window_ = nullptr;
    HFONT font_ = nullptr, bold_ = nullptr, mono_ = nullptr;
    UINT dpi_ = 96;
    std::vector<VfTable> tables_;
    std::vector<Box> boxes_;          // laid out, in content coordinates
    std::vector<RECT> grid_, shaded_; // table cell frames; header rows
    std::vector<std::pair<RECT, std::wstring>> headings_;
    int content_ = 0, widest_ = 0, laid_ = -1, x_ = 0;
};
}
