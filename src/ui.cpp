#include "worker.h"
#include "fan_chart.h"
#include "config_text.h"
#ifdef MECHCONTROL_WITH_CLI
#include "cli_server.h"
#endif
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shldisp.h>
#include <exdisp.h>
#include <commdlg.h>
#include <tlhelp32.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <iomanip>
#include <sstream>
#include <tuple>
#ifdef MECHCONTROL_PAGE_CHECK
#include <dwmapi.h>
#include "../tests/ui_pages_fixture.h"
#endif

namespace {
using namespace mech;
using namespace mech::config;
constexpr wchar_t mainClass[] = L"MechControl.Native.Main";
constexpr UINT trayMessage = WM_APP + 2, fanEditMessage = WM_APP + 4;
// GUID_CONSOLE_DISPLAY_STATE and GUID_ACDC_POWER_SOURCE.
constexpr GUID displayState{0x6FE69556, 0x704A, 0x47A0, {0x8F, 0x24, 0xC2, 0x8D, 0x93, 0x6F, 0xDA, 0x47}},
    powerSource{0x5D3E9A59, 0xE9D5, 0x4B00, {0xA6, 0xBD, 0xFF, 0x34, 0xFF, 0x51, 0x65, 0x48}};
enum Id {
    tabsId = 100,
    saveId = 110,
    defaultsId,
    renameId,
    clearId,
    showId,
    exitId,
    readId,
    slotId = 120,
    defaultId = 140,
    pl1Id = 200,
    pl2Id,
    pl4Id,
    tempId,
    tgpId,
    dbId,
    nvId,
    msId,
    inputId,
    dbOnId,
    unlockId,
    dlppcId,
    powerApplyId,
    powerReadId,
    advancedId,
    bypassId,
    coolerFlagId,
    cpuSupportId,
    gpuSupportId,
    unlockSupportId,
    boardSupportId,
    bypassSupportId,
    boostId = 300,
    switchId,
    switchMsId,
    switchApplyId,
    curveId,
    linkId,
    cpuChartId,
    gpuChartId,
    fanApplyId,
    fanReadId,
    debugCpuId,
    debugGpuId,
    boostApplyId,
    fanSupportId,
    protectionId = 400,
    thresholdId,
    reduceId,
    fanBoostId,
    showWindowId,
    protectionApplyId,
    resetId,
    testId,
    detailsId,
    thermalSupportId,
    batteryModeId = 500,
    batteryApplyId,
    batteryTextId,
    batterySupportId,
    zoneBaseId = 600,
    idleId = 660,
    dimmingId,
    lightingReadId,
    devicesId = 800,
    scanId,
    connectId,
    autoConnectId,
    coolerFanOnId,
    coolerFanId,
    coolerFanApplyId,
    coolerPumpOnId,
    coolerPumpId,
    voltageId,
    coolerPumpApplyId,
    coolerRgbOnId,
    coolerRId,
    coolerGId,
    coolerBId,
    rgbModeId,
    coolerRgbApplyId
};
std::wstring w(const std::string& value) { return wide(value); }
std::wstring text(HWND window) {
    int n = GetWindowTextLengthW(window);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(window, s.data(), n + 1);
    s.resize(n);
    return s;
}
std::wstring decimal(double value, int places = 1) { return wide(decimal_text(value, places)); }
// Up to three decimals, without trailing zeros.
std::wstring compact(double value) { return wide(decimal_text(value, 3, true)); }
// Percent-encodes a URL query value.
std::wstring encode(const std::wstring& value) {
    std::wstring out;
    for (unsigned char c : utf8(value)) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_' ||
            c == '.')
            out += wchar_t(c);
        else {
            wchar_t hex[4];
            swprintf_s(hex, L"%%%02X", c);
            out += hex;
        }
    }
    return out;
}
// Through Explorer, so the browser does not start elevated like this process.
void openUnelevated(const std::wstring& address) {
    try {
        auto windows = winrt::create_instance<IShellWindows>(CLSID_ShellWindows, CLSCTX_LOCAL_SERVER);
        VARIANT desktop{}, none{};
        desktop.vt = VT_I4;
        desktop.lVal = CSIDL_DESKTOP;
        long handle = 0;
        winrt::com_ptr<IDispatch> dispatch, background, application;
        if (windows->FindWindowSW(&desktop, &none, SWC_DESKTOP, &handle, SWFO_NEEDDISPATCH, dispatch.put()) != S_OK)
            throw winrt::hresult_error(E_FAIL);
        winrt::com_ptr<IShellBrowser> browser;
        winrt::check_hresult(dispatch.as<IServiceProvider>()->QueryService(
            SID_STopLevelBrowser, __uuidof(IShellBrowser), browser.put_void()));
        winrt::com_ptr<IShellView> view;
        winrt::check_hresult(browser->QueryActiveShellView(view.put()));
        winrt::check_hresult(view->GetItemObject(SVGIO_BACKGROUND, __uuidof(IDispatch), background.put_void()));
        winrt::check_hresult(background.as<IShellFolderViewDual>()->get_Application(application.put()));
        auto url = SysAllocString(address.c_str());
        auto result = application.as<IShellDispatch2>()->ShellExecute(url, none, none, none, none);
        SysFreeString(url);
        winrt::check_hresult(result);
    } catch (const winrt::hresult_error&) {
        throw std::runtime_error("Could not open the browser. Report at github.com/Liddo-kun/MechControl-RE/issues");
    }
}
constexpr COLORREF okColor = RGB(46, 125, 50), warnColor = RGB(230, 124, 0), errorColor = RGB(198, 40, 40),
                   frameColor = RGB(180, 180, 180);
// Green up to normal, amber midway, red from limiting; unknown keeps the normal text color.
COLORREF tint(std::optional<double> value, double normal, double limiting) {
    if (!value) return GetSysColor(COLOR_BTNTEXT);
    auto mix = [](COLORREF a, COLORREF b, double x) {
        auto c = [x](int p, int q) { return int(p + (q - p) * x); };
        return RGB(c(GetRValue(a), GetRValue(b)), c(GetGValue(a), GetGValue(b)), c(GetBValue(a), GetBValue(b)));
    };
    double f = std::clamp((*value - normal) / (limiting - normal), 0.0, 1.0);
    return f < 0.5 ? mix(okColor, warnColor, f * 2) : mix(warnColor, errorColor, f * 2 - 1);
}
// GPU Limits (live): the four headings, then the seven rows, each with the reference tooltip.
constexpr const wchar_t* liveCaptions[]{
    L"Setting",       L"Now",           L"Of limit",           L"Controller cap (above clock)",
    L"GPU clock",     L"GPU power",     L"Core rails",         L"Memory and other",
    L"NVVDD current", L"MSVDD current", L"Board input current"};
constexpr const wchar_t* railTip =
    L"The current controller lowers its clock cap until neither rail's current would exceed its limit at that cap.";
constexpr const wchar_t* liveTips[]{
    L"The active limit (for GPU power, the enforced limit: TGP + Dynamic Boost, or the driver's lower budget).",
    L"The driver's latest reading (it updates every 20 ms); refreshed every second while this tab is shown.",
    L"The reading as a share of its setting.",
    L"The highest clock the driver controller enforcing this limit allows (power or current controller); the lower cap applies. In brackets: how far the cap is above the actual clock. Board input has no controller: the GPU's inline power controller (IPC) slows the clock below the caps.",
    L"The graphics clock (NVML) against the driver's maximum.",
    L"Total GPU power (TGP) against the enforced limit.",
    L"The rails the power controller manages and predicts (NVVDD and MSVDD supply).",
    L"GPU power outside the core rails: memory, PCIe and the rest of the board. The core gets the power limit minus this, so raising TGP raises core power by less than the full amount.",
    railTip,
    railTip,
    L"Board input has no controller or cap: the GPU's inline power controller (IPC) slows the clock below every cap. IPC active: the share of time it acted since the last reading. It also clips power peaks, so it can act without board input."};
constexpr const wchar_t* thermalDescription =
    L"For machines that depend on an external / liquid cooler to run their high-power profile. Set the trigger above the EC GPU temperature normally reached with the cooler working. If the average of the last 5 one-second readings reaches the trigger, the failsafe trips, requests this model's minimum GPU power and alerts you; single noisy readings are averaged out. Optional actions also reduce CPU power and turn on fan boost. To recover, press Reset (restores the active profile) or load the profile that matches your cooling, e.g. an air-cooling profile.";
constexpr const wchar_t* thermalDetails =
    L"EC temperature checks run about once per second, including in the tray. A trip needs at least three readings and averages up to five, adding about two seconds of delay. Five invalid/unavailable readings trip protection; a sleeping GPU can cause a conservative trip.\r\n\r\nWhile the failsafe is enabled, Reset requires a fresh valid read and an average below the trigger. Loading a profile clears the trip before applying its settings. Minimum EC/driver requests are not proof of physical power enforcement or cooling. Fan boost is the laptop firmware mode, not the external pump or fans.\r\n\r\nThere is no protection while the application/OS cannot run or hardware cannot accept writes. This is not real-time protection; a plausible frozen sensor cannot be detected from this register alone. No Bluetooth cooler connection is required. Alerts and sound are best effort; closing the alert does not reset protection.";
// Trip times are ISO 8601 with a UTC offset (none means local). Shown in local time, as in the reference.
std::wstring localTime(const std::string& iso) {
    SYSTEMTIME t{};
    unsigned hours = 0, minutes = 0;
    if (sscanf_s(iso.c_str(), "%hu-%hu-%huT%hu:%hu", &t.wYear, &t.wMonth, &t.wDay, &t.wHour, &t.wMinute) != 5)
        return w(iso);
    if (auto zone = iso.find_first_of("Z+-", 16); zone != std::string::npos) {
        if (iso[zone] != 'Z') sscanf_s(iso.c_str() + zone + 1, "%u:%u", &hours, &minutes);
        FILETIME f{};
        ULARGE_INTEGER u{};
        SYSTEMTIME utc{};
        SystemTimeToFileTime(&t, &f);
        u.LowPart = f.dwLowDateTime;
        u.HighPart = f.dwHighDateTime;
        u.QuadPart += ULONGLONG((iso[zone] == '-' ? 1 : -1) * (hours * 60LL + minutes) * 600000000LL);
        f = {u.LowPart, u.HighPart};
        if (!FileTimeToSystemTime(&f, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &t)) return w(iso);
    }
    wchar_t date[80]{}, clock[80]{};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &t, nullptr, date, 80, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &t, nullptr, clock, 80);
    return std::wstring(date) + L" " + clock;
}
std::wstring dlppcTip(const Json& detected) {
    std::wstring tip =
        L"DLPPC (likely Deep Learning Power/Performance Controller) is an NVIDIA firmware controller that uses a neural-network power model to pick the GPU memory clock under load. When the GPU is power-limited it caps memory a step down (P2) to give the core more of the power limit. Disabling it keeps memory at full speed (P0) in games; idle downclocking is unaffected. Applies now and at every startup, and is reapplied after a GPU restart.";
    if (!detected.is_object()) return tip;
    std::wstring controllers;
    auto mask = detected.value("controllers", 0u);
    for (int i = 0; i < 32; ++i)
        if (mask & (1u << i)) controllers += (controllers.empty() ? L"" : L", ") + std::to_wstring(i);
    return tip + L"\nDetected: PERF_CF policy " + std::to_wstring(detected.value("policy", 0)) + L", controller " +
           controllers + L"; currently " + (detected.value("active", false) ? L"enabled" : L"disabled") + L".";
}
struct Row {
    HWND label = nullptr, control = nullptr, number = nullptr;
    int id = 0, y = 0, height = 36;
    bool slider = false;
};
struct Text {
    RECT rect;
    std::wstring text;
    COLORREF color;
    HFONT font = nullptr; // No font: the normal one.
};
// The note of a control withheld on this model: what the issue it opens is about, what else to send, and why.
struct Support {
    const wchar_t *subject = L"", *also = L"";
    std::wstring reason;
    COLORREF color = GetSysColor(COLOR_BTNTEXT);
};
// The Battery page stacks rows; the others paint titled section frames and notes.
bool framed(int page) { return page >= 0 && page < 7 && page != 3; }
struct Page {
    HWND window = nullptr;
    std::vector<Row> rows;
    int height = 0, scroll = 0;
    std::vector<std::pair<RECT, const wchar_t*>> frames;
    std::vector<Text> texts;
};

// Small native edit dialog, shared only by the existing slot rename workflow.
struct RenameDialog {
    std::wstring value;
    HFONT font = nullptr;
    HWND edit = nullptr;
};
INT_PTR CALLBACK renameProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto data = reinterpret_cast<RenameDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        data = reinterpret_cast<RenameDialog*>(lp);
        SetWindowLongPtrW(window, DWLP_USER, lp);
        SetWindowTextW(window, L"Rename Profile");
        auto scale = [dpi = GetDpiForWindow(window)](int n) { return MulDiv(n, dpi, 96); };
        RECT owner{};
        GetWindowRect(GetParent(window), &owner);
        SetWindowPos(window, nullptr, (owner.left + owner.right - scale(360)) / 2,
                     (owner.top + owner.bottom - scale(150)) / 2, scale(360), scale(150), SWP_NOZORDER);
        CreateWindowW(L"STATIC", L"Profile name:", WS_CHILD | WS_VISIBLE, scale(12), scale(12), scale(330), scale(22),
                      window, nullptr, nullptr, nullptr);
        data->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", data->value.c_str(),
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, scale(12), scale(38),
                                     scale(330), scale(26), window, reinterpret_cast<HMENU>(10), nullptr, nullptr);
        SendMessageW(data->edit, EM_SETLIMITTEXT, 256, 0);
        CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, scale(176), scale(80),
                      scale(80), scale(28), window, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, scale(264), scale(80), scale(80),
                      scale(28), window, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
        EnumChildWindows(
            window,
            [](HWND child, LPARAM font) -> BOOL {
                SendMessageW(child, WM_SETFONT, WPARAM(font), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(data->font));
        SetFocus(data->edit);
        SendMessageW(data->edit, EM_SETSEL, 0, -1);
        return FALSE;
    }
    if (message == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
        if (LOWORD(wp) == IDOK) data->value = text(data->edit);
        EndDialog(window, LOWORD(wp));
        return TRUE;
    }
    return FALSE;
}
std::optional<std::wstring> rename(HWND owner, std::wstring name, HFONT font) {
    struct Template {
        DLGTEMPLATE dialog;
        WORD menu = 0, type = 0, title = 0;
    } layout{};
    layout.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
    layout.dialog.cx = 240;
    layout.dialog.cy = 85;
    RenameDialog data{std::move(name), font};
    if (DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &layout.dialog, owner, renameProcedure,
                                reinterpret_cast<LPARAM>(&data)) == IDOK)
        return data.value;
    return {};
}

struct ModelDialog {
    const std::vector<std::filesystem::path>& paths;
    HFONT font;
    HWND list = nullptr;
    int chosen = -1;
};
INT_PTR CALLBACK modelProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto data = reinterpret_cast<ModelDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        data = reinterpret_cast<ModelDialog*>(lp);
        SetWindowLongPtrW(window, DWLP_USER, lp);
        SetWindowTextW(window, L"MechControl — Select Model");
        auto scale = [dpi = GetDpiForWindow(window)](int n) { return MulDiv(n, dpi, 96); };
        RECT owner{};
        GetWindowRect(GetParent(window), &owner);
        SetWindowPos(window, nullptr, (owner.left + owner.right - scale(760)) / 2,
                     (owner.top + owner.bottom - scale(470)) / 2, scale(760), scale(470), SWP_NOZORDER);
        CreateWindowW(
            L"STATIC", L"Automatic detection failed. Select your model's full profile path, or cancel to exit.",
            WS_CHILD | WS_VISIBLE, scale(12), scale(12), scale(720), scale(40), window, nullptr, nullptr, nullptr);
        data->list = CreateWindowW(L"LISTBOX", L"Model profile paths",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL | WS_HSCROLL |
                                       LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                   scale(12), scale(56), scale(720), scale(320), window, reinterpret_cast<HMENU>(10),
                                   nullptr, nullptr);
        auto ok = CreateWindowW(L"BUTTON", L"&Use selected profile",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, scale(384), scale(394),
                                scale(220), scale(30), window, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"&Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, scale(616), scale(394), scale(116),
                      scale(30), window, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
        EnumChildWindows(
            window,
            [](HWND child, LPARAM font) -> BOOL {
                SendMessageW(child, WM_SETFONT, WPARAM(font), TRUE);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(data->font));
        auto dc = GetDC(data->list);
        auto previous = SelectObject(dc, data->font);
        int width = 0;
        for (auto& path : data->paths) {
            auto name = path.wstring();
            SendMessageW(data->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
            SIZE size{};
            GetTextExtentPoint32W(dc, name.c_str(), int(name.size()), &size);
            width = std::max(width, int(size.cx));
        }
        SelectObject(dc, previous);
        ReleaseDC(data->list, dc);
        SendMessageW(data->list, LB_SETHORIZONTALEXTENT, width + scale(12), 0);
        EnableWindow(ok, FALSE);
        SetFocus(data->list);
        return FALSE;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(window, IDCANCEL);
            return TRUE;
        }
        if (LOWORD(wp) == 10 && HIWORD(wp) == LBN_SELCHANGE) {
            EnableWindow(GetDlgItem(window, IDOK), SendMessageW(data->list, LB_GETCURSEL, 0, 0) != LB_ERR);
            return TRUE;
        }
        if (LOWORD(wp) == IDOK || (LOWORD(wp) == 10 && HIWORD(wp) == LBN_DBLCLK)) {
            auto selected = SendMessageW(data->list, LB_GETCURSEL, 0, 0);
            if (selected >= 0 && size_t(selected) < data->paths.size()) {
                data->chosen = int(selected);
                EndDialog(window, IDOK);
            }
            return TRUE;
        }
    }
    return FALSE;
}

class Application {
public:
    explicit Application(HINSTANCE instance, WorkerOptions options = {})
        : instance_(instance), log_(executable_directory() / L"mechcontrol.log"),
          delivery_(std::make_shared<Delivery>()), workerOptions_(std::move(options)) {
        checkOem();
    }
    ~Application() {
        shutdown();
        for (auto font : {font_, bold_, title_, header_, heading_})
            if (font) DeleteObject(font);
        for (auto icon : slotIcons_)
            if (icon) DestroyIcon(icon);
        for (auto icon : animation_) DestroyIcon(icon);
    }
    HWND create() {
        auto dpi = GetDpiForSystem();
        return CreateWindowExW(WS_EX_CONTROLPARENT, mainClass, L"MechControl", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(960, dpi, 96), MulDiv(760, dpi, 96), nullptr,
                               nullptr, instance_, this);
    }
#ifdef MECHCONTROL_PAGE_CHECK
    void checkPages(const std::filesystem::path&, const std::shared_ptr<UiPageState>&);
#endif
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (!app) return DefWindowProcW(window, message, wp, lp);
        try {
            return app->message(message, wp, lp);
        } catch (const std::exception& e) {
            app->log_.write(e.what());
            app->setStatus(w(e.what()));
            if (GetTickCount64() - app->lastError_ > 300000 || !app->lastError_) {
                app->lastError_ = GetTickCount64();
                MessageBoxW(window, (w(e.what()) + L"\nSee mechcontrol.log for details.").c_str(),
                            L"MechControl — Error", MB_OK | MB_ICONERROR);
            }
            return message == WM_CREATE ? -1 : 0;
        }
    }

