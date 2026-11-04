#include "ble.h"
#include "worker.h"
#include "fan_chart.h"
#include "win_handle.h"
#ifdef MECHCONTROL_WITH_CLI
#include "cli_server.h"
#endif
#include <commctrl.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace {
constexpr UINT trayMessage = WM_APP + 2, checkMessage = WM_APP + 3;
constexpr int tabId = 100, sliderId = 101, commandId = 102, discoverId = 103, cancelId = 104;
constexpr int showId = 200, exitId = 201;
constexpr wchar_t windowClass[] = L"MechControl.Native.Foundation";
#ifdef MECHCONTROL_UI_FIXTURE
constexpr wchar_t logFilename[] = L"ui-fixture.log";
#else
constexpr wchar_t logFilename[] = L"mechcontrol-native-prototype.log";
#endif

class Application {
public:
    Application(HINSTANCE instance, const std::filesystem::path& directory, std::filesystem::path checks)
        : instance_(instance), log_(directory / logFilename),
          delivery_(std::make_shared<mech::Delivery>()),
#ifdef MECHCONTROL_UI_FIXTURE
          ble_(delivery_),
#endif
          checks_(std::move(checks)) {}
    ~Application() { shutdown(); if (font_) DeleteObject(font_); }
    HWND create() {
        return CreateWindowExW(WS_EX_CONTROLPARENT, windowClass, L"MechControl Native - foundation prototype",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 560, nullptr, nullptr, instance_, this);
    }
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (!app) return DefWindowProcW(window, message, wparam, lparam);
        // No C++ exception may cross the Win32 callback boundary.
        try { return app->message(message, wparam, lparam); }
        catch (const std::exception& error) {
            app->log_.write(error.what());
            app->shutdown();
            DestroyWindow(window);
            PostQuitMessage(1);
            return 0;
        }
    }
