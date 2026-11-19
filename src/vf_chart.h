#pragma once
#include <windows.h>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mech {
// The V/F Curve tab's chart, as on Jon's page (vfe-ui.html): the clock against the point voltage per shown temperature
// column, each card's lines, reference marks, hover values, point selection with the mouse and arrow keys, wheel zoom,
// drag and Reset view. Drawn with GDI+ like the fan curves.
struct VfSeries {
    std::wstring name;
    COLORREF color;
    char mark; // 'f': filled dot (new curve), 'o': ring (stock), '+': cross (reference)
    std::vector<std::optional<double>> y;
};
struct VfLine {
    COLORREF color;
    bool hot; // the card the user works on
    std::vector<std::optional<double>> y;
};
struct VfPlot {
    std::vector<double> mv;      // the point voltages
    std::vector<double> listed;  // the voltages the driver lists, when read
    std::vector<double> offsets; // MHz per point
    double step = 7.5;           // the clock step
    std::vector<VfLine> lines;
    std::vector<VfSeries> series;
    std::wstring axis;               // the clock axis title
    bool relative = false;           // the values are differences
    std::vector<std::wstring> notes; // shown at the top left
};
class VfChart {
public:
    VfChart(HINSTANCE, HWND parent);
    ~VfChart();
    VfChart(const VfChart&) = delete;
    VfChart& operator=(const VfChart&) = delete;
    HWND window() const { return window_; }
    // Keeps the view and the selection while the points stay the same.
    void set(VfPlot);
    void reset_view();
    void clear_selection();
    void dpi(UINT);
    // Up or down on the selected points, in clock steps; the tab changes their offsets and sets the plot again.
    std::function<void(const std::set<int>& points, int steps)> bump;

private:
    struct View {
        double x0, x1, y0, y1;
    };
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT message(UINT, WPARAM, LPARAM);
    void paint(HDC);
    float px(float n) const { return n * dpi_ / 96; }
    RECT plot() const;
    View view() const;
    int nearest(double mv) const;
    int hit(POINT) const; // 1 Reset view, 2 up, 3 down, 4 clear selection
    void pick(POINT, bool shift, bool control);
    void hover(POINT);
    std::wstring selection_text() const;
    HWND window_ = nullptr;
    UINT dpi_ = 96;
    VfPlot plot_;
    std::optional<View> view_; // none: fitted to what is shown
    View shown_{};             // as last drawn
    std::set<int> selected_;
    int anchor_ = -1, hover_ = -1;
    POINT mouse_{};
    struct Drag {
        POINT from;
        View view;
        bool moved = false;
    };
    std::optional<Drag> drag_;
    RECT buttons_[5]{}; // as hit() numbers them; empty where not shown
};
}