private:
    static LRESULT CALLBACK pageProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto app = reinterpret_cast<Application*>(data);
        if (message == WM_COMMAND || message == WM_HSCROLL || message == WM_NOTIFY)
            return SendMessageW(app->window_, message, wp, lp);
        // Up-down controls also send WM_VSCROLL; only the page's own scroll bar scrolls it.
        if ((message == WM_VSCROLL && !lp) || message == WM_MOUSEWHEEL) {
            app->scroll(window, message, wp);
            return 0;
        }
        if (message == WM_SETFOCUS) {
            auto next = GetNextDlgTabItem(window, nullptr, FALSE);
            if (next) SetFocus(next);
            return 0;
        }
        if (message == WM_PARENTNOTIFY && LOWORD(wp) == WM_CREATE) return 0;
        if (int page = app->index(window); framed(page)) {
            if (message == WM_ERASEBKGND) return 1;
            if (message == WM_PAINT) {
                PAINTSTRUCT ps{};
                auto dc = BeginPaint(window, &ps);
                app->paintPage(page, dc, ps.rcPaint);
                EndPaint(window, &ps);
                return 0;
            }
            // A static is transparent to the mouse; the live table's tooltips need it.
            if (message == WM_NCHITTEST && page == 0) {
                auto hit = DefSubclassProc(window, message, wp, lp);
                return hit == HTTRANSPARENT ? HTCLIENT : hit;
            }
        }
        return DefSubclassProc(window, message, wp, lp);
    }
    static LRESULT CALLBACK focusProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto app = reinterpret_cast<Application*>(data);
        if (message == WM_SETFOCUS) app->reveal(window);
        // Flat buttons shade while the mouse is over them.
        if (message == WM_MOUSEMOVE && app->hot_ != window && app->flat(GetDlgCtrlID(window))) {
            app->hot_ = window;
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, window};
            TrackMouseEvent(&t);
            InvalidateRect(window, nullptr, FALSE);
        }
        if (message == WM_MOUSELEAVE && app->hot_ == window) {
            app->hot_ = nullptr;
            InvalidateRect(window, nullptr, FALSE);
        }
        return DefSubclassProc(window, message, wp, lp);
    }
    static LRESULT CALLBACK alertProcedure(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto app = reinterpret_cast<Application*>(data);
        if (message == WM_COMMAND) {
            app->show();
            TabCtrl_SetCurSel(app->tabs_, 2);
            app->selectPage();
            DestroyWindow(window);
            return 0;
        }
        if (message == WM_CLOSE) {
            DestroyWindow(window);
            return 0;
        }
        // The alert is a STATIC, which would draw its title over the message.
        if (message == WM_PAINT) {
            PAINTSTRUCT ps{};
            auto dc = BeginPaint(window, &ps);
            FillRect(dc, &ps.rcPaint, GetSysColorBrush(COLOR_BTNFACE));
            EndPaint(window, &ps);
            return 0;
        }
        if (message == WM_NCDESTROY) app->alert_ = nullptr;
        return DefSubclassProc(window, message, wp, lp);
    }
    HWND control(HWND parent, const wchar_t* type, const std::wstring& title, DWORD style, int id) {
        auto child = CreateWindowExW(0, type, title.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
                                     reinterpret_cast<HMENU>(INT_PTR(id)), instance_, nullptr);
        if (!child) throw std::runtime_error("Cannot create native control");
        if (id) controls_[id] = child;
        SetWindowSubclass(child, focusProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
        return child;
    }
    HWND get(int id) const {
        auto i = controls_.find(id);
        return i == controls_.end() ? nullptr : i->second;
    }
    int index(HWND page) const {
        for (int i = 0; i < 7; ++i)
            if (pages_[i].window == page) return i;
        return -1;
    }
    void set(int id, const std::wstring& value) {
        if (text(get(id)) != value) {
            SetWindowTextW(get(id), value.c_str());
            InvalidateRect(get(id), nullptr, TRUE);
        }
    }
    // A new GitHub issue with the title, the note and this machine
    // filled in; the user attaches the files and submits it.
    std::wstring issueUrl(const Support& s) const {
        auto model = w(state_.value("model", std::string{}));
        auto identity = identity_.is_object() ? identity_ : Json::object();
        auto body =
            s.reason + L"\n\n- Model: " + model + L"\n- Baseboard: " + w(identity.value("baseboard", std::string{})) +
            L"\n- CPU: " + w(identity.value("cpu", std::string{})) +
            L"\n\nPlease attach `mechcontrol.log` (beside MechControl.exe) and a diagnostic report made with `MechControl.exe --diagnose report.json`" +
            (*s.also ? L", and add " + std::wstring(s.also) : L"") + L".";
        return L"https://github.com/Liddo-kun/MechControl-RE/issues/new?title=" +
               encode(s.subject + (L" support for " + model)) + L"&body=" + encode(body);
    }
    // A support note is a SysLink, so that its "open an issue" link works. Hidden while it has nothing to say.
    void support(int page, int id, const wchar_t* subject, const wchar_t* also = L"") {
        ShowWindow(control(pages_[page].window, WC_LINK, L"", WS_TABSTOP | LWS_TRANSPARENT | LWS_NOPREFIX, id),
                   SW_HIDE);
        support_[id] = {subject, also};
    }
    // Sets why a control is unavailable, followed by the request to report it. A note does not ask when the cause is
    // another disabled control, whose own note does.
    void explain(int id, const std::wstring& reason, bool ask = true) {
        auto& s = support_.at(id);
        s.reason = reason;
        set(id, reason.empty() || !ask ? reason
                                       : reason + L" To get it supported, <a>open an issue</a> with mechcontrol.log" +
                                             (*s.also ? L" and " + std::wstring(s.also) : L"") + L".");
        if (reason.empty()) ShowWindow(get(id), SW_HIDE);
    }
    // Places a framed page's support note below y+top and returns its bottom, or y when it is not shown.
    int supportNote(int id, bool shown, COLORREF color, int x, int y, int top, int width, bool apply) {
        auto link = get(id);
        auto& s = support_.at(id);
        if (!shown || s.reason.empty()) {
            if (apply) ShowWindow(link, SW_HIDE);
            return y;
        }
        s.color = color;
        SIZE ideal{};
        int height = int(SendMessageW(link, LM_GETIDEALSIZE, width, reinterpret_cast<LPARAM>(&ideal)));
        if (apply) {
            ShowWindow(link, SW_SHOW);
            place(link, x, y + top, width, height);
        }
        return y + top + height;
    }
    int scale(int n) const { return MulDiv(n, dpi_, 96); }
    // Power page metrics are the reference PowerTab's pixel constants, which it drew unscaled on its 144 DPI
    // screen. Scaling them from 144 DPI keeps the same proportions to the text at every DPI.
    int ref(int n) const { return MulDiv(n, dpi_, 144); }
    void place(HWND child, int x, int y, int width, int height) {
        MoveWindow(child, x, y, std::max(0, width), std::max(0, height), TRUE);
    }
    Row& row(int page, int id, const wchar_t* title, const wchar_t* type, DWORD style, int height = 36) {
        auto& p = pages_[page];
        Row r;
        r.id = id;
        r.y = p.height;
        r.height = height;
        r.control = control(p.window, type, title, style, id);
        p.rows.push_back(r);
        p.height += height + 6;
        return p.rows.back();
    }
    void label(int page, int id, const wchar_t* title, int height = 48) {
        row(page, id, title, L"EDIT", ES_MULTILINE | ES_READONLY | WS_VSCROLL, height);
    }
    void button(int page, int id, const wchar_t* title) { row(page, id, title, L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON); }
    void check(int page, int id, const wchar_t* title) {
        row(page, id, title, L"BUTTON", WS_TABSTOP | BS_AUTOCHECKBOX, 28);
    }
    void combo(int page, int id, const wchar_t* title, std::initializer_list<const wchar_t*> choices) {
        auto& r = row(page, id, L"", WC_COMBOBOXW, WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL);
        if (*title) r.label = control(pages_[page].window, L"STATIC", title, 0, 0);
        for (auto choice : choices) SendMessageW(r.control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice));
        SendMessageW(r.control, CB_SETCURSEL, 0, 0);
    }
    void slider(int page, int id, const wchar_t* title, int minimum, int maximum, int initial) {
        // Positional sliders have ticks and a formatted value label instead of an edit box.
        bool ticks = positional(id);
        auto& r = row(page, id, title, TRACKBAR_CLASSW, WS_TABSTOP | (ticks ? TBS_AUTOTICKS : TBS_NOTICKS), 44);
        r.slider = true;
        r.label = control(pages_[page].window, L"STATIC", title, SS_NOPREFIX, 0);
        // A trackbar starts at 0..100; clearing it lets value() set the ticks and page size for that range too.
        if (ticks) {
            r.number = control(pages_[page].window, L"STATIC", L"", SS_NOPREFIX, id + 1000);
            SendMessageW(r.control, TBM_SETRANGE, FALSE, 0);
        } else {
            r.number = control(pages_[page].window, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
                               id + 1000);
            SendMessageW(r.number, EM_SETLIMITTEXT, 9, 0);
        }
        value(id, initial, minimum, maximum);
    }
    // A color's R, G and B: edit boxes base+1001..base+1003 with up-down buddies base+1..base+3.
    void rgb(int page, int base) {
        for (int n = 1; n <= 3; ++n) {
            auto box = control(pages_[page].window, L"EDIT", L"0", WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_RIGHT,
                               base + n + 1000);
            SendMessageW(box, EM_SETLIMITTEXT, 3, 0);
            auto spin = control(pages_[page].window, UPDOWN_CLASSW, L"",
                                UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, base + n);
            SendMessageW(spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(box), 0);
            SendMessageW(spin, UDM_SETRANGE32, 0, 255);
        }
    }
    void tip(UINT_PTR id, const std::wstring& text, UINT flags = 0, UINT message = TTM_ADDTOOLW) {
        TOOLINFOW tool{sizeof tool};
        tool.uFlags = flags | TTF_SUBCLASS;
        tool.hwnd = pages_[0].window;
        tool.uId = id;
        tool.lpszText = const_cast<wchar_t*>(text.c_str());
        SendMessageW(tooltip_, message, 0, reinterpret_cast<LPARAM>(&tool));
    }
    void initialize() {
        dpi_ = GetDpiForWindow(window_);
        updating_ = true;
        tabs_ = control(window_, WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS, tabsId);
        const wchar_t* names[]{L"Power",    L"Fan Control",   L"Thermal Failsafe", L"Battery",
                               L"Lighting", L"Liquid Cooler", L"System Info"};
        for (int i = 0; i < 7; ++i) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t*>(names[i]);
            TabCtrl_InsertItem(tabs_, i, &item);
            pages_[i].window = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"",
                                               WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0,
                                               window_, nullptr, instance_, nullptr);
            SetWindowPos(pages_[i].window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SetWindowSubclass(pages_[i].window, pageProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
        }
        for (int i = 0; i < 5; ++i)
            control(window_, L"BUTTON", L"Profile " + std::to_wstring(i + 1), WS_TABSTOP | BS_OWNERDRAW, slotId + i);
        control(window_, L"BUTTON", L"&Save", WS_TABSTOP | BS_OWNERDRAW, saveId);
        control(window_, L"BUTTON", L"&Defaults ▼", WS_TABSTOP | BS_OWNERDRAW, defaultsId);
        control(window_, L"BUTTON", L"&Read", WS_TABSTOP | BS_OWNERDRAW, readId);
        slider(0, pl1Id, L"PL1 (Sustained)", 0, 1, 0);
        slider(0, pl2Id, L"PL2 (Burst)", 0, 1, 0);
        slider(0, pl4Id, L"PL4 (Peak)", 0, 1, 0);
        slider(0, tempId, L"CPU Temp Limit", 0, 1, 0);
        slider(0, tgpId, L"GPU TGP (before Dynamic Boost)", 0, 1, 0);
        check(0, unlockId, L"Unlock GPU power / current limits");
        check(0, dlppcId, L"Disable DLPPC (AI-controlled GPU memory clock)");
        check(0, dbOnId, L"Enable Dynamic Boost");
        slider(0, dbId, L"Dynamic Boost Max", 0, 1, 0);
        slider(0, nvId, L"NVVDD / Core current limit", 0, 1, 0);
        slider(0, msId, L"MSVDD / Fabric current limit", 0, 1, 0);
        slider(0, inputId, L"Board input current limit", 0, 1, 0);
        SendMessageW(get(inputId), TBM_SETLINESIZE, 0, 100); // Board input moves in milliamps; arrow keys step 0.1 A.
        button(0, powerApplyId, L"&Apply");
        button(0, powerReadId, L"Read from EC");
        row(0, advancedId, L"<a>Show advanced controls</a>", WC_LINK, WS_TABSTOP | LWS_TRANSPARENT);
        support(0, cpuSupportId, L"CPU power control");
        support(0, gpuSupportId, L"GPU power control");
        support(0, unlockSupportId, L"NVIDIA GPU unlock");
        support(0, boardSupportId, L"Board input current limit");
        check(0, bypassId, L"Bypass EC power throttling");
        support(0, bypassSupportId, L"EC power-limit bypass", L"a link to your model's BIOS/EC update");
        check(0, coolerFlagId, L"Tell the EC a liquid cooler is connected");
        tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr,
                                   instance_, nullptr);
        for (UINT_PTR i = 0; i < std::size(liveTips); ++i)
            tip(i + 1, liveTips[i]); // Rectangles follow the painted table.
        tip(reinterpret_cast<UINT_PTR>(get(dlppcId)), dlppcTip_ = dlppcTip(nullptr), TTF_IDISHWND);
        tip(reinterpret_cast<UINT_PTR>(get(bypassId)),
            L"Turns off on sleep, display off, AC loss, a failsafe trip and exit. Not saved.", TTF_IDISHWND);
        tip(reinterpret_cast<UINT_PTR>(get(coolerFlagId)),
            L"The flag the OEM app sets when its cooler connects. Saved with the profile. Does not connect the cooler.",
            TTF_IDISHWND);
        for (int id : {nvId, msId, inputId})
            tip(reinterpret_cast<UINT_PTR>(get(id)),
                (L"Applies live within the active ceiling. The unlock allows up to " +
                 std::wstring(id == inputId ? L"154" : L"140") +
                 L"% of the stock current; raising an older ceiling restarts the GPU."),
                TTF_IDISHWND);
        check(1, boostId, L"Enable Fan Boost");
        button(1, boostApplyId, L"Apply");
        check(1, switchId, L"Enable Fan Switch Speed");
        slider(1, switchMsId, L"Switch Speed", 1, 127, 3);
        button(1, switchApplyId, L"Apply");
        check(1, curveId, L"Custom Fan Curve");
        check(1, linkId, L"Link Fans");
        support(1, fanSupportId, L"Fan control");
        row(1, cpuChartId, L"CPU Fan Curve", chartClass, WS_BORDER);
        row(1, gpuChartId, L"GPU Fan Curve", chartClass, WS_BORDER);
        button(1, fanApplyId, L"Apply");
        button(1, fanReadId, L"Read from EC");
        row(1, debugCpuId, L"CPU (Sent to EC)", chartClass, WS_BORDER);
        row(1, debugGpuId, L"GPU (Sent to EC)", chartClass, WS_BORDER);
        support(2, thermalSupportId, L"Thermal failsafe");
        check(2, protectionId, L"Enable thermal failsafe");
        slider(2, thresholdId, L"GPU trigger temperature", 50, 95, 70);
        SendMessageW(get(thresholdId), TBM_SETPAGESIZE, 0,
                     1); // As in the reference, a click on the track moves the trigger one degree.
        check(2, reduceId, L"Also drop CPU power limits (PL1 / PL2 / PL4) to minimum");
        check(2, fanBoostId, L"Turn on fan boost when tripped");
        check(2, showWindowId,
              L"Bring up the MechControl window when tripped (safety alert, tray alert and sound remain enabled)");
        button(2, protectionApplyId, L"Apply");
        button(2, resetId, L"Reset");
        button(2, testId, L"Test Failsafe");
        row(2, detailsId, L"<a>Safety details and limitations</a>", WC_LINK, WS_TABSTOP | LWS_TRANSPARENT);
        combo(3, batteryModeId, L"Battery charge mode",
              {L"High Capacity (Performance)", L"Balanced", L"Stationary (Health)"});
        button(3, batteryApplyId, L"Apply battery mode");
        support(3, batterySupportId, L"Battery charge mode");
        label(3, batteryTextId, L"Battery telemetry is read every two seconds only while this page is visible.", 400);
        // Zones 0-2 are the keyboard, light bar and logo; the page shows the logo first, as in the reference.
        for (int zone : {2, 0, 1}) {
            int id = zoneBaseId + zone * 16;
            check(4, id, L"Enabled");
            slider(4, id + 4, L"Brightness", 0, 100, 0);
            rgb(4, id);
            button(4, id + 5, L"Apply");
            button(4, id + 7, L"Pick Color…");
            if (zone != 2) button(4, id + 6, L"Save to EC");
        }
        combo(4, idleId, L"",
              {L"Off", L"10 seconds", L"30 seconds", L"1 minute", L"2 minutes", L"5 minutes", L"10 minutes"});
        check(4, dimmingId, L"Enable Local Dimming");
        button(4, lightingReadId, L"Read from EC");
        combo(5, devicesId, L"", {});
        button(5, scanId, L"Scan");
        button(5, connectId, L"Connect");
        check(5, autoConnectId, L"Auto-connect on startup");
        check(5, coolerFanOnId, L"Fan On");
        slider(5, coolerFanId, L"Fan Duty", 0, 100, 50);
        button(5, coolerFanApplyId, L"Apply");
        check(5, coolerPumpOnId, L"Pump On");
        combo(5, voltageId, L"", {L"7V", L"8V", L"11V", L"12V"});
        slider(5, coolerPumpId, L"Pump Duty", 0, 100, 60);
        button(5, coolerPumpApplyId, L"Apply");
        check(5, coolerRgbOnId, L"LED On");
        rgb(5, coolerRgbOnId);
        combo(5, rgbModeId, L"", {L"Static", L"Breathe", L"Colorful", L"Breathe Color"});
        button(5, coolerRgbApplyId, L"Apply");
        progress_ = control(window_, PROGRESS_CLASSW, L"Device operation progress", PBS_MARQUEE, 0);
        ShowWindow(progress_, SW_HIDE);
        applyDpi();
        selectPage();
        edited_.clear();
        updating_ = false;
        delivery_->attach(window_);
        busy(true);
        worker_ = std::make_unique<ControlWorker>(delivery_, log_, std::move(workerOptions_));
#ifdef MECHCONTROL_WITH_CLI
        cli_ = std::make_unique<CliServer>(*worker_, log_);