private:
    HWND control(const wchar_t* type, const wchar_t* text, DWORD style, int id, HWND parent) {
        auto window = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (!window) throw std::runtime_error("Cannot create a native control.");
        return window;
    }
    int scale(int value) const { return MulDiv(value, dpi_, 96); }
    void initialize() {
        dpi_ = GetDpiForWindow(window_);
        tabs_ = control(WC_TABCONTROLW, L"", WS_TABSTOP, tabId, window_);
        for (auto name : {L"Power", L"Fans", L"Cooler"}) {
            TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = const_cast<LPWSTR>(name);
            TabCtrl_InsertItem(tabs_, TabCtrl_GetItemCount(tabs_), &item);
        }
        page_ = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        title_ = control(L"STATIC", L"Prototype only. No EC, GPU, lighting or cooler settings are changed.", 0, 0, page_);
        label_ = control(L"STATIC", L"Synthetic control value", 0, 0, page_);
        slider_ = control(TRACKBAR_CLASSW, L"Synthetic control value", WS_TABSTOP | TBS_AUTOTICKS, sliderId, page_);
        SendMessageW(slider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(slider_, TBM_SETPOS, TRUE, 50);
        command_ = control(L"BUTTON", L"&Queue prototype command", WS_TABSTOP | BS_PUSHBUTTON, commandId, window_);
        chart_ = control(mech::chartClass, L"Fan curve prototype", WS_TABSTOP, 105, page_);
        mech::ChartOptions chartOptions{false,true};
        SendMessageW(chart_,mech::chartOptionsMessage,0,reinterpret_cast<LPARAM>(&chartOptions));
        mech::config::Curve fixtureCurve{{30,45,60,75,90},{},{40,70,110,150,200}};
        SendMessageW(chart_,mech::chartSetMessage,0,reinterpret_cast<LPARAM>(&fixtureCurve));
#ifdef MECHCONTROL_UI_FIXTURE
        discover_ = control(L"BUTTON", L"&Discover BLE adapter", WS_TABSTOP | BS_PUSHBUTTON, discoverId, window_);
        cancel_ = control(L"BUTTON", L"&Cancel discovery", WS_TABSTOP | BS_PUSHBUTTON, cancelId, window_);
        bleText_ = control(L"STATIC", L"Read-only adapter/radio discovery; no cooler connection.", 0, 0, page_);
#else
        bleText_ = control(L"STATIC", L"Cooler controller status will appear here. Control pages are not yet integrated.", 0, 0, page_);
#endif
        status_ = control(L"STATIC", L"Starting serialized worker...", 0, 0, window_);
        applyDpi();
        selectPage();
        delivery_->attach(window_);
        mech::WorkerOptions options;
#ifdef MECHCONTROL_UI_FIXTURE
        options.fixture=true;
#else
        SetWindowTextW(title_,L"Core EC controller active. Use the optional CLI for live power and fan boost commands. Full UI is not yet integrated.");
        EnableWindow(command_,FALSE);
#endif
        worker_ = std::make_unique<mech::ControlWorker>(delivery_, log_,std::move(options));
#ifdef MECHCONTROL_WITH_CLI
        cli_=std::make_unique<mech::CliServer>(*worker_,log_);
#endif
        taskbarMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
        tray(true);
        log_.write("UI ready; DPI=" + std::to_string(dpi_));
        if (!checks_.empty()) PostMessageW(window_, checkMessage, 0, 0);
    }
    void applyDpi() {
        auto font = CreateFontW(-scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        for (auto window : {tabs_, title_, label_, slider_, command_, discover_, cancel_, bleText_, status_})
            SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        if (font_) DeleteObject(font_);
        font_ = font;
        SendMessageW(chart_, mech::chartDpiMessage, dpi_, 0);
        layout();
    }
    void place(HWND window, int x, int y, int width, int height) {
        MoveWindow(window, x, y, std::max(0, width), std::max(0, height), TRUE);
    }
    void layout() {
        if (!tabs_) return;
        RECT client; GetClientRect(window_, &client);
        int m = scale(12), width = client.right - 2 * m;
        place(tabs_, m, m, width, client.bottom - scale(70));
        RECT content; GetClientRect(tabs_, &content); TabCtrl_AdjustRect(tabs_, FALSE, &content);
        MapWindowPoints(tabs_, window_, reinterpret_cast<POINT*>(&content), 2);
        place(page_, content.left, content.top, content.right - content.left, content.bottom - content.top);
        RECT page; GetClientRect(page_, &page);
        place(title_, m, m, page.right - 2 * m, scale(40));
        place(label_, m, scale(68), page.right - 2 * m, scale(24));
        place(slider_, m, scale(96), page.right - 2 * m, scale(44));
        place(chart_, m, scale(64), page.right - 2 * m, page.bottom - scale(76));
        place(bleText_, m, scale(68), page.right - 2 * m, scale(80));
        place(command_, m, client.bottom - scale(54), scale(230), scale(30));
        place(discover_, m, client.bottom - scale(54), scale(200), scale(30));
        place(cancel_, m + scale(212), client.bottom - scale(54), scale(160), scale(30));
        place(status_, m, client.bottom - scale(22), width, scale(20));
    }
    void selectPage() {
        int selected = TabCtrl_GetCurSel(tabs_);
        ShowWindow(label_, selected == 0 ? SW_SHOW : SW_HIDE);
        ShowWindow(slider_, selected == 0 ? SW_SHOW : SW_HIDE);
        ShowWindow(command_, selected == 0 ? SW_SHOW : SW_HIDE);
        ShowWindow(chart_, selected == 1 ? SW_SHOW : SW_HIDE);
        ShowWindow(bleText_, selected == 2 ? SW_SHOW : SW_HIDE);
        ShowWindow(discover_, selected == 2 ? SW_SHOW : SW_HIDE);
        ShowWindow(cancel_, selected == 2 ? SW_SHOW : SW_HIDE);
    }
    void tray(bool add) {
        NOTIFYICONDATAW icon{sizeof icon}; icon.hWnd = window_; icon.uID = 1;
        icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        icon.uCallbackMessage = trayMessage; icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
#ifdef MECHCONTROL_UI_FIXTURE
        wcscpy_s(icon.szTip, L"MechControl Native UI fixture (no hardware control)");
#else
        wcscpy_s(icon.szTip, L"MechControl Native core controller (development UI)");
#endif
        if (add) {
            trayAdded_ = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
            icon.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &icon);
        } else { Shell_NotifyIconW(NIM_DELETE, &icon); trayAdded_ = false; }
    }
    void show() { ShowWindow(window_, SW_RESTORE); SetForegroundWindow(window_); }
    void menu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, showId, L"&Show prototype");
        AppendMenuW(menu, MF_STRING, exitId, L"E&xit");
        POINT p; GetCursorPos(&p); SetForegroundWindow(window_);
        int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, window_, nullptr);
        DestroyMenu(menu); PostMessageW(window_, WM_NULL, 0, 0);
        if (command) SendMessageW(window_, WM_COMMAND, command, 0);
    }
    void shutdown() {
        if (closing_) return;
        closing_ = true;
        delivery_->close(); // Reject late BLE/worker delivery before the HWND can be reused.
#ifdef MECHCONTROL_UI_FIXTURE
        ble_.cancel();
#endif
#ifdef MECHCONTROL_WITH_CLI
        cli_.reset();
#endif
        if (worker_) worker_->stop();
        if (trayAdded_) tray(false);
        log_.write("Orderly shutdown completed");
    }
