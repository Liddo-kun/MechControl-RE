#pragma once
#include "controller.h"
#include "page.h"
#include <windows.h>
#include <functional>
#include <memory>

namespace mech {
// The V/F Curve tab, a port of Jon's page (VFCURVE/blackbox/vfe-ui.html): the chart, the cards of sliders, the
// corrections and every equation number, with Apply, Restore stock and the readback against the calculation. It
// exists only once the tab is first shown; the tuner opens on the worker, so the window never waits for the driver.
struct VfHost {
    // Sends a command to the worker and returns its sequence. A busy command disables the window like other device
    // operations and returns 0 while another one runs.
    std::function<std::uint64_t(ControlCommand, bool busy)> submit;
    std::function<void(const std::wstring& reason)> report; // opens a GitHub issue about this GPU
    HWND tooltip;                                           // the window's tooltip control
    bool intro = true;                                      // the intro card is shown (settings.json)
};
class VfTab {
public:
    VfTab(HINSTANCE, HWND parent, VfHost);
    ~VfTab();
    VfTab(const VfTab&) = delete;
    VfTab& operator=(const VfTab&) = delete;
    HWND window() const;
    void style(const PageFonts&); // after a DPI or theme change
    void read();                  // the profile bar's Read
    void loaded();                // a profile was loaded: shows its tune and the GPU's curve
    void delivered(std::uint64_t sequence, const ControlResult&);
    void saved(); // the profile bar's Save stored the curve
    // The loaded profile's V/F curve beside the GPU's (Controller::vf_saved), said next to the status; -1 not known.
    void profile(int saved);
    // The status beside the buttons, the readback, the chart's notice and Current Changes, for the UI checks.
    std::wstring text() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