#endif
        taskbarMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
        // Explorer is unelevated; allow its restart notification into this elevated window.
        if (taskbarMessage_ && !ChangeWindowMessageFilterEx(window_, taskbarMessage_, MSGFLT_ALLOW, nullptr))
            log_.write("Could not allow TaskbarCreated notification");
        createIcons();
        tray(true);
        powerNotifications_ = {RegisterPowerSettingNotification(window_, &displayState, DEVICE_NOTIFY_WINDOW_HANDLE),
                               RegisterPowerSettingNotification(window_, &powerSource, DEVICE_NOTIFY_WINDOW_HANDLE)};
        if (!powerNotifications_[0] || !powerNotifications_[1])
            log_.write("Display or power-source notifications unavailable");
        log_.write("Native UI ready; DPI=" + std::to_string(dpi_));
    }
    void applyDpi() {
        auto make = [&](int points, int weight, BOOL underline = FALSE) {
            return CreateFontW(-MulDiv(points, dpi_, 72), 0, 0, 0, weight, FALSE, underline, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        };
        auto font = make(9, FW_NORMAL), bold = make(9, FW_BOLD), title = make(10, FW_BOLD), header = make(12, FW_BOLD),
             heading = make(9, FW_BOLD, TRUE);
        for (auto [id, window] : controls_) {
            (void)id;
            SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        for (auto& p : pages_)
            for (auto& r : p.rows)
                if (r.label)
                    SendMessageW(r.label, WM_SETFONT, reinterpret_cast<WPARAM>(framed(index(p.window)) ? bold : font),
                                 TRUE);
        for (int id : {dbOnId, boostId, switchId, curveId, linkId, fanSupportId, protectionId})
            SendMessageW(get(id), WM_SETFONT, reinterpret_cast<WPARAM>(bold), TRUE);
        SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, ref(600));
        if (alert_)
            EnumChildWindows(
                alert_,
                [](HWND child, LPARAM value) -> BOOL {
                    SendMessageW(child, WM_SETFONT, WPARAM(value), TRUE);
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(font));
        for (auto old : {font_, bold_, title_, header_, heading_})
            if (old) DeleteObject(old);
        font_ = font;
        bold_ = bold;
        title_ = title;
        header_ = header;
        heading_ = heading;
        for (int id : {cpuChartId, gpuChartId, debugCpuId, debugGpuId}) SendMessageW(get(id), chartDpiMessage, dpi_, 0);
        layout();
    }
    void layoutPage(Page& p) {
        RECT rc{};
        GetClientRect(p.window, &rc);
        int m = scale(10), width = rc.right - 2 * m;
        if (int page = index(p.window); framed(page)) {
            auto sections = [&](int scroll, bool apply) {
                return page == 0   ? layoutPower(rc.right, scroll, apply)
                       : page == 1 ? layoutFans(rc.right, scroll, apply)
                       : page == 2 ? layoutThermal(rc.right, scroll, apply)
                       : page == 4 ? layoutLighting(rc.right, scroll, apply)
                       : page == 5 ? layoutCooler(rc.right, scroll, apply)
                                   : layoutInfo(scroll, apply);
            };
            int content = sections(0, false);
            p.scroll = std::clamp(p.scroll, 0, std::max(0, content - int(rc.bottom)));
            SCROLLINFO si{
                sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, content - 1), UINT(rc.bottom), p.scroll, 0};
            SetScrollInfo(p.window, SB_VERT, &si, TRUE);
            RECT shown{};
            GetClientRect(p.window, &shown);
            // The scroll bar changed the width.
            if (shown.right != rc.right) {
                layoutPage(p);
                return;
            }
            sections(p.scroll, true);
            RedrawWindow(p.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            return;
        }
        int content = scale(p.height);
        p.scroll = std::clamp(p.scroll, 0, std::max(0, content - int(rc.bottom)));
        SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, content - 1), UINT(rc.bottom), p.scroll,
                      0};
        SetScrollInfo(p.window, SB_VERT, &si, TRUE);
        for (auto& r : p.rows) {
            int y = scale(r.y) + m - p.scroll, h = scale(r.height);
            if (r.slider) {
                int label = std::min(scale(255), width / 2), edit = scale(80);
                place(r.label, m, y, label, h);
                place(r.control, m + label, y, std::max(scale(60), width - label - edit - scale(8)), h);
                place(r.number, rc.right - m - edit, y + scale(5), edit, scale(27));
            } else if (r.label) {
                int label = std::min(scale(255), width / 2);
                place(r.label, m, y, label, h);
                place(r.control, m + label, y, width - label, scale(240));
            } else {
                place(r.control, m, y, width, h);
                // Its note takes the telemetry's place.
                if (r.id == batteryTextId) place(get(batterySupportId), m, y, width, h);
            }
        }
        RedrawWindow(p.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
    Row& find(int id) {
        for (auto& p : pages_)
            for (auto& r : p.rows)
                if (r.id == id) return r;
        throw std::logic_error("Missing control");
    }
    // CPU Power, GPU Power, Apply/Read and GPU Limits (live), as in the reference PowerTab. Returns the content height.
    int layoutPower(int width, int scroll, bool apply) {
        auto window = pages_[0].window;
        auto dc = GetDC(window);
        auto previous = SelectObject(dc, title_);
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        int titleHeight = metrics.tmHeight;
        SelectObject(dc, font_);
        GetTextMetricsW(dc, &metrics);
        int line = metrics.tmHeight;
        auto extent = [&](HFONT font, const std::wstring& s) {
            SelectObject(dc, font);
            SIZE size{};
            GetTextExtentPoint32W(dc, s.c_str(), int(s.size()), &size);
            return int(size.cx);
        };
        // pad: the reference's labels inset their text by about a third of the font's em on each side.
        int x = ref(16), right = width - ref(16), left = x + ref(15), end = right - ref(15), y = ref(6) - scroll,
            slider = std::max(ref(30), ref(8) + line), box = line + ref(4), pad = ref(6);
        int noteLeft = left + ref(3) + pad,
            wrap = std::max(ref(60), std::min(ref(600) - 2 * pad, end - noteLeft - pad));
        bool cpu = !(state_.contains("cpu") && state_["cpu"].is_null()),
             gpu = !(state_.contains("gpu") && state_["gpu"].is_null());
        std::vector<std::pair<RECT, const wchar_t*>> frames;
        std::vector<Text> texts;
        auto put = [&](HWND child, bool shown, int cx, int cy, int w, int h) {
            if (!apply) return;
            ShowWindow(child, shown ? SW_SHOW : SW_HIDE);
            if (shown) place(child, cx, cy, w, h);
        };
        auto open = [&](const wchar_t* title) {
            frames.push_back({{x, y, right, 0}, title});
            y += titleHeight + ref(11);
        };
        auto close = [&] {
            y += ref(13);
            frames.back().first.bottom = y;
            y += ref(12);
        };
        auto note = [&](const std::wstring& s, COLORREF color, int top, int bottom) {
            if (s.empty()) return;
            SelectObject(dc, font_);
            RECT r{noteLeft, y + top, noteLeft + wrap, y + top};
            DrawTextW(dc, s.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            r.right = noteLeft + wrap;
            texts.push_back({r, s, color});
            y = r.bottom + bottom;
        };
        // Each section sizes its label and value columns to its own contents.
        auto columns = [&](std::initializer_list<int> ids, const wchar_t* widest) {
            int label = 0;
            for (int id : ids) label = std::max(label, extent(bold_, text(find(id).label)));
            return std::pair(label + 2 * pad, extent(font_, widest) + 2 * pad);
        };
        auto slide = [&](int id, bool shown, std::pair<int, int> c) {
            auto& r = find(id);
            int track = left + c.first + ref(8), value = end - c.second;
            put(r.label, shown, left + pad, y + ref(8), c.first - pad, line);
            put(r.control, shown, track, y, value - ref(8) - track, slider);
            put(r.number, shown, value + pad, y + ref(8), c.second - pad, line);
            if (shown) y += slider;
        };
        auto check = [&](int id, bool shown, HFONT font) {
            if (shown) y += ref(6);
            put(get(id), shown, left, y, extent(font, text(get(id))) + ref(40), box);
            if (shown) y += box;
        };
        open(L"CPU Power");
        auto c = columns({pl1Id, pl2Id, pl4Id, tempId}, L"000 °C");
        for (int id : {pl1Id, pl2Id, pl4Id, tempId}) slide(id, cpu, c);
        y = supportNote(cpuSupportId, true, errorColor, noteLeft, y, 0, wrap, apply);
        note(cpuNote_, errorColor, 0, 0);
        note(cpuPreset_, GetSysColor(COLOR_GRAYTEXT), ref(4), 0);
        close();
        open(L"GPU Power");
        c = columns({tgpId, dbId}, L"000 W");
        slide(tgpId, gpu, c);
        check(dbOnId, gpu, bold_);
        slide(dbId, gpu, c);
        y = supportNote(gpuSupportId, true, errorColor, noteLeft, y, 0, wrap, apply);
        if (gpu) note(powerHint_, GetSysColor(COLOR_GRAYTEXT), 0, 0);
        close();
        y += ref(6);
        put(get(powerApplyId), true, x, y, ref(110), ref(32));
        put(get(powerReadId), true, x + ref(120), y, ref(140), ref(32));
        y += ref(44);
        // Collapsed by default. Controls withheld for an unverified model or firmware stay visible, greyed, with the
        // reason; the cooler flag appears only when the EC reports cooler support.
        auto advanced = state_.value("advanced", Json::object());
        bool flag = advanced.value("coolerFlag", Json::object()).value("available", false), expanded = advanced_;
        put(get(advancedId), true, x + ref(3), y, extent(font_, L"Hide advanced controls") + ref(8), line);
        y += line + ref(14);
        auto muted = GetSysColor(COLOR_GRAYTEXT);
        if (expanded) open(L"Advanced");
        check(unlockId, expanded && gpu, font_);
        y = supportNote(unlockSupportId, expanded && gpu, muted, noteLeft, y, ref(2), wrap, apply);
        check(dlppcId, expanded && gpu, font_);
        // Current limits apply with the Apply button above, like the other GPU settings.
        if (expanded && gpu) y += ref(6);
        c = columns({nvId, msId, inputId}, L"000.000 A");
        for (int id : {nvId, msId, inputId}) slide(id, expanded && gpu, c);
        y = supportNote(boardSupportId, expanded && gpu, muted, noteLeft, y, ref(2), wrap, apply);
        check(bypassId, expanded, font_);
        if (expanded) {
            note(
                L"Keeps your PL1/PL2/PL4 under heavy CPU+GPU load instead of the EC lowering them. Fans run at full speed.",
                muted, ref(2), 0);
            note(bypassText_, bypassColor_, ref(2), 0);
        }
        y = supportNote(bypassSupportId, expanded, muted, noteLeft, y, ref(2), wrap, apply);
        check(coolerFlagId, expanded && flag, font_);
        if (expanded && flag)
            note(
                L"Switches the EC to its liquid-cooler fan and GPU tables and changes some hardware signals whose purpose is unknown. Does not lift custom or adapter limits.",
                muted, ref(2), 0);
        if (expanded) close();
        int liveTop = -1;
        if (gpu) {
            open(L"GPU Limits (live)");
            liveTop = y;
            y += 8 * (ref(6) + line);
            auto gray = GetSysColor(COLOR_GRAYTEXT);
            if (!liveReason_.empty()) note(L"-- (" + liveReason_ + L")", gray, ref(6), 0);
            // Measured on an RTX 5090 Laptop: current = rail power / a fixed voltage, so it reads high.
            note(
                L"NVVDD and MSVDD currents are calculated by the driver from rail power, not measured, and can read well above the real current. The limits use the same calculated values.",
                gray, 0, 0);
            note(
                L"When software shunt modding, tune the NVVDD, MSVDD and board input current limits so that they are all solid green 99% of the time, and not any higher.",
                gray, ref(6), 0);
            close();
        }
        SelectObject(dc, previous);
        ReleaseDC(window, dc);
        if (apply) {
            pages_[0].frames = std::move(frames);
            pages_[0].texts = std::move(texts);
            liveTable_ = {left, liveTop, width, liveTop + 8 * (ref(6) + line)};
            if (liveTop < 0)
                for (UINT_PTR i = 1; i <= std::size(liveTips); ++i) {
                    TOOLINFOW tool{sizeof tool};
                    tool.hwnd = window;
                    tool.uId = i;
                    SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
                }
        }
        return y + scroll + ref(6);
    }
    // Fan Boost, Fan Switch Speed, Custom Fan Curve and, while Link Fans runs, the curves it writes, as in the
    // reference FanTab. Returns the content height.
    int layoutFans(int width, int scroll, bool apply) {
        auto window = pages_[1].window;
        auto dc = GetDC(window);
        auto previous = SelectObject(dc, title_);
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        int titleHeight = metrics.tmHeight;
        SelectObject(dc, font_);
        GetTextMetricsW(dc, &metrics);
        int line = metrics.tmHeight;
        auto extent = [&](HFONT font, const std::wstring& s) {
            SelectObject(dc, font);
            SIZE size{};
            GetTextExtentPoint32W(dc, s.c_str(), int(s.size()), &size);
            return int(size.cx);
        };
        int x = ref(16), right = width - ref(16), left = x + ref(15), end = right - ref(15), y = ref(6) - scroll,
            slider = std::max(ref(30), ref(8) + line), box = ref(28), button = ref(32), chart = ref(320), pad = ref(6);
        bool fans = state_.value("enabled", std::set<std::string>{}).contains("fanControl"),
             supported = state_.value("curvesSupported", false), hasGpu = state_.value("hasGpuFan", false);
        bool linkable = state_.value("linkAvailable", false),
             linked = supported && linkable && state_.value("linked", false);
        std::vector<std::pair<RECT, const wchar_t*>> frames;
        std::vector<Text> texts;
        RECT bar{};
        auto put = [&](HWND child, bool shown, int cx, int cy, int w, int h) {
            if (!apply) return;
            ShowWindow(child, shown ? SW_SHOW : SW_HIDE);
            if (shown) place(child, cx, cy, w, h);
        };
        auto open = [&](const wchar_t* title) {
            frames.push_back({{x, y, right, 0}, title});
            y += titleHeight + ref(11);
        };
        auto close = [&] {
            y += ref(13);
            frames.back().first.bottom = y;
            y += ref(12);
        };
        // The reference sizes check boxes to their bold text.
        auto check = [&](int id) { return extent(bold_, text(get(id))) + ref(38); };
        // The CPU and GPU curves share the width, with a small gap between them.
        auto charts = [&](int cpu, int gpu, int from, int to, bool shown) {
            int half = (to - from) / 2;
            put(get(cpu), shown, from, y, (hasGpu ? half : to - from) - ref(4), chart);
            put(get(gpu), shown && hasGpu, from + half + ref(4), y, to - from - half - ref(4), chart);
        };
        open(L"Fan Boost");
        int boost = check(boostId);
        put(get(boostId), true, left, y + ref(6), boost, box);
        put(get(boostApplyId), true, left + boost + ref(16), y, ref(110), button);
        y += ref(34);
        close();
        frames.back().first.right = left + boost + ref(141); // This section fits its contents, not the page.
        open(L"Fan Switch Speed");
        put(get(switchId), true, left, y + ref(4), check(switchId), box);
        y += ref(35);
        auto& speed = find(switchMsId);
        int from = left + ref(3), to = end - ref(3), label = extent(bold_, text(speed.label)) + 2 * pad,
            value = extent(font_, L"12700 ms") + 2 * pad, track = from + label + ref(8);
        put(speed.label, true, from + pad, y + ref(8), label - pad, line);
        put(speed.control, true, track, y, to - value - ref(8) - track, slider);
        put(speed.number, true, to - value + pad, y + ref(8), value - pad, line);
        y += slider + ref(9);
        put(get(switchApplyId), true, left, y, ref(110), button);
        y += button;
        close();
        open(L"Custom Fan Curve");
        int curve = check(curveId), link = check(linkId);
        put(get(curveId), true, left, y + ref(4), curve, box);
        put(get(linkId), linkable, left + curve + ref(24), y + ref(4), link, box);
        if (linked) bar = {left + curve + link + ref(48), y + ref(2), end, y + ref(31)};
        y += ref(39);
        charts(cpuChartId, gpuChartId, left, end, supported);
        explain(fanSupportId, fans ? L"" : disabledReason(L"Fan controls", "fanControl"));
        if (supported)
            y += chart + ref(6);
        else if (fans) {
            auto warning = L"Fan curve editing is not available (format: " +
                           w(state_.value("fanFormat", std::string{})) +
                           L"). Only fan boost and curve enable are supported.";
            RECT r{left + pad, y + ref(6), end - pad, y + ref(6)};
            SelectObject(dc, bold_);
            DrawTextW(dc, warning.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            r.right = end - pad;
            texts.push_back({r, warning, warnColor, bold_});
            y += ref(94);
        } else
            y = std::max(y + ref(94), supportNote(fanSupportId, true, errorColor, left + pad, y, ref(6),
                                                  end - left - 2 * pad, apply) +
                                          ref(6));
        y += ref(3);
        put(get(fanApplyId), true, left + ref(3), y + ref(6), ref(110), button);
        put(get(fanReadId), true, left + ref(123), y + ref(6), ref(140), button);
        int footer = ref(6) + button;
        if (supported) {
            RECT r{left + ref(273) + pad, y + ref(14), end, y + ref(14)};
            SelectObject(dc, font_);
            DrawTextW(dc, fanValidation_.c_str(), -1, &r, DT_CALCRECT | DT_NOPREFIX);
            texts.push_back({r, fanValidation_, fanValidation_ == L"Valid" ? okColor : errorColor});
            footer = std::max(footer, int(r.bottom) - y + ref(1));
        }
        y += footer + ref(3);
        close();
        if (linked) {
            open(L"Fan Curves Written to EC (Live)");
            y += ref(3);
            charts(debugCpuId, debugGpuId, left - ref(3), end + ref(3), true);
            y += chart + ref(3);
            close();
        } else
            charts(debugCpuId, debugGpuId, 0, 0, false);
        SelectObject(dc, previous);
        ReleaseDC(window, dc);
        if (apply) {
            pages_[1].frames = std::move(frames);
            pages_[1].texts = std::move(texts);
            linkedBar_ = bar;
        }
        return y + scroll + ref(6);
    }
    // GPU Thermal Failsafe and its Status, as in the reference FailsafeTab. Returns the content height.
    int layoutThermal(int width, int scroll, bool apply) {
        auto window = pages_[2].window;
        auto dc = GetDC(window);
        auto previous = SelectObject(dc, title_);
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        int titleHeight = metrics.tmHeight;
        SelectObject(dc, font_);
        GetTextMetricsW(dc, &metrics);
        int line = metrics.tmHeight;
        auto extent = [&](HFONT font, const std::wstring& s) {
            SelectObject(dc, font);
            SIZE size{};
            GetTextExtentPoint32W(dc, s.c_str(), int(s.size()), &size);
            return int(size.cx);
        };
        int x = ref(16), right = width - ref(16), left = x + ref(15), end = right - ref(15), y = ref(6) - scroll,
            slider = std::max(ref(30), ref(8) + line), box = line + ref(4), button = ref(32), pad = ref(6);
        COLORREF gray = GetSysColor(COLOR_GRAYTEXT), black = GetSysColor(COLOR_BTNTEXT);
        std::vector<std::pair<RECT, const wchar_t*>> frames;
        std::vector<Text> texts;
        RECT live{};
        auto put = [&](HWND child, int cx, int cy, int w, int h) {
            if (apply) place(child, cx, cy, w, h);
        };
        auto open = [&](const wchar_t* title) {
            frames.push_back({{x, y, right, 0}, title});
            y += titleHeight + ref(11);
        };
        auto close = [&] {
            y += ref(13);
            frames.back().first.bottom = y;
            y += ref(12);
        };
        // Text wrapped from 'from' to the section's edge. Returns its bottom.
        auto paragraph = [&](const std::wstring& s, int from, int top, COLORREF color, bool bold) {
            RECT r{from, top, end - pad, top};
            SelectObject(dc, bold ? bold_ : font_);
            DrawTextW(dc, s.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            r.right = end - pad;
            texts.push_back({r, s, color, bold ? bold_ : font_});
            return int(r.bottom);
        };
        auto note = [&](const std::wstring& s) { y = paragraph(s, left + pad, y + ref(3), gray, false) + ref(3); };
        auto check = [&](int id, HFONT font) {
            y += ref(6);
            put(get(id), left, y, std::min(extent(font, text(get(id))) + ref(40), end - left), box);
            y += box;
        };
        open(L"GPU Thermal Failsafe");
        note(thermalDescription);
        bool unavailable = state_.contains("thermalActions") && !(state_["thermalActions"].get<int>() & Gpu);
        explain(thermalSupportId,
                unavailable ? L"Thermal failsafe unavailable: " + w(state_.value("thermalReason", std::string{})) + L"."
                            : L"",
                state_.value("enabled", std::set<std::string>{}).contains("gpuPower"));
        y = supportNote(thermalSupportId, true, errorColor, left + pad, y, ref(3), end - left - 2 * pad, apply);
        check(protectionId, bold_);
        auto& trigger = find(thresholdId);
        int label = extent(bold_, text(trigger.label)) + 2 * pad, value = extent(font_, L"00 °C") + 2 * pad,
            track = left + label + ref(8);
        y += ref(6);
        put(trigger.label, left + pad, y + ref(8), label - pad, line);
        put(trigger.control, track, y, end - value - ref(8) - track, slider);
        put(trigger.number, end - value + pad, y + ref(8), value - pad, line);
        y += slider;
        for (int id : {reduceId, fanBoostId, showWindowId}) check(id, font_);
        y += ref(3);
        note(thermalText_[0]);
        y += ref(6);
        put(get(protectionApplyId), left, y, ref(110), button);
        paragraph(thermalText_[1], left + ref(120) + pad, y + (button - line) / 2,
                  thermalText_[1] == L"Settings match saved profile" ? gray : warnColor, false);
        y += button + ref(3);
        note(
            L"Applies to the current profile. Apply takes effect now; use the profile bar's Save button to keep these settings.");
        put(get(detailsId), left + pad, y + ref(3), extent(font_, L"Safety details and limitations") + ref(4), line);
        y += line + ref(6);
        if (thermalDetails_) note(thermalDetails);
        close();
        open(L"Status");
        const wchar_t* captions[]{L"State:", L"GPU temperature (EC):", L"Peak since armed:", L"Last trip:",
                                  L"Configuration:"};
        int column = 0;
        for (auto c : captions) column = std::max(column, extent(bold_, c));
        column += left + pad + ref(16);
        auto row = [&](int i, const std::wstring& s, COLORREF color, bool bold) {
            int top = y + ref(3);
            texts.push_back({{left + pad, top, column, top + line}, captions[i], black, bold_});
            y = (s.empty() ? top + line : std::max(top + line, paragraph(s, column, top, color, bold))) + ref(3);
        };
        bool enabled = state_.value("protection", Json::object()).value("enabled", false);
        row(0, thermalText_[2],
            tripped_                                               ? errorColor
            : !enabled                                             ? gray
            : state_.value("temperature", Json(nullptr)).is_null() ? warnColor
                                                                   : okColor,
            tripped_);
        live = {column, y, end - pad, 0};
        row(1, L"", black, false);
        row(2, L"", black, false);
        live.bottom = y; // Readings are painted as they arrive.
        row(3, thermalText_[3], black, false);
        if (!thermalText_[4].empty()) row(4, thermalText_[4], warnColor, false);
        y += ref(6);
        put(get(resetId), left, y, ref(110), button);
        put(get(testId), left + ref(120), y, ref(140), button);
        y += button;
        close();
        SelectObject(dc, previous);
        ReleaseDC(window, dc);
        if (apply) {
            pages_[2].frames = std::move(frames);
            pages_[2].texts = std::move(texts);
            thermalLive_ = live;
        }
        return y + scroll + ref(6);
    }
    // The logo, keyboard and light bar zones, then Idle Timer and Local Dimming, as in the reference LightingTab.
    // The last two sit side by side instead of stacked. Returns the content height.
    int layoutLighting(int width, int scroll, bool apply) {
        auto window = pages_[4].window;
        auto dc = GetDC(window);
        auto previous = SelectObject(dc, title_);
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        int titleHeight = metrics.tmHeight;
        SelectObject(dc, font_);
        GetTextMetricsW(dc, &metrics);
        int line = metrics.tmHeight;
        auto extent = [&](HFONT font, const std::wstring& s) {
            SelectObject(dc, font);
            SIZE size{};
            GetTextExtentPoint32W(dc, s.c_str(), int(s.size()), &size);
            return int(size.cx);
        };
        int x = ref(16), right = width - ref(16), left = x + ref(15), end = right - ref(15), y = ref(6) - scroll,
            slider = std::max(ref(30), ref(8) + line), box = line + ref(4), button = ref(32), pad = ref(6),
            row = ref(36);
        std::vector<std::pair<RECT, const wchar_t*>> frames;
        std::vector<Text> texts;
        std::array<RECT, 3> swatches{};
        auto put = [&](HWND child, int cx, int cy, int w, int h) {
            if (apply) place(child, cx, cy, w, h);
        };
        auto open = [&](const wchar_t* title, int from, int to) {
            frames.push_back({{from, y, to, 0}, title});
            y += titleHeight + ref(11);
        };
        auto close = [&] {
            y += ref(13);
            frames.back().first.bottom = y;
            y += ref(12);
        };
        // Painted text; returns its right edge.
        auto say = [&](const wchar_t* s, int cx, int cy) {
            int to = cx + extent(font_, s);
            texts.push_back({{cx, cy, to, cy + line}, s, GetSysColor(COLOR_BTNTEXT)});
            return to;
        };
        int label = extent(bold_, L"Brightness") + 2 * pad, value = extent(font_, L"100 %") + 2 * pad,
            track = left + label + ref(8);
        const wchar_t* titles[]{L"Keyboard Backlight", L"Light Bar (Front)", L"Logo LED (Rear Cover)"};
        for (int zone : {2, 0, 1}) {
            int base = zoneBaseId + zone * 16;
            auto& brightness = find(base + 4);
            open(titles[zone], x, right);
            y += ref(4);
            put(get(base), left, y, extent(font_, L"Enabled") + ref(40), box);
            y += box + ref(6);
            put(brightness.label, left + pad, y + ref(8), label - pad, line);
            put(brightness.control, track, y, end - value - ref(8) - track, slider);
            put(brightness.number, end - value + pad, y + ref(8), value - pad, line);
            y += slider + ref(6);
            int cx = colorRow(dc, texts, base, left, y, row, apply);
            swatches[zone] = {cx + ref(12), y + ref(2), cx + ref(44), y + ref(34)};
            y += row + ref(6);
            put(get(base + 5), left, y, ref(110), button);
            put(get(base + 7), left + ref(120), y, ref(110), button);
            if (zone != 2) put(get(base + 6), left + ref(240), y, ref(120), button);
            y += button;
            close();
        }
        // Both sections fit their content or title, whichever is wider.
        auto fit = [&](const wchar_t* title, int from, int content) {
            return std::max(content + ref(15), from + ref(25) + extent(title_, title) + ref(15));
        };
        const wchar_t *idleTitle = L"Idle Timer (Keyboard + Light Bar)", *dimmingTitle = L"Local Dimming (Mini-LED)";
        int top = y, combo = left + pad + extent(font_, L"Turn off after:") + ref(16),
            idle = fit(idleTitle, x, combo + ref(140));
        int dimmingX = idle + ref(12), dimmingLeft = dimmingX + ref(15),
            check = extent(font_, text(get(dimmingId))) + ref(40);
        open(idleTitle, x, idle);
        say(L"Turn off after:", left + pad, y + ref(6));
        put(get(idleId), combo, y + ref(2), ref(140), ref(300));
        put(get(dimmingId), dimmingLeft, y + (row - box) / 2, check, box);
        y += row;
        close();
        frames.push_back({{dimmingX, top, fit(dimmingTitle, dimmingX, dimmingLeft + check), frames.back().first.bottom},
                          dimmingTitle});
        put(get(lightingReadId), x, y, ref(140), button);
        y += button;
        SelectObject(dc, previous);
        ReleaseDC(window, dc);
        if (apply) {
            pages_[4].frames = std::move(frames);
            pages_[4].texts = std::move(texts);
            swatches_ = swatches;
        }
        return y + scroll + ref(6);
    }
    // A color's R, G and B labels and boxes on a row, from cx. Returns the right edge.
    int colorRow(HDC dc, std::vector<Text>& texts, int base, int cx, int y, int row, bool apply) {
        TEXTMETRICW metrics{};
        SelectObject(dc, font_);
        GetTextMetricsW(dc, &metrics);
        int line = metrics.tmHeight, edit = line + ref(3);
        for (int n = 1; n <= 3; ++n) {
            const wchar_t* name = n == 1 ? L"R:" : n == 2 ? L"G:" : L"B:";
            SIZE size{};
            GetTextExtentPoint32W(dc, name, 2, &size);
            cx += n == 1 ? ref(6) : ref(14);
            texts.push_back({{cx, y + (row - line) / 2, cx + size.cx, y + (row - line) / 2 + line},
                             name,
                             GetSysColor(COLOR_BTNTEXT)});
            cx += size.cx + ref(8);
            if (apply) {
                place(get(base + n + 1000), cx, y + (row - edit) / 2, ref(68), edit);
                SendMessageW(get(base + n), UDM_SETBUDDY, reinterpret_cast<WPARAM>(get(base + n + 1000)), 0);
            }
            cx += ref(68);
        }
        return cx;
    }
    // The connection, Radiator Fan, Pump and LED sections, as in the reference LiquidCoolerTab. The LED section fits
    // its contents. Returns the content height.
    int layoutCooler(int width, int scroll, bool apply) {
        auto window = pages_[5].window;
        auto dc = GetDC(window);
        auto previous = SelectObject(dc, title_);
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        int titleHeight = metrics.tmHeight;
        SelectObject(dc, font_);
        GetTextMetricsW(dc, &metrics);
        int line = metrics.tmHeight;
        auto extent = [&](HFONT font, const std::wstring& s) {
            SelectObject(dc, font);
            SIZE size{};
            GetTextExtentPoint32W(dc, s.c_str(), int(s.size()), &size);
            return int(size.cx);
        };
        int x = ref(16), right = width - ref(16), left = x + ref(15), end = right - ref(15), y = ref(6) - scroll,
            slider = std::max(ref(30), ref(8) + line), box = line + ref(4), button = ref(32), pad = ref(6),
            row = ref(36);
        RECT r{};
        GetWindowRect(get(devicesId), &r);
        int field = r.bottom - r.top; // A closed drop-down list is as tall as its font needs.
        COLORREF black = GetSysColor(COLOR_BTNTEXT), gray = GetSysColor(COLOR_GRAYTEXT);
        std::vector<std::pair<RECT, const wchar_t*>> frames;
        std::vector<Text> texts;
        RECT swatch{};
        auto put = [&](HWND child, int cx, int cy, int w, int h) {
            if (apply) place(child, cx, cy, w, h);
        };
        auto open = [&](const wchar_t* title) {
            frames.push_back({{x, y, right, 0}, title});
            y += titleHeight + ref(11);
        };
        auto close = [&] {
            y += ref(13);
            frames.back().first.bottom = y;
            y += ref(12);
        };
        // Painted text; returns its right edge.
        auto say = [&](const std::wstring& s, int cx, int cy, COLORREF color) {
            int to = cx + extent(font_, s);
            texts.push_back({{cx, cy, to, cy + line}, s, color});
            return to;
        };
        // A check box sized to its text; returns its right edge.
        auto check = [&](int id, int cy) {
            int to = left + extent(font_, text(get(id))) + ref(40);
            put(get(id), left, cy, to - left, box);
            return to;
        };
        auto duty = [&](int id) {
            auto& d = find(id);
            int label = extent(bold_, text(d.label)) + 2 * pad, value = extent(font_, L"100 %") + 2 * pad,
                track = left + label + ref(8);
            put(d.label, left + pad, y + ref(8), label - pad, line);
            put(d.control, track, y, end - value - ref(8) - track, slider);
            put(d.number, end - value + pad, y + ref(8), value - pad, line);
            y += slider + ref(6);
        };
        // The latest connection status: green while connected, amber while connecting or scanning, red for a problem.
        auto has = [&](const wchar_t* s) { return coolerStatus_.find(s) != std::wstring::npos; };
        COLORREF tone = has(L"failed") || has(L"lost") || has(L"unavailable") ? errorColor
                        : coolerConnected_                                    ? okColor
                        : has(L"Connecting") || has(L"Scanning")              ? warnColor
                                                                              : gray;
        open(L"Liquid Cooler (BLE)");
        RECT status{left + pad, y + ref(4), end - pad, y + ref(4)};
        SelectObject(dc, bold_);
        DrawTextW(dc, coolerStatus_.c_str(), -1, &status, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        status.right = end - pad;
        texts.push_back({status, coolerStatus_, tone, bold_});
        y = status.bottom + ref(6);
        int connect = end - ref(100), scan = connect - ref(92), height = std::max(field, button);
        put(get(devicesId), left, y + (height - field) / 2, scan - ref(12) - left, ref(300));
        put(get(scanId), scan, y + (height - button) / 2, ref(80), button);
        put(get(connectId), connect, y + (height - button) / 2, ref(100), button);
        y += height + ref(6);
        int to = check(autoConnectId, y);
        if (!coolerFirmware_.empty()) say(coolerFirmware_, to + ref(16), y + (box - line) / 2, gray);
        y += box;
        close();
        open(L"Radiator Fan");
        y += ref(4);
        check(coolerFanOnId, y);
        y += box + ref(6);
        duty(coolerFanId);
        put(get(coolerFanApplyId), left, y, ref(110), button);
        y += button;
        close();
        open(L"Pump");
        y += ref(2);
        height = std::max(field, box);
        to = say(L"Voltage:", check(coolerPumpOnId, y + (height - box) / 2) + ref(24), y + (height - line) / 2, black);
        put(get(voltageId), to + ref(16), y + (height - field) / 2, ref(80), ref(300));
        y += height + ref(6);
        duty(coolerPumpId);
        put(get(coolerPumpApplyId), left, y, ref(110), button);
        y += button;
        close();
        open(L"LED");
        y += ref(4);
        check(coolerRgbOnId, y);
        y += box + ref(6);
        int cx = colorRow(dc, texts, coolerRgbOnId, left, y, row, apply);
        swatch = {cx + ref(12), y + ref(2), cx + ref(44), y + ref(34)};
        cx = say(L"Mode:", swatch.right + ref(19), y + (row - line) / 2, black) + ref(12);
        put(get(rgbModeId), cx, y + (row - field) / 2, ref(120), ref(300));
        y += row + ref(6);
        put(get(coolerRgbApplyId), left, y, ref(110), button);
        y += button;
        close();
        frames.back().first.right = cx + ref(135);
        SelectObject(dc, previous);
        ReleaseDC(window, dc);
        if (apply) {
            pages_[5].frames = std::move(frames);
            pages_[5].texts = std::move(texts);
            coolerSwatch_ = swatch;
        }
        return y + scroll + ref(6);
    }
    // System Identity, Temperatures and Power Delivery, as in the reference SystemInfoTab. Each section fits its
    // contents; the readings are painted into their cells. Returns the content height.
    int layoutInfo(int scroll, bool apply) {
        auto window = pages_[6].window;
        auto dc = GetDC(window);
        auto previous = SelectObject(dc, title_);
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        int titleHeight = metrics.tmHeight;
        SelectObject(dc, header_);
        GetTextMetricsW(dc, &metrics);
        int headerHeight = metrics.tmHeight;
        SelectObject(dc, font_);
        GetTextMetricsW(dc, &metrics);
        int line = metrics.tmHeight;
        auto extent = [&](HFONT font, const std::wstring& s) {
            SelectObject(dc, font);
            SIZE size{};
            GetTextExtentPoint32W(dc, s.c_str(), int(s.size()), &size);
            return int(size.cx);
        };
        int x = ref(16), left = x + ref(21), y = ref(6) - scroll, right = 0;
        std::vector<std::pair<RECT, const wchar_t*>> frames;
        std::vector<Text> texts;
        std::array<RECT, 8> cells{};
        // Painted text; returns its right edge.
        auto say = [&](const std::wstring& s, HFONT font, int cy, int height) {
            int to = left + extent(font, s);
            texts.push_back({{left, cy, to, cy + height}, s, GetSysColor(COLOR_BTNTEXT), font});
            return to;
        };
        auto open = [&](const wchar_t* title) {
            frames.push_back({{x, y, 0, 0}, title});
            y += titleHeight + ref(11);
            right = 0;
        };
        auto close = [&] {
            auto& [r, title] = frames.back();
            r.right = std::max(right + ref(21), x + ref(25) + extent(title_, title) + ref(15));
            y += ref(13);
            r.bottom = y;
            y += ref(12);
        };
        // Bold captions and their readings' cells. Index -1 is a source heading.
        auto table = [&](const wchar_t* title, const std::vector<std::pair<int, std::wstring>>& rows) {
            open(title);
            int column = 0;
            for (auto& [i, c] : rows)
                if (i >= 0) column = std::max(column, extent(bold_, c));
            column += left + ref(16);
            for (auto& [i, c] : rows) {
                if (i < 0) {
                    if (&c != &rows.front().second) y += ref(8);
                    right = std::max(right, say(c, heading_, y, line));
                    y += line;
                    continue;
                }
                say(c, bold_, y + ref(3), line);
                cells[i] = {column, y + ref(3), column + infoWidth(dc, i), y + ref(3) + line};
                right = std::max(right, int(cells[i].right));
                y += line + ref(6);
            }
            close();
        };
        auto identity = identity_.is_object() ? identity_ : Json::object();
        auto board = w(identity.value("baseboard", std::string{})), cpu = w(identity.value("cpu", std::string{}));
        if (!board.empty() || !cpu.empty()) {
            open(L"System Identity");
            if (!board.empty()) {
                right = say(board, header_, y, headerHeight);
                y += headerHeight + ref(4);
            }
            if (!cpu.empty()) {
                right = std::max(right, say(cpu, font_, y, line));
                y += line;
            }
            close();
        }
        // Temperature registers come from the model profile; the other readings use fixed registers.
        auto reg = [&](const char* key) {
            wchar_t s[16]{};
            if (state_.contains(key)) swprintf_s(s, L" [0x%03X]", state_[key].get<int>());
            return std::wstring(s);
        };
        table(L"Temperatures", {{0, L"CPU Temperature" + reg("cpuTempRegister") + L":"},
                                {1, L"GPU Temperature" + reg("gpuTempRegister") + L":"}});
        std::vector<std::pair<int, std::wstring>> power{{2, L"Adapter Current [0x449]:"},
                                                        {3, L"Adapter Current, raw ADC [0x60C-0x60D]:"},
                                                        {4, L"TGP Live / NPCF ACBT [0x788]:"},
                                                        {5, L"DB Max Live / NPCF AMAT [0x7D5]:"}};
        // Source headings only when the NVIDIA driver is a second source.
        if (state_.value("enabled", std::set<std::string>{}).contains("gpuPower")) {
            power.emplace(power.begin(), -1, L"EC");
            power.insert(power.end(), {{-1, L"NVIDIA Driver"}, {6, L"GPU Budget F7:"}, {7, L"CPU+GPU Budget TPP:"}});
        }
        table(L"Power Delivery", power);
        SelectObject(dc, previous);
        ReleaseDC(window, dc);
        if (apply) {
            pages_[6].frames = std::move(frames);
            pages_[6].texts = std::move(texts);
            infoCells_ = cells;
        }
        return y + scroll + ref(6);
    }
    void paintPage(int index, HDC target, const RECT& area) {
        auto& p = pages_[index];
        int width = area.right - area.left, height = area.bottom - area.top;
        if (width <= 0 || height <= 0) return;
        // Readings repaint small areas often, so the buffer covers only the area being painted.
        auto dc = CreateCompatibleDC(target);
        auto bitmap = CreateCompatibleBitmap(target, width, height);
        auto oldBitmap = SelectObject(dc, bitmap);
        SetViewportOrgEx(dc, -area.left, -area.top, nullptr);
        FillRect(dc, &area, GetSysColorBrush(COLOR_BTNFACE));
        SetBkMode(dc, TRANSPARENT);
        auto pen = CreatePen(PS_SOLID, 1, frameColor);
        auto oldPen = SelectObject(dc, pen);
        auto oldFont = SelectObject(dc, title_);
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        for (auto& [r, title] : p.frames) {
            // A thin frame whose top edge stops either side of the bold title.
            int n = int(wcslen(title));
            SIZE size{};
            GetTextExtentPoint32W(dc, title, n, &size);
            int titleX = r.left + ref(10), middle = r.top + ref(2) + size.cy / 2;
            // The reference measured the title with GDI's default padding, hence the wider gap after it.
            POINT edge[]{{titleX - ref(2), middle},   {r.left, middle},      {r.left, r.bottom - 1},
                         {r.right - 1, r.bottom - 1}, {r.right - 1, middle}, {titleX + size.cx + ref(15), middle}};
            Polyline(dc, edge, 6);
            TextOutW(dc, titleX, r.top + ref(2), title, n);
        }
        for (auto& t : p.texts) {
            SelectObject(dc, t.font ? t.font : font_);
            SetTextColor(dc, t.color);
            RECT r = t.rect;
            DrawTextW(dc, t.text.c_str(), -1, &r, DT_WORDBREAK | DT_NOPREFIX);
        }
        if (index == 0 && liveTable_.top >= 0) paintLive(dc);
        if (index == 1 && linkedBar_.right > linkedBar_.left)
            paintLinkedStatus(dc, linkedBar_, state_.value("linkedStatus", Json::object()), dpi_);
        // The failsafe's GPU temperature and peak repaint on their own as readings arrive.
        if (index == 2) {
            SelectObject(dc, font_);
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            int pitch = (thermalLive_.bottom - thermalLive_.top) / 2;
            for (int i : {0, 1})
                TextOutW(dc, thermalLive_.left, thermalLive_.top + ref(3) + i * pitch, thermalText_[5 + i].c_str(),
                         int(thermalText_[5 + i].size()));
        }
        // Each swatch shows the color in its R, G and B boxes.
        auto swatch = [&](const RECT& r, int base) {
            auto brush = CreateSolidBrush(color(base, false));
            FillRect(dc, &r, brush);
            DeleteObject(brush);
            FrameRect(dc, &r, GetSysColorBrush(COLOR_WINDOWFRAME));
        };
        if (index == 4)
            for (int zone = 0; zone < 3; ++zone) swatch(swatches_[zone], zoneBaseId + zone * 16);
        if (index == 5) swatch(coolerSwatch_, coolerRgbOnId);
        if (index == 6) {
            SelectObject(dc, font_);
            SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
            for (int i = 0; i < 8; ++i)
                if (infoCells_[i].right > infoCells_[i].left)
                    TextOutW(dc, infoCells_[i].left, infoCells_[i].top, infoText_[i].c_str(), int(infoText_[i].size()));
        }
        BitBlt(target, area.left, area.top, width, height, dc, area.left, area.top, SRCCOPY);
        SelectObject(dc, oldFont);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
    // Each limit next to the clock: its setting, reading, share of the setting and the clock cap of the driver
    // controller enforcing it. No verdicts: several limits hold the clock back at once, so only numbers are shown.
    void paintLive(HDC dc) {
        const Json& g = live_;
        bool live = liveReason_.empty() && g.is_object();
        auto number = [&](const Json& j, const char* key) -> std::optional<double> {
            if (!live || !j.is_object() || !j.contains(key) || !j[key].is_number()) return {};
            return j[key].get<double>();
        };
        auto rail = [&](const char* key) { return live && g.contains(key) ? g[key] : Json(nullptr); };
        auto nv = rail("nvvdd"), ms = rail("msvdd"), input = rail("boardInput");
        auto clock = number(g, "clockMhz"), maxClock = number(g, "maxClockMhz"), power = number(g, "gpuPowerMw"),
             enforced = number(g, "enforcedMw"), core = number(g, "corePowerMw"),
             ipc = number(g, "boardInputLimiterActivity");
        auto nvNow = number(nv, "measuredMa"), nvLimit = number(nv, "liveLimitMa"), msNow = number(ms, "measuredMa"),
             msLimit = number(ms, "liveLimitMa"), inputNow = number(input, "measuredMa"),
             inputLimit = number(input, "liveLimitMa");
        auto ratio = [](std::optional<double> v, std::optional<double> limit) -> std::optional<double> {
            if (v && limit && *limit > 0) return *v / *limit;
            return {};
        };
        auto percent = [](std::optional<double> r) {
            return r ? std::to_wstring(std::lround(*r * 100)) + L"%" : std::wstring(L"--");
        };
        auto mhz = [](std::optional<double> v) {
            return v ? std::to_wstring(std::lround(*v)) + L" MHz" : std::wstring(L"--");
        };
        auto watts = [](std::optional<double> mw) { return mw ? decimal(*mw / 1000) + L" W" : std::wstring(L"--"); };
        auto amps = [](std::optional<double> ma, int places) {
            return ma ? (places ? decimal(*ma / 1000, places) : compact(*ma / 1000)) + L" A" : std::wstring(L"--");
        };
        auto cap = [&](const wchar_t* controller, std::optional<double> c) {
            if (!c) return std::wstring(L"--");
            auto s = controller + (L" " + mhz(c));
            if (clock) {
                auto above = std::lround(*c - *clock);
                s += L" (";
                s += above > 0 ? L"+" : L"";
                s += std::to_wstring(above) + L")";
            }
            return s;
        };
        std::optional<double> rest;
        if (power && core && *power >= *core) rest = *power - *core;
        // From the reference, tuned on an RTX 5090 Laptop: GPU power and the core rails against what each reads
        // while limiting. Board input is enforced on fast peaks, so its average stays well below the limit while
        // limiting. IPC reached at most 37% from power peaks alone and 93.5% with board input clearly limiting.
        COLORREF text = GetSysColor(COLOR_BTNTEXT), gray = GetSysColor(COLOR_GRAYTEXT);
        auto powerColor = tint(ratio(power, enforced), 0.94, 0.99), nvColor = tint(ratio(nvNow, nvLimit), 0.89, 0.99),
             msColor = tint(ratio(msNow, msLimit), 0.90, 0.99),
             inputColor = tint(ratio(inputNow, inputLimit), 0.68, 0.81);
        struct Cell {
            std::wstring text;
            COLORREF color;
        };
        std::array<std::array<Cell, 5>, 8> cells{};
        for (int i = 1; i < 5; ++i) cells[0][i] = {liveCaptions[i - 1], gray};
        auto row = [&](int r, COLORREF color, std::wstring setting, std::wstring now, std::wstring share,
                       std::wstring limiter) {
            cells[r] = {
                {{liveCaptions[r + 3], color}, {setting, color}, {now, color}, {share, color}, {limiter, text}}};
        };
        row(1, text, maxClock ? L"max " + mhz(maxClock) : L"--", mhz(clock), percent(ratio(clock, maxClock)), L"");
        row(2, powerColor, watts(enforced), watts(power), percent(ratio(power, enforced)),
            cap(L"power", number(g, "powerCapMhz")));
        row(3, text, L"", watts(core), L"", L"");
        row(4, text, L"", watts(rest), L"", L"");
        // One current controller caps both rails, so its cap spans the NVVDD and MSVDD rows.
        row(5, nvColor, amps(nvLimit, 0), amps(nvNow, 1), percent(ratio(nvNow, nvLimit)),
            cap(L"current", number(g, "currentCapMhz")));
        row(6, msColor, amps(msLimit, 0), amps(msNow, 1), percent(ratio(msNow, msLimit)), L"");
        row(7, inputColor, amps(inputLimit, 0), amps(inputNow, 2), percent(ratio(inputNow, inputLimit)),
            L"IPC active " + (ipc ? percent(std::min(*ipc, 1.0)) : std::wstring(L"--")));
        cells[7][4].color = tint(ipc, 0.52, 0.80);
        if (!live)
            for (int r = 1; r < 8; ++r)
                for (int i = 1; i < 5; ++i)
                    if (r != 6 || i != 4) cells[r][i].text = L"--";
        TEXTMETRICW metrics{};
        SelectObject(dc, font_);
        GetTextMetricsW(dc, &metrics);
        int line = metrics.tmHeight, height = ref(6) + line, pad = ref(6), gap = ref(12);
        auto part = [](int r) { return r == 3 || r == 4; };
        auto font = [&](int r, int i) { return i == 0 && !part(r) ? bold_ : font_; };
        auto extent = [&](int r, int i) {
            SelectObject(dc, font(r, i));
            SIZE size{};
            GetTextExtentPoint32W(dc, cells[r][i].text.c_str(), int(cells[r][i].text.size()), &size);
            return int(size.cx);
        };
        std::array<int, 5> column{};
        int next = liveTable_.left;
        for (int i = 0; i < 5; ++i) {
            int widest = 0;
            for (int r = 0; r < 8; ++r) widest = std::max(widest, extent(r, i) + (i == 0 && part(r) ? gap : 0));
            column[i] = next + (i ? gap : 0) + pad;
            next = column[i] + widest + pad;
        }
        for (int r = 0; r < 8; ++r)
            for (int i = 0; i < 5; ++i) {
                auto& cell = cells[r][i];
                if (cell.text.empty()) continue;
                int cx = column[i] + (i == 0 && part(r) ? gap : 0), cy = liveTable_.top + r * height + ref(6);
                if (r == 5 && i == 4) cy += (2 * height - ref(6) - line) / 2;
                SelectObject(dc, font(r, i));
                SetTextColor(dc, cell.color);
                TextOutW(dc, cx, cy, cell.text.c_str(), int(cell.text.size()));
                if (r == 0 || i == 0) {
                    TOOLINFOW tool{sizeof tool};
                    tool.hwnd = pages_[0].window;
                    tool.uId = r == 0 ? i : r + 4;
                    tool.rect = {cx, cy, cx + extent(r, i), cy + line};
                    SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
                }
            }
    }
    // Why startup validation disabled a feature.
    std::wstring disabledReason(const wchar_t* what, const char* feature) const {
        return std::wstring(what) + L" disabled: " +
               w(state_.value("disabled", Json::object()).value(feature, std::string("register validation failed"))) +
               L".";
    }
    void refreshPower() {
        const Json& g = gpuTelemetry_;
        auto nvidia = g.is_object() ? g.value("nvidia", Json(nullptr)) : Json(nullptr);
        bool cpu = !(state_.contains("cpu") && state_["cpu"].is_null());
        explain(cpuSupportId, cpu ? L"" : disabledReason(L"CPU power controls", "cpuPower"));
        cpuNote_ =
            cpu && state_.contains("cpuError") ? L"CPU read failed: " + w(state_["cpuError"].get<std::string>()) : L"";
        auto preset = state_.contains("cpu") && state_["cpu"].is_object() ? state_["cpu"].value("preset", std::string{})
                                                                          : std::string{};
        if (!preset.empty()) preset[0] = char(std::toupper(static_cast<unsigned char>(preset[0])));
        cpuPreset_ = preset.empty()
                         ? L""
                         : L"No custom limits are set in the EC, so it uses a built-in preset. Showing its " +
                               w(preset) + L" preset; Apply sets these as custom limits.";
        explain(gpuSupportId, state_.contains("gpu") && state_["gpu"].is_null()
                                  ? disabledReason(L"GPU power controls", "gpuPower")
                                  : L"");
        auto unlockReason = g.is_object() ? g.value("unlockReason", std::string{}) : std::string{};
        explain(unlockSupportId, unlockReason.empty() ? L"" : L"Unavailable: " + w(unlockReason) + L".");
        explain(boardSupportId, g.is_object() && g.value("boardInputUnmatched", false)
                                    ? L"Board input current limit unavailable: no driver limit matches the GPU BIOS."
                                    : L"");
        powerHint_ = gpuError_;
        if (gpuError_.empty() && g.is_object()) {
            auto failed = g.value("powerError", std::string{});
            if (failed.empty()) failed = g.value("currentsError", std::string{});
            auto stock = [&](const char* key) { return compact(g[key]["stockMa"].get<int>() / 1000.0); };
            if (nvidia.is_string())
                powerHint_ = w(nvidia.get<std::string>());
            else if (!failed.empty())
                powerHint_ = L"NVIDIA readback failed: " + w(failed);
            else if (g.value("ceilingWatts", Json(nullptr)).is_number() && g.value("nvvdd", Json(nullptr)).is_object())
                powerHint_ = L"Driver ceiling " + std::to_wstring(g["ceilingWatts"].get<int>()) + L" W · enforced " +
                             std::to_wstring(g["enforcedWatts"].get<int>()) + L" W · stock currents " + stock("nvvdd") +
                             (g.value("msvdd", Json(nullptr)).is_object() ? L" / " + stock("msvdd") : L"") +
                             (g.value("boardInput", Json(nullptr)).is_object() ? L" / " + stock("boardInput") : L"") +
                             L" A";
        }
        auto liveNvidia = live_.is_object() ? live_.value("nvidia", Json(nullptr)) : Json(nullptr);
        liveReason_ = !live_.is_object()       ? L"not read yet"
                      : liveNvidia.is_string() ? w(liveNvidia.get<std::string>())
                      : live_.contains("telemetryError")
                          ? L"NVIDIA readback failed: " + w(live_["telemetryError"].get<std::string>())
                          : L"";
        auto dlppc = dlppcTip(g.is_object() ? g.value("dlppc", Json(nullptr)) : Json(nullptr));
        if (dlppc != dlppcTip_) {
            dlppcTip_ = std::move(dlppc);
            tip(reinterpret_cast<UINT_PTR>(get(dlppcId)), dlppcTip_, TTF_IDISHWND, TTM_UPDATETIPTEXTW);
        }
        // Text that changes the layout relays out the page; live readings only repaint the table.
        auto key = cpuNote_ + L'\n' + cpuPreset_ + L'\n' + powerHint_ + L'\n' + liveReason_ + L'\n' +
                   text(get(unlockId)) + L'\n' + bypassText_ +
                   std::to_wstring(int(advanced_) + 2 * state_.value("advanced", Json::object())
                                                            .value("coolerFlag", Json::object())
                                                            .value("available", false));
        for (int id : {cpuSupportId, gpuSupportId, unlockSupportId, boardSupportId, bypassSupportId})
            key += L'\n' + text(get(id));
        if (key != powerKey_) {
            powerKey_ = std::move(key);
            layoutPage(pages_[0]);
        } else
            InvalidateRect(pages_[0].window, &liveTable_, FALSE);
    }
    // The reference MainForm: the profile bar, the tabs and the monitor bar fill the window edge to edge.
    void layout() {
        if (!tabs_) return;
        RECT r{};
        GetClientRect(window_, &r);
        auto dc = GetDC(window_);
        auto previous = SelectObject(dc, font_);
        SIZE size{};
        GetTextExtentPoint32W(dc, L"Defaults ▼", 10, &size);
        SelectObject(dc, previous);
        ReleaseDC(window_, dc);
        int defaults = size.cx + ref(26), bar = layoutBar(r.right, r.bottom);
        for (int i = 0; i < 5; ++i) place(get(slotId + i), ref(7) + i * ref(126), ref(5), ref(120), ref(32));
        place(get(saveId), ref(637), ref(5), ref(80), ref(32));
        place(get(readId), r.right - defaults - ref(86), ref(5), ref(80), ref(32));
        place(get(defaultsId), r.right - defaults, 0, defaults, ref(40));
        place(tabs_, 0, ref(40), r.right, bar - ref(40));
        RECT page{};
        GetClientRect(tabs_, &page);
        TabCtrl_AdjustRect(tabs_, FALSE, &page);
        MapWindowPoints(tabs_, window_, reinterpret_cast<POINT*>(&page), 2);
        for (auto& p : pages_) {
            place(p.window, page.left, page.top, page.right - page.left, page.bottom - page.top);
            layoutPage(p);
        }
        place(progress_, 1, bar + 1, r.right - 2, ref(5));
        InvalidateRect(window_, &bar_, FALSE);
    }
    // The reference MonitorPanel: bold readings between gray bars, wrapping like its flow layout onto at least two
    // lines, then the latest status in gray. Returns the bar's top.
    int layoutBar(int width, int bottom) {
        auto dc = GetDC(window_);
        auto previous = SelectObject(dc, bold_);
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        int line = metrics.tmHeight, pitch = line + ref(2), pad = line / 6, left = 1 + ref(4),
            right = width - 1 - ref(4), x = left, y = 1 + ref(6), lowest = y + pitch + line;
        std::vector<Text> texts;
        auto gray = GetSysColor(COLOR_GRAYTEXT);
        auto put = [&](const std::wstring& s, bool bold, COLORREF color, int margin) {
            // The reference measured its labels slightly wider than GDI does, hence the extra space after each.
            SelectObject(dc, bold ? bold_ : font_);
            SIZE size{};
            GetTextExtentPoint32W(dc, s.c_str(), int(s.size()), &size);
            int space = 2 * (margin + pad) + ref(3);
            if (x > left && x + size.cx + space > right) {
                x = left;
                y += pitch;
            }
            RECT r{x + margin + pad, y, right, y + line};
            // Only a long status can be wider than the bar; it wraps on its own lines.
            if (x + size.cx + space > right) {
                r.right -= margin + pad;
                DrawTextW(dc, s.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            }
            texts.push_back({r, s, color, bold ? bold_ : font_});
            x += size.cx + space;
            lowest = std::max(lowest, int(r.bottom));
        };
        for (size_t i = 0; i < monitor_.size(); ++i) {
            put(monitor_[i], true, GetSysColor(COLOR_BTNTEXT), ref(2));
            if (i + 1 < monitor_.size() || !status_.empty()) put(L"|", true, gray, 0);
        }
        if (!status_.empty()) put(status_, false, gray, ref(2));
        SelectObject(dc, previous);
        ReleaseDC(window_, dc);
        bar_ = {0, bottom - lowest - ref(5), width, bottom};
        for (auto& t : texts) OffsetRect(&t.rect, 0, bar_.top);
        barText_ = std::move(texts);
        return bar_.top;
    }
    void updateBar() {
        RECT r{};
        GetClientRect(window_, &r);
        int top = bar_.top;
        if (layoutBar(r.right, r.bottom) != top)
            layout();
        else
            InvalidateRect(window_, &bar_, FALSE);
    }
    void paintBar(HDC target) {
        int width = bar_.right - bar_.left, height = bar_.bottom - bar_.top;
        if (width <= 0 || height <= 0) return;
        auto dc = CreateCompatibleDC(target);
        auto bitmap = CreateCompatibleBitmap(target, width, height);
        auto oldBitmap = SelectObject(dc, bitmap);
        RECT r{0, 0, width, height};
        FillRect(dc, &r, GetSysColorBrush(COLOR_3DLIGHT));
        FrameRect(dc, &r, GetSysColorBrush(COLOR_WINDOWFRAME));
        SetBkMode(dc, TRANSPARENT);
        auto oldFont = SelectObject(dc, font_);
        for (auto& t : barText_) {
            SelectObject(dc, t.font ? t.font : font_);
            SetTextColor(dc, t.color);
            RECT tr = t.rect;
            OffsetRect(&tr, 0, -bar_.top);
            DrawTextW(dc, t.text.c_str(), -1, &tr, DT_WORDBREAK | DT_NOPREFIX);
        }
        BitBlt(target, bar_.left, bar_.top, width, height, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
    bool flat(int id) const {
        return id == saveId || id == defaultsId || id == readId || (id >= slotId && id < slotId + 5);
    }
    bool emptySlot(int i) const { return state_.contains("slots") && !state_.at("slots").at(i).get<bool>(); }
    // The reference's flat profile buttons: the selected slot is filled with the highlight color, empty slots are gray.
    void drawButton(const DRAWITEMSTRUCT& d) {
        int id = int(d.CtlID);
        bool slot = id >= slotId && id < slotId + 5, active = slot && target_ == id - slotId + 1,
             disabled = (d.itemState & ODS_DISABLED) != 0;
        COLORREF fill = GetSysColor(active ? COLOR_HIGHLIGHT : COLOR_BTNFACE),
                 ink = GetSysColor(active                                         ? COLOR_HIGHLIGHTTEXT
                                   : disabled || (slot && emptySlot(id - slotId)) ? COLOR_GRAYTEXT
                                                                                  : COLOR_BTNTEXT);
        double shade = d.itemState & ODS_SELECTED ? 0.84 : hot_ == d.hwndItem && !disabled ? 0.92 : 1.0;
        fill = RGB(BYTE(GetRValue(fill) * shade), BYTE(GetGValue(fill) * shade), BYTE(GetBValue(fill) * shade));
        auto brush = CreateSolidBrush(fill);
        FillRect(d.hDC, &d.rcItem, brush);
        DeleteObject(brush);
        brush = CreateSolidBrush(ink);
        FrameRect(d.hDC, &d.rcItem, brush);
        DeleteObject(brush);
        auto old = SelectObject(d.hDC, active || id == saveId ? bold_ : font_);
        SetBkMode(d.hDC, TRANSPARENT);
        SetTextColor(d.hDC, ink);
        auto s = text(d.hwndItem);
        RECT r = d.rcItem;
        DrawTextW(d.hDC, s.c_str(), -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      (d.itemState & ODS_NOACCEL ? DT_HIDEPREFIX : 0));
        if ((d.itemState & ODS_FOCUS) && !(d.itemState & ODS_NOFOCUSRECT)) {
            r = d.rcItem;
            InflateRect(&r, -ref(3), -ref(3));
            DrawFocusRect(d.hDC, &r);
        }
        SelectObject(d.hDC, old);
    }
    void scroll(HWND window, UINT message, WPARAM wp) {
        for (auto& p : pages_)
            if (p.window == window) {
                if (message == WM_MOUSEWHEEL)
                    p.scroll -= GET_WHEEL_DELTA_WPARAM(wp) * scale(54) / WHEEL_DELTA;
                else {
                    SCROLLINFO si{sizeof si, SIF_ALL};
                    GetScrollInfo(window, SB_VERT, &si);
                    switch (LOWORD(wp)) {
                    case SB_LINEUP: p.scroll -= scale(28); break;
                    case SB_LINEDOWN: p.scroll += scale(28); break;
                    case SB_PAGEUP: p.scroll -= int(si.nPage); break;
                    case SB_PAGEDOWN: p.scroll += int(si.nPage); break;
                    case SB_THUMBTRACK: p.scroll = si.nTrackPos; break;
                    case SB_TOP: p.scroll = 0; break;
                    case SB_BOTTOM: p.scroll = si.nMax; break;
                    }
                }
                layoutPage(p);
                return;
            }
    }
    void reveal(HWND child) {
        if (int page = index(GetParent(child)); framed(page)) {
            auto& sections = pages_[page];
            RECT rc{}, r{};
            GetClientRect(sections.window, &rc);
            GetWindowRect(child, &r);
            MapWindowPoints(nullptr, sections.window, reinterpret_cast<POINT*>(&r), 2);
            if (r.top < 0)
                sections.scroll += r.top;
            else if (r.bottom > rc.bottom)
                sections.scroll += r.bottom - rc.bottom;
            else
                return;
            layoutPage(sections);
            return;
        }
        for (auto& p : pages_)
            for (auto& r : p.rows)
                if (r.control == child || r.number == child) {
                    RECT rc{};
                    GetClientRect(p.window, &rc);
                    int top = scale(r.y + 10), bottom = top + scale(r.height);
                    if (top < p.scroll)
                        p.scroll = top;
                    else if (bottom > p.scroll + rc.bottom)
                        p.scroll = bottom - rc.bottom;
                    layoutPage(p);
                    return;
                }
    }
    void selectPage() {
        int page = TabCtrl_GetCurSel(tabs_);
        for (int i = 0; i < 7; ++i) ShowWindow(pages_[i].window, i == page ? SW_SHOW : SW_HIDE);
        enable(readId, !busy_ && !(page == 1 && state_.value("linked", false)));
        if (ready_ && IsWindowVisible(window_)) bindPage(page);
        if (worker_) {
            worker_->request(UiPage{page});
            worker_->request(BatteryPage{page == 3});
        }
    }
    bool checked(int id) const { return SendMessageW(get(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
    void checked(int id, bool value) {
        if (!edited_.contains(id)) SendMessageW(get(id), BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    // Power, fan switch, failsafe trigger, lighting brightness and cooler
    // duty values are slider positions; the other sliders have an edit box.
    static bool positional(int id) {
        return (id >= pl1Id && id <= inputId) || id == switchMsId || id == thresholdId || id == coolerFanId ||
               id == coolerPumpId || (id >= zoneBaseId && id < zoneBaseId + 48 && (id - zoneBaseId) % 16 == 4);
    }
    // A lighting zone's R, G and B boxes. Unless strict, an invalid box keeps its last valid value.
    COLORREF color(int base, bool strict = true) const {
        int c[3]{};
        for (int n = 0; n < 3; ++n) {
            BOOL error = FALSE;
            c[n] = int(SendMessageW(get(base + 1 + n), UDM_GETPOS32, 0, reinterpret_cast<LPARAM>(&error)));
            if (error && strict) throw std::runtime_error("Enter color values from 0 to 255");
        }
        return RGB(c[0], c[1], c[2]);
    }
    void setColor(int base, COLORREF value) {
        int c[]{GetRValue(value), GetGValue(value), GetBValue(value)};
        for (int n = 0; n < 3; ++n)
            if (!edited_.contains(base + 1 + n)) SendMessageW(get(base + 1 + n), UDM_SETPOS32, 0, c[n]);
    }
    int number(int id) {
        if (positional(id)) return int(SendMessageW(get(id), TBM_GETPOS, 0, 0));
        auto value = utf8(text(get(id + 1000)));
        int n = 0;
        auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), n);
        if (value.empty() || error != std::errc{} || end != value.data() + value.size())
            throw std::runtime_error("Enter a whole number");
        int lo = int(SendMessageW(get(id), TBM_GETRANGEMIN, 0, 0)),
            hi = int(SendMessageW(get(id), TBM_GETRANGEMAX, 0, 0));
        if (n < lo || n > hi)
            throw std::runtime_error("Value is outside the displayed model range (" + std::to_string(lo) + "–" +
                                     std::to_string(hi) + ")");
        return n;
    }
    void value(int id, int v, int lo, int hi) {
        auto slider = get(id);
        if (SendMessageW(slider, TBM_GETRANGEMIN, 0, 0) != lo || SendMessageW(slider, TBM_GETRANGEMAX, 0, 0) != hi) {
            bool ticks = positional(id);
            SendMessageW(slider, TBM_SETRANGEMIN, FALSE, lo);
            SendMessageW(slider, TBM_SETRANGEMAX, !ticks, hi);
            if (ticks) {
                int tick = std::max(1, (hi - lo) / 10);
                SendMessageW(slider, TBM_SETTICFREQ, tick, 0);
                SendMessageW(slider, TBM_SETPAGESIZE, 0, tick);
                InvalidateRect(slider, nullptr, TRUE);
            }
        }
        if (!edited_.contains(id)) {
            SendMessageW(slider, TBM_SETPOS, TRUE, v);
            set(id + 1000, display(id, v));
        }
    }
    // Current sliders move in whole amps (board input: milliamps). An unmoved slider keeps the exact detected
    // limit, which some BIOSes define in fractional amps.
    int milliamps(int id, int position) const {
        int step = id == inputId ? 1 : 1000;
        auto i = detectedMa_.find(id);
        return i != detectedMa_.end() && position == i->second / step ? i->second : position * step;
    }
    std::wstring display(int id, int v) const {
        if (id == tempId || id == thresholdId) return std::to_wstring(v) + L" °C";
        if (id >= nvId && id <= inputId) return compact(milliamps(id, v) / 1000.0) + L" A";
        if (id >= pl1Id && id <= dbId) return std::to_wstring(v) + L" W";
        if (id == switchMsId) return std::to_wstring(v * 100) + L" ms";
        if ((id >= zoneBaseId && id < zoneBaseId + 48) || id == coolerFanId || id == coolerPumpId)
            return std::to_wstring(v) + L" %";
        return std::to_wstring(v);
    }
    void enable(int id, bool on) {
        EnableWindow(get(id), on);
        if (get(id + 1000)) EnableWindow(get(id + 1000), on);
    }
    void eraseEdits(int lo, int hi) {
        for (auto i = edited_.begin(); i != edited_.end();)
            if (*i >= lo && *i <= hi)
                i = edited_.erase(i);
            else
                ++i;
    }
    void busy(bool on) {
        busy_ = on;
        for (auto& p : pages_) EnableWindow(p.window, !on);
        for (int id :
             std::array<int, 8>{saveId, defaultsId, readId, slotId, slotId + 1, slotId + 2, slotId + 3, slotId + 4})
            EnableWindow(get(id), !on);
        ShowWindow(progress_, on ? SW_SHOW : SW_HIDE);
        SendMessageW(progress_, PBM_SETMARQUEE, on, 60);
    }
    void request(ControlCommand command) {
        if (busy_) return;
        operation_ = command;
        busy(true);
        busySequence_ = ++sequence_;
        setStatus(L"Working… device operations may take a few seconds.");
        worker_->request(std::move(command), busySequence_);
    }
    void setStatus(const std::wstring& value) {
        status_ = value;
        updateBar();
    }
    bool confirm(const wchar_t* message, const wchar_t* title = L"MechControl") {
        return MessageBoxW(window_, message, title, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
    }
    void read() {
        int page = TabCtrl_GetCurSel(tabs_);
        const int lo[]{200, 300, 400, 500, 600, 800, 700};
        eraseEdits(lo[page], lo[page] + 99);
        if (page == 1) {
            boundCharts_.clear();
            request(FanRead{});
        } else
            request(UiRead{page});
    }
    void selectSlot(int slot) {
        if (busy_ || state_.empty()) return;
        if (!state_["slots"][slot - 1].get<bool>()) {
            target_ = slot;
            updateSlots();
            updateTray();
            setStatus(L"Empty slot selected as save target; live hardware unchanged.");
            return;
        }
        if (tripped_ &&
            !confirm(
                L"Loading a profile clears the thermal trip and reapplies that profile. Check your cooling. Continue?",
                L"Thermal Failsafe Tripped"))
            return;
        request(UiPreflight{{}, slot});
    }
    void updateSlots() {
        if (!state_.contains("names")) return;
        for (int i = 0; i < 5; ++i) {
            auto name = state_["names"][i];
            set(slotId + i, name.is_null() ? L"Profile " + std::to_wstring(i + 1) : w(name.get<std::string>()));
            InvalidateRect(get(slotId + i), nullptr, FALSE);
        }
    }
    void slotMenu(int slot, POINT p) {
        if (busy_) return;
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, renameId, L"Rename…");
        AppendMenuW(menu, MF_STRING, clearId, L"Clear Profile");
        int id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, window_, nullptr);
        DestroyMenu(menu);
        if (id == renameId) {
            auto value = rename(window_,
                                state_.contains("names") && !state_["names"][slot - 1].is_null()
                                    ? w(state_["names"][slot - 1].get<std::string>())
                                    : L"",
                                font_);
            if (value) request(SlotRename{slot, utf8(*value)});
        }
        if (id == clearId &&
            confirm(
                L"Delete this saved profile and reset its name? Settings currently applied to hardware are not changed.",
                L"Clear Profile"))
            request(SlotClear{slot});
    }
    void defaults() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, defaultId, L"Restore Firmware Defaults");
        if (state_.contains("modes"))
            for (size_t i = 0; i < state_["modes"].size(); ++i)
                AppendMenuW(menu, MF_STRING, defaultId + 1 + UINT(i), w(state_["modes"][i].get<std::string>()).c_str());
        RECT r{};
        GetWindowRect(get(defaultsId), &r);
        int id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, r.left, r.bottom, 0, window_, nullptr);
        DestroyMenu(menu);
        if (id &&
            confirm(
                id == defaultId
                    ? L"Apply firmware defaults? Changes are not saved to a profile. Owned NVIDIA power/current defaults will also be restored; the GPU may restart and the display may blink."
                    : L"Apply this mode's defaults? Changes are not saved to a profile.",
                L"Restore Defaults")) {
            edited_.clear();
            request(DefaultsApply{id == defaultId ? "" : state_["modes"][id - defaultId - 1].get<std::string>()});
        }
    }
    GpuSettings gpuIntent() {
        GpuSettings g;
        g.tgp = number(tgpId);
        g.dynamic_boost_enabled = checked(dbOnId);
        g.dynamic_boost_watts = number(dbId);
        if (IsWindowEnabled(get(nvId))) g.nvvdd_ma = milliamps(nvId, number(nvId));
        if (IsWindowEnabled(get(msId))) g.msvdd_ma = milliamps(msId, number(msId));
        if (IsWindowEnabled(get(inputId))) g.board_input_ma = milliamps(inputId, number(inputId));
        if (state_.contains("gpu") && !state_["gpu"].is_null())
            g.bios_override_enabled = state_["gpu"]["unlocked"].get<bool>();
        return g;
    }
    void applyPower() {
        if (IsWindowEnabled(get(pl1Id)))
            pendingCpu_ = CpuSettings{number(pl1Id), number(pl2Id), number(pl4Id), number(tempId)};
        if (IsWindowEnabled(get(tgpId)))
            request(UiPreflight{gpuIntent(), 0, pendingCpu_});
        else if (pendingCpu_) {
            auto c = *pendingCpu_;
            pendingCpu_.reset();
            eraseEdits(pl1Id, tempId);
            request(CpuApply{c});
        }
    }
    void applyPlan(const Json& plan) {
        bool restart = plan.value("restart", false), unlock = plan.value("unlock", false),
             was = plan.value("wasUnlocked", false);
        if (restart) {
            std::wstring message =
                unlock && !was
                    ? L"Raise the NVIDIA BIOS power ceiling and allow higher power/current limits? Higher limits increase heat and hardware stress. This is a persistent software override, not a BIOS flash.\n\n"
                    : L"Changing NVIDIA power/current limits requires a GPU restart.\n\n";
            message += L"The GPU will restart; the display may blink and GPU applications may close. Continue?";
            if (!confirm(message.c_str(), L"GPU BIOS Override")) {
                pendingCpu_.reset();
                return;
            }
        }
        int slot = plan.value("slot", 0);
        if (slot) {
            edited_.clear();
            target_ = 0;
            request(SlotLoad{slot});
        } else {
            eraseEdits(pl1Id, inputId);
            edited_.erase(dbOnId);
            auto cpu = pendingCpu_;
            pendingCpu_.reset();
            request(GpuApply{plan.at("gpu").get<GpuSettings>(), true, restart, cpu});
        }
    }
    FanSettings fanIntent() {
        auto s = state_.at("fan").get<FanSettings>();
        if (boundPages_[1]) {
            s.curve = checked(curveId);
            s.linked = checked(linkId);
        }
        if (boundPages_[1] && IsWindowEnabled(get(cpuChartId))) {
            Curve c;
            SendMessageW(get(cpuChartId), chartGetMessage, 0, reinterpret_cast<LPARAM>(&c));
            s.cpu_curve = std::move(c);
        }
        if (boundPages_[1] && IsWindowEnabled(get(gpuChartId))) {
            Curve c;
            SendMessageW(get(gpuChartId), chartGetMessage, 0, reinterpret_cast<LPARAM>(&c));
            s.gpu_curve = std::move(c);
        }
        return s;
    }
    ProtectionSettings protectionIntent() {
        return {checked(protectionId), number(thresholdId), checked(reduceId), checked(fanBoostId),
                checked(showWindowId)};
    }
    CoolerSettings coolerIntent() {
        CoolerSettings s;
        s.fan_on = checked(coolerFanOnId);
        s.fan_duty = number(coolerFanId);
        s.pump_on = checked(coolerPumpOnId);
        s.pump_duty = number(coolerPumpId);
        const PumpVoltage voltages[]{PumpVoltage::V7, PumpVoltage::V8, PumpVoltage::V11, PumpVoltage::V12};
        s.voltage = voltages[std::clamp(int(SendMessageW(get(voltageId), CB_GETCURSEL, 0, 0)), 0, 3)];
        auto c = color(coolerRgbOnId);
        s.rgb_on = checked(coolerRgbOnId);
        s.r = GetRValue(c);
        s.g = GetGValue(c);
        s.b = GetBValue(c);
        s.rgb_mode = CoolerRgbMode(SendMessageW(get(rgbModeId), CB_GETCURSEL, 0, 0));
        return s;
    }
    void coolerApply(CoolerSection section) {
        CoolerRequest c;
        c.action = CoolerAction::apply;
        c.section = section;
        c.persist = true;
        c.settings = coolerIntent();
        request(c);
    }
    void command(int id, int code) {
        if (updating_) return;
        if (code == EN_CHANGE && id >= 1200) {
            edited_.insert(id - 1000);
            hints();
            return;
        }
        if (code == EN_CHANGE && (id == cpuChartId || id == gpuChartId)) {
            edited_.insert(id);
            boundCharts_.erase(id);
            if (state_.value("linked", false) && !fanEditQueued_) {
                fanEditQueued_ = true;
                PostMessageW(window_, fanEditMessage, 0, 0);
            }
            hints();
            return;
        }
        if (id == showId) {
            show();
            return;
        }
        if (id == exitId) {
            DestroyWindow(window_);
            return;
        }
        if (id >= slotId && id < slotId + 5) {
            selectSlot(id - slotId + 1);
            return;
        }
        if (busy_ || !worker_) return;
        switch (id) {
        case saveId: {
            SlotSave save{target_ ? target_ : 1};
            save.editor_fan = fanIntent();
            if (coolerAvailable_ && std::any_of(edited_.begin(), edited_.end(),
                                                [](int id) { return id >= coolerFanOnId && id <= coolerRgbApplyId; }))
                save.editor_cooler = coolerIntent();
            request(std::move(save));
            break;
        }
        case readId:
        case powerReadId:
        case fanReadId: read(); break;
        case defaultsId: defaults(); break;
        case powerApplyId: applyPower(); break;
        case dbOnId:
            edited_.insert(id);
            enable(dbId, checked(id));
            break;
        case unlockId: {
            bool on = checked(unlockId);
            auto message =
                on ? L"Raise the NVIDIA BIOS power ceiling and allow lower TGP / higher current limits? Higher limits increase heat and hardware stress. This is a persistent software override, not a BIOS flash. The GPU will restart; the display may blink and GPU applications may close. Continue?"
                   : L"Restore original NVIDIA BIOS power/current limits? The GPU will restart; the display may blink and GPU applications may close. Continue?";
            if (confirm(message, L"GPU BIOS Override"))
                request(GpuUnlock{on});
            else {
                updating_ = true;
                SendMessageW(get(id), BM_SETCHECK, on ? BST_UNCHECKED : BST_CHECKED, 0);
                updating_ = false;
            }
            break;
        }
        case dlppcId: request(DlppcApply{checked(id)}); break;
        case bypassId:
        case coolerFlagId: {
            bool on = checked(id);
            updating_ = true;
            SendMessageW(get(id), BM_SETCHECK, on ? BST_UNCHECKED : BST_CHECKED, 0);
            updating_ = false; // The result rebinds it.
            if (id == coolerFlagId)
                request(CoolerFlag{on});
            else if (
                !on ||
                confirm(
                    L"Stop the EC from throttling your CPU power limits?\n\nPL1/PL2/PL4 apply under heavy load within about 24 s, the fans run at full speed and the battery covers what the adapter can't supply. It turns off on sleep, display off, AC loss, a failsafe trip and exit.",
                    L"Bypass EC Power Throttling"))
                request(EcBypassApply{on});
            break;
        }
        case boostId:
            edited_.insert(id);
            hints();
            break;
        case boostApplyId:
            edited_.erase(boostId);
            request(FanBoost{checked(boostId)});
            break;
        case linkId: {
            auto s = fanIntent();
            s.curve = true;
            updating_ = true;
            SendMessageW(get(curveId), BM_SETCHECK, BST_CHECKED, 0);
            updating_ = false;
            eraseEdits(curveId, gpuChartId);
            request(FanApply{s});
            break;
        }
        case switchId:
            edited_.insert(id);
            EnableWindow(get(switchMsId), checked(id));
            break; // The speed moves only while enabled, as in the reference.
        case switchApplyId:
            eraseEdits(switchId, switchMsId);
            request(SwitchApply{checked(switchId), number(switchMsId) * 100});
            break;
        case fanApplyId: {
            if (!state_.value("curvesSupported", false)) {
                request(FanMaster{checked(curveId)});
                break;
            }
            auto s = fanIntent();
            eraseEdits(curveId, gpuChartId);
            request(FanApply{s});
            break;
        }
        case protectionApplyId: request(ProtectionApply{protectionIntent()}); break;
        case resetId:
            if (confirm(
                    L"Check cooling, then reset the failsafe and reapply the active profile's power and fan settings?",
                    L"Reset Thermal Failsafe"))
                request(ThermalReset{});
            break;
        case testId: {
            auto applied = state_.at("protection").get<ProtectionSettings>();
            auto description =
                actions(applied) + L"\n\nThis really applies emergency settings and latches protection. ";
            if (Json(protectionIntent()) != Json(applied))
                description += L"Unapplied control changes are NOT part of this test. ";
            description += L"Continue?";
            if (confirm(description.c_str(), L"Test Failsafe")) request(ThermalTest{applied});
            break;
        }
        case batteryApplyId:
            edited_.erase(batteryModeId);
            request(BatteryApply{int(SendMessageW(get(batteryModeId), CB_GETCURSEL, 0, 0))});
            break;
        case lightingReadId:
            eraseEdits(zoneBaseId, dimmingId);
            request(UiRead{4});
            break;
        case idleId:
            if (code == CBN_SELCHANGE) {
                const int seconds[]{0, 10, 30, 60, 120, 300, 600};
                request(LightingIdle{seconds[SendMessageW(get(id), CB_GETCURSEL, 0, 0)]});
            }
            break;
        case dimmingId: request(DimmingApply{checked(id)}); break;
        case scanId: request(CoolerRequest{}); break;
        case connectId: {
            CoolerRequest c;
            c.action = coolerConnected_ ? CoolerAction::disconnect : CoolerAction::connect;
            if (!coolerConnected_) {
                auto i = SendMessageW(get(devicesId), CB_GETCURSEL, 0, 0);
                if (i < 0 || size_t(i) >= devices_.size()) {
                    setStatus(L"Select a cooler first.");
                    break;
                }
                c.address = devices_[size_t(i)].at("address").get<uint64_t>();
            }
            request(c);
            break;
        }
        case autoConnectId: {
            CoolerRequest c;
            c.action = CoolerAction::auto_connect;
            c.auto_connect = checked(id);
            request(c);
            break;
        }
        case coolerFanApplyId: coolerApply(CoolerSection::fan); break;
        case coolerPumpApplyId: coolerApply(CoolerSection::pump); break;
        case coolerRgbApplyId: coolerApply(CoolerSection::rgb); break;
        default:
            if (id >= zoneBaseId && id < zoneBaseId + 48) {
                int zone = (id - zoneBaseId) / 16, base = zoneBaseId + zone * 16, field = id - base;
                if (field == 5 || field == 6) {
                    const int wire[]{0, 2, 3};
                    auto c = color(base);
                    Zone s{checked(base), GetRValue(c), GetGValue(c), GetBValue(c), number(base + 4)};
                    eraseEdits(base, base + 4);
                    request(LightingApply{wire[zone], s, field == 6});
                } else if (field == 7) {
                    COLORREF colors[16]{};
                    CHOOSECOLORW choice{sizeof choice};
                    choice.hwndOwner = window_;
                    choice.rgbResult = color(base, false);
                    choice.lpCustColors = colors;
                    choice.Flags = CC_FULLOPEN | CC_RGBINIT;
                    if (ChooseColorW(&choice)) {
                        eraseEdits(base + 1, base + 3);
                        updating_ = true;
                        setColor(base, choice.rgbResult);
                        updating_ = false;
                        for (int n = 1; n <= 3; ++n) edited_.insert(base + n);
                    }
                } else
                    edited_.insert(id);
            } else if (code == BN_CLICKED || code == CBN_SELCHANGE)
                edited_.insert(id);
            break;
        }
        hints();
    }
    std::wstring actions(const ProtectionSettings& p) const {
        auto limits = state_.value("actionLimits", Json::object());
        int supported = state_.value("thermalActions", 0);
        std::wstring s = L"On trip: ";
        if (supported & Gpu)
            s += L"request this model's minimum GPU power (" + std::to_wstring(limits.value("gpu", 0)) +
                 L" W), disable Dynamic Boost";
        else
            s += L"GPU protection unavailable on this model";
        if (p.reduce_cpu && (supported & Cpu))
            s += L"; CPU PL1/PL2/PL4 → " + std::to_wstring(limits.value("pl1", 0)) + L"/" +
                 std::to_wstring(limits.value("pl2", 0)) + L"/" + std::to_wstring(limits.value("pl4", 0)) +
                 L" W (temperature limit unchanged)";
        if (p.fan_boost && (supported & Fan)) s += L"; enable laptop Fan Boost";
        if (p.reduce_cpu && !(supported & Cpu)) s += L"; CPU reduction unavailable";
        if (p.fan_boost && !(supported & Fan)) s += L"; Fan Boost unavailable";
        return s + L".";
    }
    void hints() {
        if (!ready_ || !IsWindowVisible(window_)) return;
        int page = TabCtrl_GetCurSel(tabs_);
        if (page == 1 && state_.contains("fan")) refreshFans();
        if (page == 2 && state_.contains("protection")) refreshThermal();
        if (page == 4)
            for (auto& r : swatches_) InvalidateRect(pages_[4].window, &r, FALSE);
        if (page == 5) InvalidateRect(pages_[5].window, &coolerSwatch_, FALSE);
    }
    // The reference's failsafe texts: the pending actions, the Apply/Save state, the protection state, the last trip,
    // configuration warnings and the readings. A changed text relays out the page; new readings only repaint theirs.
    void refreshThermal() {
        auto applied = state_["protection"], saved = state_.value("saved", Json(nullptr)),
             history = state_.value("thermal", Json::object());
        auto pending = protectionIntent();
        std::wstring hint = Json(pending) != applied                               ? L"Changes not applied"
                            : saved.is_null()                                      ? L"No saved profile loaded"
                            : saved.value("gpuFailsafe", Json(nullptr)) != applied ? L"Applied - not saved to profile"
                                                                                   : L"Settings match saved profile";
        auto trip = history.value("LastTrip", Json(nullptr));
        auto reason = trip.is_object() ? w(trip.value("Reason", std::string{}))
                                       : std::wstring(L"Saved protection state uncertain");
        auto state = w(state_.value("thermalStatus", std::string{}));
        if (tripped_) {
            std::wstring held;
            int holds = history.value("Held", 0);
            auto protection = applied.get<ProtectionSettings>();
            for (auto [action, name] : {std::pair(Gpu, L"GPU"), std::pair(Fan, L"fan boost"), std::pair(Cpu, L"CPU")})
                if (holds & action) held += (held.empty() ? L"" : L", ") + std::wstring(name);
            state += L"\nHeld actions: " + held + L". " + reason + w(history.value("RecoveryDetails", std::string{})) +
                     L"\nStop workloads if protection is incomplete. To restore normal settings, Reset" +
                     (protection.enabled ? L" (the GPU average must be below the " +
                                               std::to_wstring(protection.threshold) + L"°C trigger)"
                                         : L"") +
                     L" or load a profile that matches your cooling.";
        }
        // Unknown optional values are null. Temperatures show one decimal at most.
        auto degrees = [](const Json& v) {
            return v.is_number() ? compact(std::round(v.get<double>() * 10) / 10) + L"°C" : std::wstring(L"--");
        };
        std::wstring last = L"--";
        if (trip.is_object()) {
            auto field = [&](const char* key) { return trip.value(key, Json(nullptr)); };
            last = (field("Time").is_string() ? localTime(field("Time").get<std::string>()) : L"Time unavailable") +
                   L": " + reason;
            if (field("TemperatureC").is_number())
                last +=
                    L"\nGPU " + degrees(field("TemperatureC")) + L"; threshold " + degrees(field("ThresholdC")) + L".";
            last += L"\n" + w(trip.value("ActionDescription", std::string{}));
            auto outcomes = trip.value("Results", Json::object());
            std::wstring list;
            for (auto& [key, outcome] : outcomes.items())
                list += (list.empty() ? L"" : L"; ") + w(key) + L": " +
                        (outcome.is_string() ? w(outcome.get<std::string>()) : L"--");
            if (!list.empty()) last += L"\n" + list;
        }
        auto reading = [&](const char* key) { return degrees(state_.value(key, Json(nullptr))); };
        auto now = reading("temperature"), average = reading("average");
        if (now != L"--" && average != L"--") now += L" (5 s average " + average + L")";
        std::array<std::wstring, 7> texts{actions(pending), hint, state, last, w(warning_), now, reading("peak")};
        bool relayout = !std::equal(texts.begin(), texts.begin() + 5, thermalText_.begin()),
             repaint = texts != thermalText_;
        thermalText_ = std::move(texts);
        if (relayout)
            layoutPage(pages_[2]);
        else if (repaint)
            InvalidateRect(pages_[2].window, &thermalLive_, FALSE);
    }
    // The reference's curve checks under the curves: "Valid", or one line per problem.
    std::wstring fanValidation() {
        std::wstring errors;
        bool curves = false;
        for (auto [id, name] : {std::pair(cpuChartId, L"CPU: "), std::pair(gpuChartId, L"GPU: ")}) {
            if (!IsWindowEnabled(get(id))) continue;
            Curve c;
            SendMessageW(get(id), chartGetMessage, 0, reinterpret_cast<LPARAM>(&c));
            curves = true;
            auto add = [&](const std::wstring& s) { errors += (errors.empty() ? L"" : L"\n") + (name + s); };
            auto n = [](int v) { return std::to_wstring(v); };
            if (!c.duty.empty() && c.duty[0] / 2)
                add(L"Point 0: duty must be 0% (EC rejects table if duty[0] != 0), got " + n(c.duty[0] / 2) + L"%");
            for (size_t i = 0; i + 1 < c.temp_up.size(); ++i) {
                auto point = L"Point " + n(int(i));
                if (c.temp_up[i] + 4 > c.temp_up[i + 1])
                    add(point + L": tempUp (" + n(c.temp_up[i]) + L") must be at least 4°C below point " +
                        n(int(i + 1)) + L" tempUp (" + n(c.temp_up[i + 1]) + L")");
                if (c.duty[i] / 2 > c.duty[i + 1] / 2)
                    add(point + L": duty (" + n(c.duty[i] / 2) + L"%) must be <= point " + n(int(i + 1)) + L" duty (" +
                        n(c.duty[i + 1] / 2) + L"%)");
            }
        }
        return !curves ? L"" : errors.empty() ? L"Valid" : errors;
    }
    // Sections and validation text change the layout; the linked status only repaints its bar.
    void refreshFans() {
        fanValidation_ = fanValidation();
        bool fans = state_.value("enabled", std::set<std::string>{}).contains("fanControl");
        auto key = fanValidation_ + L'\n' +
                   std::to_wstring(state_.value("linked", false) + 2 * state_.value("curvesSupported", false) +
                                   4 * state_.value("hasGpuFan", false) + 8 * state_.value("linkAvailable", false) +
                                   16 * fans);
        if (key != fanKey_) {
            fanKey_ = std::move(key);
            layoutPage(pages_[1]);
        } else
            InvalidateRect(pages_[1].window, &linkedBar_, FALSE);
    }
    void chart(int id, const Json& value, bool readOnly, bool linear, int dutyBase) {
        ChartOptions options{readOnly, linear, dutyBase};
        auto bound = boundCharts_.find(id);
        if (bound != boundCharts_.end() && bound->second.first == value && bound->second.second == options) return;
        boundCharts_[id] = {value, options};
        SendMessageW(get(id), chartOptionsMessage, 0, reinterpret_cast<LPARAM>(&options));
        std::optional<Curve> curve;
        if (!value.is_null()) curve = value.get<Curve>();
        SendMessageW(get(id), chartSetMessage, 0, curve ? reinterpret_cast<LPARAM>(&*curve) : 0);
    }
    void bindPage(int page) {
        if (!ready_ || !IsWindowVisible(window_)) return;
        updating_ = true;
        auto features = state_.value("enabled", std::set<std::string>{});
        bool linked = state_.value("linked", false);
        enable(readId, !busy_ && !(page == 1 && linked));
        if (page == 0) {
            bool cpu = state_.contains("cpu") && !state_["cpu"].is_null();
            for (int id : {pl1Id, pl2Id, pl4Id, tempId}) enable(id, cpu);
            if (cpu)
                for (auto [id, key] : {std::pair(pl1Id, "pl1"), std::pair(pl2Id, "pl2"), std::pair(pl4Id, "pl4"),
                                       std::pair(tempId, "tempLimit")}) {
                    auto& v = state_["cpu"][key];
                    value(id, v["value"], v["min"], v["max"]);
                }
            bool gpu = !gpuTelemetry_.is_null() && gpuError_.empty();
            for (int id : {tgpId, dbOnId}) enable(id, gpu);
            if (gpu)
                gpuLive(gpuTelemetry_, powerRead_);
            else
                for (int id : {nvId, msId, inputId, unlockId, dlppcId}) enable(id, false);
            enable(dbId, gpu && checked(dbOnId));
            powerRead_ = false;
            enable(powerApplyId, cpu || gpu);
            // The check boxes show the controller's state; clicks only request a change.
            auto advanced = state_.value("advanced", Json::object());
            auto bypass = advanced.value("ecBypass", Json::object()),
                 flag = advanced.value("coolerFlag", Json::object());
            bool active = bypass.value("active", false), clearing = bypass.value("clearing", false);
            auto status = w(bypass.value("status", std::string{}));
            checked(bypassId, active || clearing);
            enable(bypassId, bypass.value("available", false));
            checked(coolerFlagId, flag.value("connected", false));
            enable(coolerFlagId, flag.value("available", false));
            // The status line only explains an automatic turn-off or a failure to clear.
            bypassText_ = clearing || (!active && status != L"Off (turned off)") ? status : L"";
            bypassColor_ = clearing ? errorColor : GetSysColor(COLOR_GRAYTEXT);
            // Only the EC firmware image can show that a model or EC version handles the bypass the same way.
            explain(bypassSupportId,
                    bypass.value("available", false)
                        ? L""
                        : L"Unavailable: " + w(bypass.value("reason", std::string{})) + L".",
                    features.contains("cpuPower"));
            refreshPower();
        }
        if (page == 1 && state_.contains("fan")) {
            auto f = state_["fan"].get<FanSettings>();
            bool fans = features.contains("fanControl"), supported = state_.value("curvesSupported", false),
                 hasGpu = state_.value("hasGpuFan", false);
            checked(curveId, f.curve);
            checked(linkId, linked);
            enable(cpuChartId, supported && f.cpu_curve && !f.cpu_curve->temp_up.empty());
            enable(gpuChartId, supported && hasGpu && f.gpu_curve && !f.gpu_curve->temp_up.empty());
            // Link Fans owns the table writes, so the reference locks the curve switch, Apply and Read while it runs.
            enable(linkId, state_.value("linkAvailable", false));
            for (int id : {curveId, fanApplyId, fanReadId}) enable(id, fans && !linked);
            for (int id : {boostId, boostApplyId, switchId, switchApplyId}) enable(id, fans);
            if (!edited_.contains(cpuChartId))
                chart(cpuChartId, supported ? nullable(f.cpu_curve) : Json(nullptr), false, linked,
                      state_.value("dutyBaseCpu", 0));
            if (!edited_.contains(gpuChartId))
                chart(gpuChartId, supported && hasGpu ? nullable(f.gpu_curve) : Json(nullptr), false, linked,
                      state_.value("dutyBaseGpu", 0));
            if (linked)
                for (auto [id, key, base] : {std::tuple(debugCpuId, "writtenCpu", "dutyBaseCpu"),
                                             std::tuple(debugGpuId, "writtenGpu", "dutyBaseGpu")})
                    chart(id, state_.value(key, Json(nullptr)), true, true, state_.value(base, 0));
            if (state_.contains("boost")) checked(boostId, state_["boost"]);
            if (state_.contains("switchEnabled")) checked(switchId, state_["switchEnabled"]);
            if (state_.contains("switchMs"))
                value(switchMsId, std::clamp(state_["switchMs"].get<int>() / 100, 1, 127), 1, 127);
            EnableWindow(get(switchMsId), fans && checked(switchId));
        }
        if (page == 2 && state_.contains("protection")) {
            auto p = state_["protection"].get<ProtectionSettings>();
            checked(protectionId, p.enabled);
            value(thresholdId, p.threshold, 50, 95);
            checked(reduceId, p.reduce_cpu);
            checked(fanBoostId, p.fan_boost);
            checked(showWindowId, p.show_window);
            int a = state_.value("thermalActions", 0);
            for (int id : {protectionId, thresholdId, protectionApplyId, showWindowId}) enable(id, (a & Gpu) != 0);
            enable(testId, (a & Gpu) != 0 && !tripped_);
            enable(resetId, tripped_);
            enable(reduceId, (a & Cpu) != 0);
            enable(fanBoostId, (a & Fan) != 0);
        }
        if (page == 3) {
            bool battery = features.contains("battery");
            for (int id : {batteryModeId, batteryApplyId}) enable(id, battery);
            explain(batterySupportId, battery ? L"" : disabledReason(L"Battery charge mode and telemetry", "battery"));
            ShowWindow(get(batterySupportId), battery ? SW_HIDE : SW_SHOW);
            ShowWindow(get(batteryTextId), battery ? SW_SHOW : SW_HIDE); // The note takes the telemetry's place.
            if (state_.contains("batteryMode") && !edited_.contains(batteryModeId))
                SendMessageW(get(batteryModeId), CB_SETCURSEL, state_["batteryMode"].get<int>(), 0);
        }
        if (page == 6) refreshInfo();
        if (page >= 3 && page <= 5) bindPeripherals(page);
        boundPages_[page] = true;
        updating_ = false;
        hints();
    }
    void presentation(const Json& j, bool initial) {
        int readPage = j.value("readPage", -3), old = state_.value("activeSlot", -1);
        if (j.contains("fan") && state_.contains("fan") && j["fan"] != state_["fan"] &&
            !std::any_of(edited_.begin(), edited_.end(), [](int id) { return id >= curveId && id <= gpuChartId; }))
            boundPages_[1] = false;
        if (readPage == 0) {
            for (auto key : {"cpu", "cpuError", "gpu", "gpuError"}) state_.erase(key);
            powerRead_ = true;
        }
        if (readPage == 1) state_.erase("fanError");
        if (readPage == 3) state_.erase("batteryError");
        for (auto i = j.begin(); i != j.end(); ++i) state_[i.key()] = i.value();
        if (initial && !ready_) {
            ready_ = true;
            busy(false);
            setStatus(L"Controller ready.");
        }
        if (j.contains("identity")) {
            identity_ = j["identity"];
            SetWindowTextW(window_, (L"MechControl — " + w(state_.value("model", std::string{}))).c_str());
            layoutPage(pages_[6]);
        }
        bool selection = old != state_.value("activeSlot", 0);
        if (selection) target_ = state_.value("activeSlot", 0);
        if (selection || j.contains("slots") || j.contains("names")) updateSlots();
        bool wasTripped = tripped_;
        tripped_ = state_.value("tripped", false);
        warning_ = state_.value("warning", std::string{});
        if (!configurationWarning_.empty()) warning_ += (warning_.empty() ? "" : "\n") + configurationWarning_;
        if (j.contains("gpu")) {
            gpuTelemetry_ = live_ = j["gpu"];
            gpuError_.clear();
            state_.erase("gpuError");
        }
        if (j.contains("gpuError")) gpuError_ = L"GPU read failed: " + w(j["gpuError"].get<std::string>());
        if (j.contains("gpuLive")) live_ = j["gpuLive"];
        if (j.contains("gpuBudget")) gpuBudget_ = j["gpuBudget"];
        if (IsWindowVisible(window_)) {
            int page = TabCtrl_GetCurSel(tabs_);
            if (readPage >= 0 || readPage == -1 || initial || page == 1 || page == 2 || j.contains("gpuError") ||
                (page == 6 && j.contains("gpuBudget")) || (page == 0 && j.contains("advanced")))
                bindPage(page);
            else if (ready_ && page == 0 && j.contains("gpuLive"))
                refreshPower(); // Live readings repaint the table; the editor keeps its values.
        }
        int lost = state_.value("thermalLostAlerts", 0);
        bool protectionLost = lost > lostAlerts_;
        lostAlerts_ = lost;
        updateTray();
        if (tripped_ && (!wasTripped || protectionLost))
            alert(); // Like the source, also re-alert when verified protection is lost.
        if (alert_ && !tripped_) DestroyWindow(alert_);
        if (alert_)
            SetWindowTextW(alertText_, (w(state_.value("thermalStatus", std::string{})) +
                                        L"\r\n\r\nCheck the cooler. Closing this alert does NOT reset the failsafe.")
                                           .c_str());
    }
    void gpuLive(const Json& gpu, bool editor) {
        if (gpu.is_null()) return;
        gpuTelemetry_ = gpu;
        for (auto [id, key] : {std::pair(tgpId, "tgp"), std::pair(dbId, "dynamicBoostWatts")}) {
            auto& v = gpu.at(key);
            value(id, v["value"], v["min"], v["max"]);
        }
        checked(dbOnId, gpu.value("dynamicBoost", false));
        checked(unlockId, gpu.value("unlocked", false));
        checked(dlppcId, state_.value("dlppcDisabled", false));
        enable(unlockId, gpu.value("unlockAvailable", false));
        enable(dlppcId, gpu.value("dlppcAvailable", false) &&
                            !(gpu.contains("dlppc") && gpu["dlppc"].is_null())); // Clicking connects NVIDIA on demand.
        if (gpu.contains("unlockMin") && gpu.contains("unlockMax"))
            set(unlockId, L"Unlock GPU power / current limits (TGP " + std::to_wstring(gpu["unlockMin"].get<int>()) +
                              L"-" + std::to_wstring(gpu["unlockMax"].get<int>()) + L" W)");
        for (auto [id, key] : {std::pair(nvId, "nvvdd"), std::pair(msId, "msvdd"), std::pair(inputId, "boardInput")}) {
            auto& rail = gpu.at(key);
            bool previouslyEnabled = IsWindowEnabled(get(id)) != FALSE;
            enable(id, !rail.is_null());
            if (rail.is_null()) {
                set(id + 1000, L"--");
                continue;
            }
            if (editor || !previouslyEnabled) {
                // Positions round down, so the detected limit never sits above the slider maximum.
                int step = id == inputId ? 1 : 1000, limit = rail["limitMa"],
                    lo = (rail["minMa"].get<int>() + step - 1) / step;
                if (!edited_.contains(id)) detectedMa_[id] = limit;
                value(id, limit / step, lo, std::max(lo, rail["maxMa"].get<int>() / step));
            }
            set(id + 1000, display(id, number(id)));
        }
    }
    // System Info's readings, formatted as in the reference. A reading repaints its own cell; one that no longer
    // fits its cell relays out the page.
    void refreshInfo() {
        std::array<std::wstring, 8> texts;
        texts.fill(L"--");
        if (lastSensors_) {
            auto& s = *lastSensors_;
            auto positive = [](std::optional<int> v, const wchar_t* unit, const wchar_t* zero = L"--") {
                return !v ? std::wstring(L"--") : *v > 0 ? std::to_wstring(*v) + unit : std::wstring(zero);
            };
            texts[0] = positive(s.cpu_temp, L"°C");
            texts[1] = positive(s.gpu_temp, L"°C");
            if (s.adapter_amps && *s.adapter_amps > 0)
                texts[2] = decimal(*s.adapter_amps) + L" A (" + decimal(*s.adapter_amps * 19.5, 0) + L"W @ 19.5V)";
            texts[3] = positive(s.adapter_raw, L"");
            if (s.tgp_live) texts[4] = std::to_wstring(*s.tgp_live) + L" W";
            texts[5] = positive(s.db_live, L" W", L"off");
        }
        if (const Json& b = gpuBudget_; b.is_object()) {
            if (b.contains("reason"))
                texts[6] = texts[7] = L"-- (" + w(b.at("reason").get<std::string>()) + L")";
            else {
                texts[6] = std::to_wstring(b.at("laptopWatts").get<int>()) + L" W";
                texts[7] = std::to_wstring(b.at("platformWatts").get<int>()) + L" W";
            }
        }
        if (texts == infoText_) return;
        std::swap(texts, infoText_);
        auto window = pages_[6].window;
        auto dc = GetDC(window);
        auto previous = SelectObject(dc, font_);
        bool fits = true;
        for (int i = 0; i < 8; ++i)
            if (infoCells_[i].right > infoCells_[i].left &&
                infoWidth(dc, i) != infoCells_[i].right - infoCells_[i].left)
                fits = false;
        SelectObject(dc, previous);
        ReleaseDC(window, dc);
        if (!fits)
            layoutPage(pages_[6]);
        else
            for (int i = 0; i < 8; ++i)
                if (texts[i] != infoText_[i]) InvalidateRect(window, &infoCells_[i], FALSE);
    }
    // A reading's cell fits the reading and its widest usual value, so most changes only repaint the cell.
    int infoWidth(HDC dc, int i) const {
        constexpr const wchar_t* usual[]{L"000°C", L"000°C", L"00.0 A (000W @ 19.5V)", L"0000", L"000 W", L"000 W",
                                         L"000 W", L"000 W"};
        SelectObject(dc, font_);
        SIZE a{}, b{};
        GetTextExtentPoint32W(dc, usual[i], int(wcslen(usual[i])), &a);
        GetTextExtentPoint32W(dc, infoText_[i].c_str(), int(infoText_[i].size()), &b);
        return int(std::max(a.cx, b.cx));
    }
    void peripherals(const Json& j) {
        if (j.contains("battery")) peripheralState_.erase("batteryError");
        if (j.contains("lightingIntent"))
            for (auto key : {"logoError", "dimmingError"}) peripheralState_.erase(key);
        for (auto i = j.begin(); i != j.end(); ++i)
            if (i.key() != "ui") peripheralState_[i.key()] = i.value();
        if (j.contains("coolerAvailable")) coolerAvailable_ = j["coolerAvailable"].get<bool>();
        if (IsWindowVisible(window_)) {
            int page = TabCtrl_GetCurSel(tabs_);
            if ((page == 3 && (j.contains("battery") || j.contains("batteryError"))) ||
                (page == 4 && j.contains("lightingIntent")) ||
                (page == 5 && (j.contains("cooler") || j.contains("coolerAvailable"))))
                bindPeripherals(page);
        }
    }
    void bindPeripherals(int page) {
        const auto& j = peripheralState_;
        updating_ = true;
        if (page == 3 && j.contains("battery")) {
            const auto& b = j["battery"];
            auto voltage = b.value("designVoltageMv", 0) / 1000.0;
            auto capacity = [&](const char* key) {
                int n = b.value(key, 0);
                return std::to_wstring(n) + L" mAh (~" + decimal(n * voltage / 1000) + L" Wh)";
            };
            set(batteryTextId,
                L"Power source: " + std::wstring(b.value("onAC", false) ? L"AC (plugged in)" : L"Battery") +
                    L"\r\nCharge: " + std::to_wstring(b.value("chargePercent", 0)) + L"%\r\nTemperature: " +
                    decimal(b.value("temperatureCelsius", 0.0)) + L" °C\r\nCycle count: " +
                    std::to_wstring(b.value("cycleCount", 0)) + L"\r\nDesign capacity: " +
                    capacity("designCapacityMah") + L"\r\nFull charge capacity: " + capacity("fullChargeCapacityMah") +
                    L"\r\nRemaining capacity: " + capacity("remainingCapacityMah") + L"\r\nVoltage: " +
                    std::to_wstring(b.value("presentVoltageMv", 0)) + L" mV (design " +
                    std::to_wstring(b.value("designVoltageMv", 0)) + L" mV)\r\nCharge rate: " +
                    std::to_wstring(b.value("chargeRateMa", 0)) + L" mA\r\nHealth: " +
                    std::to_wstring(b.value("healthPercent", 0)) + L"%");
        }
        if (page == 3 && j.contains("batteryError"))
            set(batteryTextId,
                L"Battery read failed; previous telemetry is stale: " + w(j["batteryError"].get<std::string>()));
        if (page == 4 && j.contains("lightingIntent")) {
            auto s = j["lightingIntent"].get<LightingState>();
            std::optional<Zone> zones[]{s.keyboard, s.light_bar, s.logo};
            if (j.contains("logo")) zones[2] = j["logo"].get<Zone>();
            for (int i = 0; i < 3; ++i) {
                int base = zoneBaseId + i * 16;
                bool available = j.contains("zones") && j["zones"][i].get<bool>() && zones[i].has_value();
                if (i == 2 && j.contains("logoError")) available = false;
                for (int n = 0; n <= 7; ++n) enable(base + n, available);
                if (zones[i]) {
                    auto z = *zones[i];
                    checked(base, z.enabled);
                    setColor(base, RGB(z.r, z.g, z.b));
                    value(base + 4, z.brightness, 0, 100);
                }
            }
            const int seconds[]{0, 10, 30, 60, 120, 300, 600};
            for (int i = 0; i < 7; ++i)
                if (seconds[i] == s.idle_seconds) SendMessageW(get(idleId), CB_SETCURSEL, i, 0);
        }
        if (page == 4) {
            if (j.contains("logoError"))
                setStatus(L"Logo read failed independently: " + w(j["logoError"].get<std::string>()));
            if (j.contains("dimmingError"))
                setStatus(L"Local dimming read failed independently: " + w(j["dimmingError"].get<std::string>()));
            if (j.contains("dimmingAvailable"))
                enable(dimmingId, j["dimmingAvailable"].get<bool>() && !j.contains("dimmingError"));
            if (j.contains("localDimming")) checked(dimmingId, j["localDimming"]);
        }
        if (page == 5) {
            std::wstring status = j.contains("coolerAvailable") && !coolerAvailable_
                                      ? L"This model does not report liquid cooler support."
                                      : L"Disconnected",
                         firmware;
            bool connected = false;
            if (coolerAvailable_ && j.contains("cooler")) {
                const auto& c = j["cooler"];
                cooler(c);
                connected = c.value("connected", false);
                firmware = w(c.value("firmware", std::string{}));
                if (!firmware.empty()) firmware = L"FW: " + firmware;
                // Scan and Connect check Bluetooth again first.
                status =
                    c.value("bluetoothAvailable", Json(nullptr)) == false
                        ? L"Bluetooth is off or unavailable. Turn it on in Windows Settings, then press Scan or Connect."
                        : w(c.value("status", std::string{}));
            }
            set(connectId, connected ? L"Disconnect" : L"Connect");
            for (int id : {devicesId, scanId, connectId, autoConnectId}) enable(id, coolerAvailable_);
            for (int id = coolerFanOnId; id <= coolerRgbApplyId; ++id)
                enable(id, connected); // As in the reference, settings change only while connected.
            // The status and firmware are painted; a change relays out the page.
            if (status != coolerStatus_ || firmware != coolerFirmware_ || connected != coolerConnected_) {
                coolerStatus_ = std::move(status);
                coolerFirmware_ = std::move(firmware);
                coolerConnected_ = connected;
                layoutPage(pages_[5]);
            }
        }
        updating_ = false;
        hints();
    }
    // The found and saved coolers, the auto-connect choice and, unless edited, the requested settings.
    void cooler(const Json& j) {
        auto devices = j.value("devices", Json::array());
        auto saved = j.value("state", Json::object());
        auto address = saved.value("savedAddress", uint64_t{});
        if (address &&
            std::none_of(devices.begin(), devices.end(), [&](const auto& d) { return d["address"] == address; }))
            devices.push_back({{"address", address},
                               {"name", saved.value("savedDeviceName", Json(nullptr)).is_string()
                                            ? saved["savedDeviceName"].get<std::string>()
                                            : "Cooler (saved)"}});
        if (devices != devices_) {
            uint64_t selected = 0;
            auto index = SendMessageW(get(devicesId), CB_GETCURSEL, 0, 0);
            if (index >= 0 && size_t(index) < devices_.size())
                selected = devices_[size_t(index)]["address"].get<uint64_t>();
            devices_ = std::move(devices);
            SendMessageW(get(devicesId), CB_RESETCONTENT, 0, 0);
            int choose = 0, i = 0;
            for (auto& d : devices_) {
                auto a = d["address"].get<uint64_t>();
                wchar_t mac[24]{};
                swprintf_s(mac, L"%02X:%02X:%02X:%02X:%02X:%02X", unsigned(a >> 40) & 255, unsigned(a >> 32) & 255,
                           unsigned(a >> 24) & 255, unsigned(a >> 16) & 255, unsigned(a >> 8) & 255, unsigned(a) & 255);
                auto name = w(d.value("name", std::string{})) + L" (" + mac + L")" + (a == address ? L" [saved]" : L"");
                SendMessageW(get(devicesId), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
                if (a == selected || (!selected && a == address)) choose = i;
                ++i;
            }
            if (!devices_.empty()) SendMessageW(get(devicesId), CB_SETCURSEL, choose, 0);
        }
        if (j.contains("state")) checked(autoConnectId, j["state"].value("autoConnect", false));
        if (j.contains("intent")) {
            auto s = j["intent"].get<CoolerSettings>();
            checked(coolerFanOnId, s.fan_on);
            value(coolerFanId, s.fan_duty, 0, 100);
            checked(coolerPumpOnId, s.pump_on);
            value(coolerPumpId, s.pump_duty, 0, 100);
            int voltage = s.voltage == PumpVoltage::V7    ? 0
                          : s.voltage == PumpVoltage::V8  ? 1
                          : s.voltage == PumpVoltage::V11 ? 2
                                                          : 3;
            if (!edited_.contains(voltageId)) SendMessageW(get(voltageId), CB_SETCURSEL, voltage, 0);
            checked(coolerRgbOnId, s.rgb_on);
            setColor(coolerRgbOnId, RGB(s.r, s.g, s.b));
            if (!edited_.contains(rgbModeId)) SendMessageW(get(rgbModeId), CB_SETCURSEL, int(s.rgb_mode), 0);
        }
    }
    void sensors(const Sensors& s) {
        lastSensors_ = s;
        const wchar_t* modes[]{L"High Capacity", L"Balanced", L"Stationary"};
        auto mode = s.battery_mode && *s.battery_mode >= 0 && *s.battery_mode < 3 ? modes[*s.battery_mode] : L"--";
        wchar_t raw[40]{};
        if (s.battery_raw) swprintf_s(raw, L" [0x%03X=0x%02X]", state_.value("batteryRegister", 0), *s.battery_raw);
        auto percent = [](auto x) { return x ? std::to_wstring(*x) + L"%" : std::wstring(L"--"); };
        auto on = [](auto x) { return x ? std::wstring(*x ? L"ON" : L"OFF") : L"--"; };
        std::array<std::wstring, 6> monitor{
            L"CPU Fan: " + percent(s.cpu_percent) + L" (" + std::to_wstring(s.cpu_rpm) + L" RPM)",
            L"GPU Fan: " + percent(s.gpu_percent) + L" (" + std::to_wstring(s.gpu_rpm) + L" RPM)",
            L"CPU Temp Limit: " + (s.cpu_limit ? std::to_wstring(*s.cpu_limit) + L"°C" : L"--"),
            L"Battery: " + percent(s.battery_soc) + L" (" + mode + L")" + raw,
            L"Fan Boost: " + on(s.boost),
            L"Fan Curve: " + on(s.curve_enabled)};
        if (monitor != monitor_) {
            monitor_ = std::move(monitor);
            updateBar();
        }
        if (TabCtrl_GetCurSel(tabs_) == 6) refreshInfo();
        if (TabCtrl_GetCurSel(tabs_) == 1)
            for (auto [id, temp] : {std::pair(cpuChartId, s.cpu_temp), std::pair(debugCpuId, s.cpu_temp),
                                    std::pair(gpuChartId, s.gpu_temp), std::pair(debugGpuId, s.gpu_temp)})
                if (IsWindowVisible(get(id))) SendMessageW(get(id), chartTemperatureMessage, temp ? *temp + 1 : 0, 0);
    }
    void results() {
        for (auto& result : delivery_->take()) {
            if (result.kind == ResultKind::selection) {
                selectModel(result.selection);
                continue;
            }
            if (result.kind == ResultKind::warning) {
                setStatus(L"EC initialization had errors; see mechcontrol.log.");
                MessageBoxW(window_, result.text.c_str(), L"EC Init Warning", MB_OK | MB_ICONWARNING);
                continue;
            }
            if (result.kind == ResultKind::failure) {
                MessageBoxW(window_,
                            (result.text +
                             L"\nMake sure UWACPIDriver is installed and run as Administrator. See mechcontrol.log.")
                                .c_str(),
                            L"MechControl — Controller Error", MB_OK | MB_ICONERROR);
                DestroyWindow(window_);
                PostQuitMessage(1);
                return;
            }
            bool own = result.sequence && result.sequence == busySequence_;
            if (own) busy(false);
            if (result.control) {
                auto& r = *result.control;
                if (r.peripherals) {
                    auto& p = *r.peripherals;
                    if (p.contains("ui")) {
                        presentation(p["ui"], p["ui"].contains("identity"));
                        if (own)
                            setStatus(
                                L"Read completed. Independent errors are shown without replacing successful values.");
                    }
                    peripherals(p);
                    if (p.contains("plan") && own) {
                        applyPlan(p["plan"]);
                        continue;
                    }
                }
                if (own && (!r.peripherals || !r.peripherals->contains("ui"))) {
                    if (operation_ && (std::holds_alternative<SlotSave>(*operation_) ||
                                       std::holds_alternative<SlotLoad>(*operation_))) {
                        if (r.code || r.partial)
                            configurationWarning_ = r.text;
                        else
                            configurationWarning_.clear();
                    }
                    if (!r.code && operation_ && std::holds_alternative<ProtectionApply>(*operation_))
                        eraseEdits(protectionId, showWindowId);
                    if (!r.code && operation_ &&
                        (std::holds_alternative<ThermalReset>(*operation_) ||
                         std::holds_alternative<SlotLoad>(*operation_) ||
                         std::holds_alternative<DefaultsApply>(*operation_))) {
                        edited_.clear();
                        boundPages_[1] = false;
                        boundCharts_.clear();
                    }
                    setStatus(result.text + (r.slot_committed ? L" [slot committed]" : L"") +
                              (r.partial ? L" [partial]" : L"") + (r.applied ? L" [applied]" : L"") +
                              (r.saved ? L" [saved]" : L""));
                    if (r.code) {
                        pendingCpu_.reset();
                        MessageBoxW(window_, result.text.c_str(), L"MechControl — Not completed", MB_OK | MB_ICONERROR);
                    }
                    hints();
                }
            } else if (result.kind != ResultKind::sample && !result.text.empty())
                setStatus(result.text);
            if (result.sensors && IsWindowVisible(window_)) sensors(*result.sensors);
        }
    }
    void selectModel(const std::shared_ptr<ModelSelection>& selection) {
        struct Template {
            DLGTEMPLATE dialog;
            WORD menu = 0, type = 0, title = 0;
        } layout{};
        layout.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
        layout.dialog.cx = 510;
        layout.dialog.cy = 310;
        ModelDialog data{selection->paths, font_};
        auto result = DialogBoxIndirectParamW(instance_, &layout.dialog, window_, modelProcedure,
                                              reinterpret_cast<LPARAM>(&data));
        if (result == IDOK && data.chosen >= 0) selection->chosen = selection->paths[size_t(data.chosen)];
        if (result == -1) log_.write("Manual model selection dialog failed: " + std::to_string(GetLastError()));
        SetEvent(selection->completed.get());
        if (!selection->chosen) {
            log_.write("Manual model selection canceled or failed");
            DestroyWindow(window_);
        }
    }
    void checkOem() {
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshot) return;
        PROCESSENTRY32W p{sizeof p};
        std::wstring conflicts;
        if (Process32FirstW(snapshot.get(), &p)) do {
                if (_wcsicmp(p.szExeFile, L"GCUService.exe") == 0 || _wcsicmp(p.szExeFile, L"GCUBridge.exe") == 0)
                    conflicts += std::wstring(p.szExeFile) + L" (PID " + std::to_wstring(p.th32ProcessID) + L")\n";
            } while (Process32NextW(snapshot.get(), &p));
        if (!conflicts.empty()) {
            log_.write("OEM conflicts: " + utf8(conflicts));
            MessageBoxW(window_,
                        (L"The following OEM processes are running and may overwrite your settings:\n\n" + conflicts +
                         L"\nConsider stopping them before using MechControl.")
                            .c_str(),
                        L"MechControl — OEM Conflict Warning", MB_OK | MB_ICONWARNING);
        }
    }
    void createIcons() {
        for (int i = 0; i < 5; ++i) {
            HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
            HBITMAP bitmap = CreateCompatibleBitmap(screen, 32, 32), mask = CreateBitmap(32, 32, 1, 1, nullptr);
            auto old = SelectObject(dc, bitmap);
            RECT rect{0, 0, 32, 32};
            HBRUSH brush = CreateSolidBrush(RGB(30 + i * 35, 100, 190 - i * 25));
            FillRect(dc, &rect, brush);
            DeleteObject(brush);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            auto digit = std::to_wstring(i + 1);
            DrawTextW(dc, digit.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            ICONINFO info{TRUE, 0, 0, mask, bitmap};
            slotIcons_[i] = CreateIconIndirect(&info);
            SelectObject(dc, old);
            DeleteObject(bitmap);
            DeleteObject(mask);
            DeleteDC(dc);
            ReleaseDC(nullptr, screen);
        }
        for (int i = 0; i < 40; ++i) {
            auto resource = FindResourceW(instance_, MAKEINTRESOURCEW(1000 + i), RT_RCDATA);
            if (!resource) continue;
            auto data = LoadResource(instance_, resource);
            auto bytes = static_cast<PBYTE>(LockResource(data));
            auto icon = CreateIconFromResourceEx(bytes, SizeofResource(instance_, resource), TRUE, 0x30000, 32, 32,
                                                 LR_DEFAULTCOLOR);
            if (icon) animation_.push_back(icon);
        }
    }
    void tray(bool add = false) {
        if (closing_) return;
        NOTIFYICONDATAW icon{sizeof icon};
        icon.hWnd = window_;
        icon.uID = 1;
        icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        icon.uCallbackMessage = trayMessage;
        auto tip = tripped_            ? L"MechControl — THERMAL PROTECTION HOLD"
                   : !warning_.empty() ? L"MechControl — CONFIGURATION WARNING"
                                       : L"MechControl";
        wcscpy_s(icon.szTip, tip);
        icon.hIcon = LoadIconW(nullptr, tripped_ ? IDI_ERROR : !warning_.empty() ? IDI_WARNING : IDI_APPLICATION);
        if (!tripped_ && warning_.empty() && target_ >= 1 && target_ <= 5)
            icon.hIcon = target_ == 5 && !animation_.empty() ? animation_[animationFrame_] : slotIcons_[target_ - 1];
        const bool wasAdded = trayAdded_, adding = add || !wasAdded;
        trayAdded_ = Shell_NotifyIconW(adding ? NIM_ADD : NIM_MODIFY, &icon) != FALSE;
        if (trayAdded_) {
            if (adding) {
                icon.uVersion = NOTIFYICON_VERSION_4;
                Shell_NotifyIconW(NIM_SETVERSION, &icon);
                log_.write("Tray icon registration succeeded");
            }
        } else if (add || wasAdded)
            log_.write("Tray icon unavailable; waiting for Explorer's TaskbarCreated notification");
    }
    void updateTray() {
        int state = tripped_ ? -2 : !warning_.empty() ? -1 : target_;
        if (state == trayState_) return;
        trayState_ = state;
        KillTimer(window_, 1);
        animationFrame_ = 0;
        if (state == 5 && animation_.size() > 1) SetTimer(window_, 1, 67, nullptr);
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<LPWSTR>(tripped_            ? L"Thermal Failsafe (!)"
                                          : !warning_.empty() ? L"Thermal Failsafe (configuration !)"
                                                              : L"Thermal Failsafe");
        TabCtrl_SetItem(tabs_, 2, &item);
        if (trayAdded_) tray();
    }
    void balloon(const std::wstring& title, const std::wstring& text, DWORD flags) {
        NOTIFYICONDATAW icon{sizeof icon};
        icon.hWnd = window_;
        icon.uID = 1;
        icon.uFlags = NIF_INFO;
        icon.dwInfoFlags = flags;
        wcsncpy_s(icon.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(icon.szInfo, text.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &icon);
    }
    void alert() {
        balloon(L"MechControl Thermal Failsafe", w(state_.value("thermalStatus", std::string{})), NIIF_ERROR);
        MessageBeep(MB_ICONHAND);
        if (state_.at("protection").get<ProtectionSettings>().show_window) {
            show();
            TabCtrl_SetCurSel(tabs_, 2);
            selectPage();
        }
        if (alert_) return;
        alert_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"STATIC", L"MechControl — Thermal Failsafe",
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                 scale(560), scale(200), window_, nullptr, instance_, nullptr);
        SetWindowSubclass(alert_, alertProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
        alertText_ = CreateWindowW(L"STATIC", L"Check the cooler. Closing this alert does NOT reset the failsafe.",
                                   WS_CHILD | WS_VISIBLE, scale(12), scale(12), scale(520), scale(100), alert_, nullptr,
                                   instance_, nullptr);
        auto open =
            CreateWindowW(L"BUTTON", L"Open Failsafe Settings", WS_CHILD | WS_VISIBLE | WS_TABSTOP, scale(12),
                          scale(118), scale(520), scale(30), alert_, reinterpret_cast<HMENU>(1), instance_, nullptr);
        for (auto child : {alertText_, open}) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    // Waits briefly so the EC bypass is cleared before Windows sleeps, turns the display off or ends the session.
    void powerTransition(PowerTransition::Kind kind) {
        if (worker_ && ready_ && !closing_) worker_->request(PowerTransition{kind}).wait_for(std::chrono::seconds(2));
    }
    void show() {
        ShowWindow(window_, SW_RESTORE);
        SetForegroundWindow(window_);
        if (worker_) {
            worker_->request(Visibility{true});
            worker_->request(UiPage{TabCtrl_GetCurSel(tabs_)});
        }
    }
    void menu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, showId, L"Show");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        for (int i = 0; i < 5; ++i) {
            auto name = std::to_wstring(i + 1) + L": " + text(get(slotId + i)) + (emptySlot(i) ? L" (empty)" : L"");
            AppendMenuW(menu, MF_STRING | (target_ == i + 1 ? MF_CHECKED : 0), slotId + i, name.c_str());
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, exitId, L"Exit");
        POINT p{};
        GetCursorPos(&p);
        SetForegroundWindow(window_);
        auto id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, window_, nullptr);
        DestroyMenu(menu);
        PostMessageW(window_, WM_NULL, 0, 0);
        if (id) command(id, 0);
    }
    void shutdown() {
        if (closing_) return;
        powerTransition(PowerTransition::shutdown);
        closing_ = true;
        delivery_->close();
        for (auto notify : powerNotifications_)
            if (notify) UnregisterPowerSettingNotification(notify);
#ifdef MECHCONTROL_WITH_CLI
        cli_.reset();
#endif
        if (worker_) worker_->stop();
        if (trayAdded_) {
            NOTIFYICONDATAW icon{sizeof icon};
            icon.hWnd = window_;
            icon.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &icon);
            trayAdded_ = false;
        }
        log_.write("Orderly shutdown completed");
    }
    LRESULT message(UINT message, WPARAM wp, LPARAM lp) {
        if (taskbarMessage_ && message == taskbarMessage_) {
            tray(true);
            return 0;
        }
        switch (message) {
        case WM_CREATE: initialize(); return 0;
        case WM_TIMER:
            if (wp == 1) {
                if (trayState_ != 5 || ++animationFrame_ >= animation_.size()) {
                    KillTimer(window_, 1);
                    animationFrame_ = animation_.empty() ? 0 : animation_.size() - 1;
                } else
                    tray();
            }
            return 0;
        case WM_SHOWWINDOW:
            if (worker_) worker_->request(Visibility{wp != 0});
            if (wp && ready_) bindPage(TabCtrl_GetCurSel(tabs_));
            break;
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED)
                ShowWindow(window_, SW_HIDE);
            else
                layout();
            return 0;
        // The window paints only the monitor bar; its children cover the rest.
        case WM_ERASEBKGND: {
            RECT r{};
            GetClientRect(window_, &r);
            r.bottom = bar_.top;
            FillRect(reinterpret_cast<HDC>(wp), &r, GetSysColorBrush(COLOR_BTNFACE));
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            auto dc = BeginPaint(window_, &ps);
            paintBar(dc);
            EndPaint(window_, &ps);
            return 0;
        }
        case WM_DRAWITEM: drawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE;
        case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {scale(760), scale(480)}; return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            auto r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(window_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            applyDpi();
            return 0;
        }
        case WM_NOTIFY: {
            auto n = reinterpret_cast<NMHDR*>(lp);
            if (n->idFrom == tabsId && n->code == TCN_SELCHANGE) selectPage();
            if (n->idFrom == detailsId && (n->code == NM_CLICK || n->code == NM_RETURN)) {
                thermalDetails_ = !thermalDetails_;
                layoutPage(pages_[2]);
            }
            if (auto note = support_.find(int(n->idFrom)); note != support_.end()) {
                if (n->code == NM_CLICK || n->code == NM_RETURN) openUnelevated(issueUrl(note->second));
                // A support note takes the color of the painted note it stands in for; its link keeps the link color.
                if (n->code == NM_CUSTOMDRAW) {
                    auto draw = reinterpret_cast<NMCUSTOMDRAW*>(lp);
                    if (draw->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                    if (draw->dwDrawStage == CDDS_ITEMPREPAINT && draw->dwItemSpec == UINT_PTR(-1))
                        SetTextColor(draw->hdc, note->second.color);
                    return CDRF_DODEFAULT;
                }
            }
            if (n->idFrom == advancedId && (n->code == NM_CLICK || n->code == NM_RETURN)) {
                advanced_ = !advanced_;
                SetWindowTextW(get(advancedId),
                               advanced_ ? L"<a>Hide advanced controls</a>" : L"<a>Show advanced controls</a>");
                refreshPower();
            }
            return 0;
        }
        case WM_MOUSEWHEEL: scroll(pages_[TabCtrl_GetCurSel(tabs_)].window, message, wp); return 0;
        case WM_HSCROLL:
            if (!updating_ && lp) {
                int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
                if (id >= 200 && id < 1000) {
                    updating_ = true;
                    set(id + 1000, display(id, int(SendMessageW(reinterpret_cast<HWND>(lp), TBM_GETPOS, 0, 0))));
                    updating_ = false;
                    edited_.insert(id);
                    hints();
                }
            }
            return 0;
        case WM_COMMAND: command(LOWORD(wp), HIWORD(wp)); return 0;
        case WM_CONTEXTMENU: {
            int id = GetDlgCtrlID(reinterpret_cast<HWND>(wp));
            if (id >= slotId && id < slotId + 5) {
                POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (p.x == -1) {
                    RECT r{};
                    GetWindowRect(get(id), &r);
                    p = {r.left, r.bottom};
                }
                slotMenu(id - slotId + 1, p);
            }
            return 0;
        }
        case resultMessage: results(); return 0;
        case fanEditMessage:
            fanEditQueued_ = false;
            if (worker_ && state_.value("linked", false)) {
                auto intent = fanIntent();
                worker_->request(FanEdit{intent});
                state_["fan"] = intent;
                hints();
            }
            return 0;
        case trayMessage:
            if (LOWORD(lp) == WM_CONTEXTMENU)
                menu();
            else if (LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT)
                show();
            return 0;
        case WM_POWERBROADCAST:
            if (worker_ && wp == PBT_APMPOWERSTATUSCHANGE) {
                SYSTEM_POWER_STATUS s{};
                if (GetSystemPowerStatus(&s) && s.ACLineStatus != 255) worker_->request(AcEvent{s.ACLineStatus == 1});
            }
            if (worker_ && wp == PBT_APMRESUMEAUTOMATIC) worker_->request(Resume{});
            if (wp == PBT_APMSUSPEND) powerTransition(PowerTransition::suspend);
            if (worker_ && wp == PBT_POWERSETTINGCHANGE) {
                auto s = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
                if (s->DataLength < sizeof(DWORD)) return TRUE;
                DWORD value = *reinterpret_cast<const DWORD*>(s->Data);
                // Modern Standby turns the display off before it sleeps: the earliest reliable warning.
                if (s->PowerSetting == displayState && value == 0) powerTransition(PowerTransition::display_off);
                if (s->PowerSetting == powerSource) worker_->request(AcEvent{value == 0});
            }
            return TRUE;
        case WM_CLOSE: ShowWindow(window_, SW_HIDE); return 0;
        case WM_QUERYENDSESSION: powerTransition(PowerTransition::shutdown); return TRUE;
        case WM_ENDSESSION:
            if (wp) DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            shutdown();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window_, message, wp, lp);
    }
    HINSTANCE instance_;
    HWND window_ = nullptr, tabs_ = nullptr, progress_ = nullptr, alert_ = nullptr, alertText_ = nullptr;
    HWND tooltip_ = nullptr, hot_ = nullptr;
    RECT bar_{}, linkedBar_{}, thermalLive_{};
    std::vector<Text> barText_;
    std::wstring status_, fanValidation_, fanKey_;
    std::array<std::wstring, 7> thermalText_;
    bool thermalDetails_ = false;
    std::array<std::wstring, 6> monitor_{L"CPU Fan: --", L"GPU Fan: --",   L"CPU Temp Limit: --",
                                         L"Battery: --", L"Fan Boost: --", L"Fan Curve: --"};
    HFONT font_ = nullptr, bold_ = nullptr, title_ = nullptr, header_ = nullptr, heading_ = nullptr;
    UINT dpi_ = 96, taskbarMessage_ = 0;
    RECT liveTable_{0, -1, 0, -1}, coolerSwatch_{};
    std::array<RECT, 3> swatches_{};
    std::wstring coolerStatus_ = L"Disconnected", coolerFirmware_;
    std::array<RECT, 8> infoCells_{};
    std::array<std::wstring, 8> infoText_;
    std::map<int, int> detectedMa_;
    std::wstring cpuNote_, cpuPreset_, powerHint_, liveReason_, powerKey_, dlppcTip_, bypassText_;
    COLORREF bypassColor_ = 0;
    bool advanced_ = false;
    std::map<int, Support> support_;
    std::array<HPOWERNOTIFY, 2> powerNotifications_{};
    std::array<Page, 7> pages_;
    std::map<int, HWND> controls_;
    std::set<int> edited_;
    std::array<HICON, 5> slotIcons_{};
    std::vector<HICON> animation_;
    size_t animationFrame_ = 0;
    int trayState_ = -3, lostAlerts_ = 0;
    bool closing_ = false, trayAdded_ = false, updating_ = false, busy_ = false, ready_ = false, tripped_ = false,
         coolerConnected_ = false, coolerAvailable_ = false, fanEditQueued_ = false, powerRead_ = false;
    int target_ = 0;
    std::uint64_t sequence_ = 0, busySequence_ = 0;
    ULONGLONG lastError_ = 0;
    Json state_ = Json::object(), peripheralState_ = Json::object(), identity_, gpuTelemetry_, live_, gpuBudget_,
         devices_ = Json::array();
    std::map<int, std::pair<Json, ChartOptions>> boundCharts_;
    std::array<bool, 7> boundPages_{};
    std::string warning_, configurationWarning_;
    std::wstring gpuError_;
    std::optional<CpuSettings> pendingCpu_;
    std::optional<Sensors> lastSensors_;
    std::optional<ControlCommand> operation_;
    Log log_;
    std::shared_ptr<Delivery> delivery_;
    WorkerOptions workerOptions_;
    std::unique_ptr<ControlWorker> worker_;
#ifdef MECHCONTROL_WITH_CLI
    std::unique_ptr<CliServer> cli_;
#endif
};
#ifdef MECHCONTROL_PAGE_CHECK
#include "../tests/ui_pages_checks.h"
#endif
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        int argc = 0;
        auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) throw std::runtime_error("Cannot parse command line");
#ifdef MECHCONTROL_PAGE_CHECK
        if (argc != 3 || std::wstring_view(argv[1]) != L"--ui-check") {
            LocalFree(argv);
            return 2;
        }
        std::filesystem::path checks = argv[2];
        LocalFree(argv);
        auto fixture = std::make_shared<UiPageState>();
#else
        std::filesystem::path diagnostic;
        bool invalid = false;
        for (int i = 1; i < argc; ++i) {
            if (std::wstring_view(argv[i]) == L"--diagnose") {
                if (argc == 3 && i == 1 && argv[2][0] && !std::wstring_view(argv[2]).starts_with(L"--"))
                    diagnostic = argv[++i];
                else
                    invalid = true;
            } else
                invalid = true;
        }
        LocalFree(argv);
        if (invalid) return 2; // Unknown/invalid diagnostics can never start controlling hardware.
        if (!diagnostic.empty()) {
            Log log(executable_directory() / L"mechcontrol.log");
            ControlWorker worker(std::make_shared<Delivery>(), log, {.once = true, .report = diagnostic});
            return worker.wait();
        }
        Handle singleton(CreateMutexW(nullptr, TRUE, L"Global\\MechControl"));
        if (!singleton) throw std::runtime_error("Cannot acquire Global\\MechControl");
        if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
#endif
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        struct Apartment {
            ~Apartment() { winrt::uninit_apartment(); }
        } apartment;
        INITCOMMONCONTROLSEX cc{sizeof cc, ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_LINK_CLASS |
                                               ICC_UPDOWN_CLASS};
        if (!InitCommonControlsEx(&cc)) throw std::runtime_error("Cannot initialize native controls");
        registerFanChart(instance);
        WNDCLASSW type{};
        type.hInstance = instance;
        type.lpfnWndProc = Application::procedure;
        type.lpszClassName = mainClass;
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        type.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        if (!RegisterClassW(&type)) throw std::runtime_error("Cannot register main window");
#ifdef MECHCONTROL_PAGE_CHECK
        Application app(instance, ui_page_options(checks, fixture));
#else
        Application app(instance);
#endif
        auto window = app.create();
        if (!window) throw std::runtime_error("Cannot create main window");
#ifdef MECHCONTROL_PAGE_CHECK
        app.checkPages(checks, fixture);
        return 0;
#else
        MSG message{};
        int result;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0)
            if (!IsDialogMessageW(window, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        return result == -1 ? 1 : int(message.wParam);
#endif
    } catch (const winrt::hresult_error& e) {
        MessageBoxW(nullptr, e.message().c_str(), L"MechControl", MB_OK | MB_ICONERROR);
        return 1;
    } catch (const std::exception& e) {
        MessageBoxW(nullptr, w(e.what()).c_str(), L"MechControl", MB_OK | MB_ICONERROR);
        return 1;
    }
}