#ifdef MECHCONTROL_UI_FIXTURE
    void uiCheck();
#endif
    LRESULT message(UINT message, WPARAM wparam, LPARAM lparam) {
        if (taskbarMessage_ && message == taskbarMessage_) { tray(true); return 0; }
        switch (message) {
        case WM_CREATE: initialize(); return 0;
        case WM_SHOWWINDOW:
            if(worker_) worker_->request(mech::Visibility{wparam!=0});
            break;
        case WM_POWERBROADCAST:
            if(worker_ && wparam==PBT_APMPOWERSTATUSCHANGE) {
                SYSTEM_POWER_STATUS power{};if(GetSystemPowerStatus(&power) && power.ACLineStatus!=255) worker_->request(mech::AcEvent{power.ACLineStatus==1});
            }
            if(worker_ && (wparam==PBT_APMSUSPEND || wparam==PBT_APMRESUMEAUTOMATIC))
                worker_->request(mech::PowerEvent{wparam==PBT_APMSUSPEND});
            return TRUE;
        case WM_SIZE: if (wparam == SIZE_MINIMIZED) ShowWindow(window_, SW_HIDE); else layout(); return 0;
        case WM_GETMINMAXINFO: {
            auto size = reinterpret_cast<MINMAXINFO*>(lparam);
            size->ptMinTrackSize = {scale(620), scale(440)}; return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wparam); auto rect = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left,
                rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            applyDpi(); return 0;
        }
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR*>(lparam)->idFrom == tabId && reinterpret_cast<NMHDR*>(lparam)->code == TCN_SELCHANGE) selectPage();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
            case commandId: worker_->submit({++commandSequence_, static_cast<int>(SendMessageW(slider_, TBM_GETPOS, 0, 0))}); return 0;
#ifdef MECHCONTROL_UI_FIXTURE
            case discoverId: ble_.start(); SetWindowTextW(bleText_, L"Discovering adapter asynchronously..."); return 0;
            case cancelId: ble_.cancel(); SetWindowTextW(bleText_, L"Discovery canceled"); return 0;
#endif
            case showId: show(); return 0;
            case exitId: shutdown(); DestroyWindow(window_); return 0;
            }
            break;
        case mech::resultMessage:
            for (const auto& result : delivery_->take()) {
                if (result.kind == mech::ResultKind::failure) {
                    shutdown(); DestroyWindow(window_); PostQuitMessage(1); return 0;
                }
                else if (result.kind == mech::ResultKind::ble) SetWindowTextW(bleText_, result.text.c_str());
                else if (result.kind == mech::ResultKind::command) SetWindowTextW(status_, result.text.c_str());
                else if (IsWindowVisible(window_)) {
                    SetWindowTextW(status_, result.text.c_str());
                }
            }
            return 0;
        case trayMessage:
            if (LOWORD(lparam) == WM_CONTEXTMENU) menu();
            else if (LOWORD(lparam) == NIN_SELECT || LOWORD(lparam) == NIN_KEYSELECT) show();
            return 0;
#ifdef MECHCONTROL_UI_FIXTURE
        case checkMessage: uiCheck(); return 0;
#endif
        case WM_CLOSE: ShowWindow(window_, SW_HIDE); return 0;
        case WM_QUERYENDSESSION: return TRUE;
        case WM_ENDSESSION: if (wparam) DestroyWindow(window_); return 0;
        case WM_DESTROY: shutdown(); PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }
    HINSTANCE instance_;
    HWND window_ = nullptr, tabs_ = nullptr, page_ = nullptr, title_ = nullptr, label_ = nullptr;
    HWND slider_ = nullptr, command_ = nullptr, chart_ = nullptr, discover_ = nullptr, cancel_ = nullptr;
    HWND bleText_ = nullptr, status_ = nullptr;
    HFONT font_ = nullptr;
    UINT dpi_ = 96, taskbarMessage_ = 0;
    bool trayAdded_ = false, closing_ = false;
    std::uint64_t commandSequence_ = 0;
    mech::Log log_;
    std::shared_ptr<mech::Delivery> delivery_;
