#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <functional>
#include <optional>
#include <string>

namespace mech {
// Holds the NVIDIA GPU awake while the screen is off and the PC is awake; see gpu_keepawake.cpp. The UI thread
// owns it: it forwards the window's power, display and timer messages.
class GpuKeepAwake {
public:
    static constexpr UINT_PTR tickTimer = 2, releaseTimer = 3;
    static constexpr UINT sleepMessage = WM_APP + 5; // Windows logged that it is going to sleep.
    GpuKeepAwake(HWND window, std::function<void(const std::string&)> log);
    ~GpuKeepAwake();
    GpuKeepAwake(const GpuKeepAwake&) = delete;
    GpuKeepAwake& operator=(const GpuKeepAwake&) = delete;
    void enable(bool);
    void display(DWORD state); // GUID_CONSOLE_DISPLAY_STATE: 0 off, 1 on, 2 dimmed.
    void suspend(bool asleep); // PBT_APMSUSPEND, then false on resume.
    void goingToSleep();       // sleepMessage.
    void topology();           // WM_DISPLAYCHANGE.
    void timer(UINT_PTR id);

private:
    struct Panel {
        LUID adapter;
        std::wstring instance; // The adapter's PnP device instance, for its power state.
    };
    static std::optional<Panel> nvidiaPanel();
    void watch();
    void update();
    void stop(const std::string& why);
    void tick();
    bool create();
    void release();
    HWND window_;
    std::function<void(const std::string&)> log_;
    std::optional<Panel> panel_;
    void* subscription_ = nullptr; // EVT_HANDLE of the Kernel-Power sleep event.
    bool enabled_ = false, off_ = false, seenOn_ = false, asleep_ = false, active_ = false, failed_ = false;
    ULONGLONG last_ = 0;
    unsigned ticks_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> target_, staging_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view_;
};
}
