#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace mech {
// The V/F Curve tab's tables of every equation number, as on Jon's page: painted, with one edit box over the number
// being edited and a menu for a comparison's or choice's setting. It scrolls sideways where it is wider than the page.
struct VfCell {
    std::wstring text;
    bool changed = false;              // differs from stock
    std::vector<std::wstring> choices; // a setting chosen from a menu; the text is one of them
};
struct VfItem {
    std::wstring text;
    int cell = -1;      // the cell this item shows, or -1 for text
    bool muted = false; // secondary text
    bool line = false;  // starts a line and wraps within its column
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
    void set(std::vector<VfTable>, std::vector<VfCell>);
    void cells(std::vector<VfCell>); // new texts for the same tables
    void style(HFONT font, HFONT bold, UINT dpi);
    int height(int width); // the window's height at this width
    // A number typed into a cell (empty: the stock value), or the index of a chosen setting.
    std::function<void(int cell, const std::wstring& text)> edited;

private:
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK editProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT message(UINT, WPARAM, LPARAM);
    int scale(int n) const { return MulDiv(n, dpi_, 96); }
    void layout(int width);
    void paint(HDC);
    void open(int cell);
    void close(bool commit);
    void scrolled();
    struct Box {
        RECT r;
        int cell; // -1: text
        const VfItem* item;
        bool header;
    };
    HWND window_ = nullptr, edit_ = nullptr;
    HFONT font_ = nullptr, bold_ = nullptr, mono_ = nullptr;
    UINT dpi_ = 96;
    std::vector<VfTable> tables_;
    std::vector<VfCell> cells_;
    std::vector<Box> boxes_;          // laid out, in content coordinates
    std::vector<RECT> grid_, shaded_; // table cell frames; header rows
    std::vector<std::pair<RECT, std::wstring>> headings_;
    int content_ = 0, laid_ = -1, x_ = 0, editing_ = -1;
};
}