#ifdef MECHCONTROL_UI_FIXTURE
    mech::BleDiscovery ble_;
#endif
    std::unique_ptr<mech::ControlWorker> worker_;
#ifdef MECHCONTROL_WITH_CLI
    std::unique_ptr<mech::CliServer> cli_;
#endif
    std::filesystem::path checks_;
};

#ifdef MECHCONTROL_UI_FIXTURE
void Application::uiCheck() {
    std::ofstream report(checks_ / L"ui-check.txt");
    if (!report) throw std::runtime_error("Cannot open UI check report.");
    auto require = [&](bool condition, const char* name) {
        report << (condition ? "PASS " : "FAIL ") << name << '\n'; report.flush();
        if (!condition) throw std::runtime_error(name);
    };
    show();
    for (UINT dpi : {96u, 144u, 192u}) {
        RECT rect{40, 40, 40 + MulDiv(800, dpi, 96), 40 + MulDiv(560, dpi, 96)};
        SendMessageW(window_, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&rect));
        for (int page = 0; page < 3; ++page) {
            TabCtrl_SetCurSel(tabs_, page); selectPage();
            RECT parent; GetClientRect(page_, &parent);
            for (auto control : {title_, label_, slider_, chart_, bleText_}) {
                if (!IsWindowVisible(control)) continue;
                RECT child; GetWindowRect(control, &child); MapWindowPoints(nullptr, page_, reinterpret_cast<POINT*>(&child), 2);
                require(child.left >= 0 && child.top >= 0 && child.right <= parent.right && child.bottom <= parent.bottom,
                    "visible page control fits at scaled DPI");
            }
            RedrawWindow(window_, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_INVALIDATE);
            // Capture each actual rendered child tree with PrintWindow to a BMP evidence file.
            RECT client; GetWindowRect(window_, &client);
            int width = client.right - client.left, height = client.bottom - client.top;
            HDC screen = GetDC(window_), dc = CreateCompatibleDC(screen);
            BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
            void* pixels = nullptr;
            HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            auto old = SelectObject(dc, bitmap);
            BOOL printed = PrintWindow(window_, dc, 0);
            BITMAPFILEHEADER header{}; header.bfType = 0x4d42;
            header.bfOffBits = sizeof header + sizeof(BITMAPINFOHEADER);
            header.bfSize = header.bfOffBits + width * height * 4;
            std::ofstream image(checks_ / (L"ui-" + std::to_wstring(dpi) + L"-" + std::to_wstring(page) + L".bmp"), std::ios::binary);
            image.write(reinterpret_cast<char*>(&header), sizeof header);
            image.write(reinterpret_cast<char*>(&info.bmiHeader), sizeof(BITMAPINFOHEADER));
            image.write(static_cast<char*>(pixels), static_cast<std::streamsize>(width) * height * 4);
            SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(window_, screen);
            require(printed != FALSE && image.good(), "native rendering captured");
        }
        TabCtrl_SetCurSel(tabs_, 0); selectPage();
        SetFocus(slider_);
        auto before = SendMessageW(slider_, TBM_GETPOS, 0, 0);
        SendMessageW(slider_, WM_KEYDOWN, VK_RIGHT, 0);
        require(SendMessageW(slider_, TBM_GETPOS, 0, 0) == before + 1, "slider keyboard right");
        MSG tab{tabs_, WM_KEYDOWN, VK_TAB};
        SetFocus(tabs_); IsDialogMessageW(window_, &tab);
        require(GetFocus() == slider_, "Tab traverses tab/page to slider");
        TabCtrl_SetCurSel(tabs_, 1); selectPage(); SetFocus(chart_);
        auto point = SendMessageW(chart_, mech::chartPointMessage, 0, 0);
        SendMessageW(chart_, WM_KEYDOWN, VK_UP, 0);
        require(HIWORD(SendMessageW(chart_, mech::chartPointMessage, 0, 0)) == HIWORD(point) + 1, "fan point keyboard up");
        auto afterKeyboard = SendMessageW(chart_, mech::chartPointMessage, 0, 0);
        RECT plot; GetClientRect(chart_, &plot);plot.left+=scale(44);plot.right-=scale(14);plot.top+=scale(30);plot.bottom-=scale(32);
        int x = plot.left + MulDiv(LOWORD(point), plot.right - plot.left, 105);
        int y = plot.bottom - MulDiv(HIWORD(point) + 1, plot.bottom - plot.top, 100);
        SendMessageW(chart_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        require(GetCapture() == chart_, "fan point drag owns mouse capture");
        SendMessageW(chart_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + scale(8), y - scale(8)));
        SendMessageW(chart_, WM_LBUTTONUP, 0, MAKELPARAM(x + scale(8), y - scale(8)));
        require(SendMessageW(chart_, mech::chartPointMessage, 0, 0) != afterKeyboard, "fan point mouse drag");
        report << "DPI " << dpi << " checked (injected WM_DPICHANGED, not monitor switch)\n";
    }
    SendMessageW(window_, WM_CLOSE, 0, 0);
    require(!IsWindowVisible(window_), "close hides to tray");
    SendMessageW(window_, WM_COMMAND, showId, 0);
    require(IsWindowVisible(window_), "tray Show command restores");
    ShowWindow(window_, SW_MINIMIZE);
    require(!IsWindowVisible(window_), "minimize hides to tray");
    show();
    require(trayAdded_, "Shell tray icon registered");
    require(SendMessageW(window_, WM_QUERYENDSESSION, 0, ENDSESSION_LOGOFF) == TRUE, "session-end query accepted");
    SendMessageW(window_, WM_ENDSESSION, FALSE, ENDSESSION_LOGOFF);
    require(worker_->submit({999, 0, std::chrono::seconds(30)}), "canceled session end keeps worker available");
    ble_.start();
    report << "PASS exit with outstanding worker command and BLE discovery\n";
    report.close();
    SendMessageW(window_, WM_COMMAND, exitId, 0);
}
#endif
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        bool show = false;
        std::filesystem::path checkDirectory,diagnosticReport;
        int argc; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) throw std::runtime_error("Cannot parse command line.");
        bool invalidDiagnostic=false;
        for (int i = 1; i < argc; ++i) {
            if (std::wstring_view(argv[i]) == L"--show") show = true;
#ifdef MECHCONTROL_UI_FIXTURE
            else if (std::wstring_view(argv[i]) == L"--ui-check" && i + 1 < argc) checkDirectory = argv[++i];
#else
            else if (std::wstring_view(argv[i]) == L"--diagnose") {
                if(argc==3 && i==1 && argv[i+1][0]!=L'\0' && !std::wstring_view(argv[i+1]).starts_with(L"--")) diagnosticReport=argv[++i];
                else invalidDiagnostic=true;
            }
#endif
        }
        LocalFree(argv);
        if(invalidDiagnostic) return 2; // Never fall through to controlling startup on a malformed diagnostic.
        wchar_t executable[32768]; GetModuleFileNameW(nullptr, executable, 32768);
        if(!diagnosticReport.empty()) {
            mech::Log log(std::filesystem::path(executable).parent_path()/logFilename);
            mech::ControlWorker worker(std::make_shared<mech::Delivery>(),log,{.once=true,.report=diagnosticReport});
            return worker.wait(); // Read/SMAPC only. No controller mutex, UI, activation, UEFI, HID or NVIDIA.
        }
#ifdef MECHCONTROL_UI_FIXTURE
        constexpr auto mutexName=L"Local\\MechControl.Native.UiFixture";
#else
        constexpr auto mutexName=L"Global\\MechControl";
#endif
        mech::Handle singleton(CreateMutexW(nullptr, TRUE, mutexName));
        if (!singleton) return 1;
        if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
        INITCOMMONCONTROLSEX controls{sizeof controls, ICC_TAB_CLASSES | ICC_BAR_CLASSES};
        if (!InitCommonControlsEx(&controls)) throw std::runtime_error("Cannot initialize common controls.");
        mech::registerFanChart(instance);
        WNDCLASSW type{}; type.hInstance = instance; type.lpszClassName = windowClass;
        type.lpfnWndProc = Application::procedure; type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hIcon = LoadIconW(nullptr, IDI_APPLICATION); type.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        if (!RegisterClassW(&type)) throw std::runtime_error("Cannot register the main window.");
        Application app(instance, std::filesystem::path(executable).parent_path(), checkDirectory);
        auto window = app.create();
        if (!window) throw std::runtime_error("Cannot create the main window.");
        if (show) ShowWindow(window, SW_SHOW);
        MSG message{};
        int result;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        return result == -1 ? 1 : static_cast<int>(message.wParam);
    } catch (const winrt::hresult_error& error) {
        MessageBoxW(nullptr, error.message().c_str(), L"MechControl Native prototype", MB_OK | MB_ICONERROR);
        return 1;
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "MechControl Native prototype", MB_OK | MB_ICONERROR);
        return 1;
    }
}
