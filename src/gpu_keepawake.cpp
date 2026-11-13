#include "gpu_keepawake.h"
#include "nvidia.h"
#include <dxgi1_4.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <winevt.h>
#include <cstdio>
#include <vector>

// Works around an NVIDIA driver fault (seen on 610.62 to 621.24 with the NVIDIA GPU driving the panel): when the GPU
// is powered up shortly after Windows powered it down, the display may not come back (driver reset, black
// wallpaper, sometimes bugcheck 0x116). With the screen off, Windows powers the GPU down about 10 s after its last
// piece of work. This gives the GPU one trivial piece of Direct3D work every few seconds while the screen is off,
// so that power-down never happens. Ported from the tested standalone GpuKeepAwake. Its rules:
//   - It runs only when turned on, the NVIDIA GPU drives the internal panel (not hybrid mode), the screen is off and
//     the PC is awake. Otherwise it holds no Direct3D device and no timer.
//   - Screen off: one 1x1-pixel clear and read-back every tickMs, the first one tickMs after the screen-off notice,
//     never at once. After a sleep, the stale screen-off notice arrives at wake-up just before screen-on, and the
//     GPU must be left alone then.
//   - Sleep: Windows freezes desktop programs in sleep and sends no suspend broadcast for Modern Standby. As a second
//     safeguard it also stops on Kernel-Power event 566 with NextSessionType 2 ("going to sleep"; screen-off is
//     type 1) and on a suspend broadcast.
//   - It never wakes a GPU that is already powered down. It reads the GPU's power state from Windows' device records
//     before each tick. After a pause longer than staleMs (it was frozen, or the PC slept) or any sight of the GPU
//     outside D0, it stays quiet until the screen has been on and gone off again.
//   - Direct3D only. The fault is waking the GPU within a few seconds after it powered down, whatever does it; an
//     NVAPI query did it in a report of the same fault on another laptop, where a Direct3D wake was fine. NVAPI
//     polling while the GPU is held in D0 tested clean, and MechControl's own NVIDIA reads skip a GPU outside D0.
namespace mech {
namespace {
constexpr UINT tickMs = 3000;     // Windows' idle-to-power-down threshold is 10 s.
constexpr UINT staleMs = 8000;    // A longer gap between ticks means the app was frozen: the GPU may be down.
constexpr UINT releaseMs = 30000; // The device is dropped this long after the screen came back on.
std::string hex(HRESULT hr) {
    char text[16];
    snprintf(text, sizeof text, "0x%08lX", static_cast<unsigned long>(hr));
    return text;
}
DWORD WINAPI onSleep(EVT_SUBSCRIBE_NOTIFY_ACTION action, PVOID window, EVT_HANDLE) {
    if (action == EvtSubscribeActionDeliver) PostMessageW(static_cast<HWND>(window), GpuKeepAwake::sleepMessage, 0, 0);
    return 0;
}
}
GpuKeepAwake::GpuKeepAwake(HWND window, std::function<void(const std::string&)> log)
    : window_(window), log_(std::move(log)) {}
GpuKeepAwake::~GpuKeepAwake() {
    if (subscription_) EvtClose(subscription_);
    KillTimer(window_, tickTimer);
    KillTimer(window_, releaseTimer);
}
// The NVIDIA adapter when it drives the active internal panel. Reads Windows' display configuration only.
std::optional<GpuKeepAwake::Panel> GpuKeepAwake::nvidiaPanel() {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG result;
    do {
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return {};
        paths.resize(pathCount);
        modes.resize(modeCount);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        paths.resize(pathCount);
    } while (result == ERROR_INSUFFICIENT_BUFFER);
    if (result != ERROR_SUCCESS) return {};
    for (const auto& path : paths) {
        auto technology = path.targetInfo.outputTechnology;
        if (technology != DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL &&
            technology != DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED &&
            technology != DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED &&
            technology != DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS)
            continue;
        DISPLAYCONFIG_ADAPTER_NAME name{};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
        name.header.size = sizeof name;
        name.header.adapterId = path.targetInfo.adapterId;
        wchar_t instance[MAX_DEVICE_ID_LEN]{};
        ULONG size = sizeof instance;
        DEVPROPTYPE type = 0;
        if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS ||
            CM_Get_Device_Interface_PropertyW(name.adapterDevicePath, &DEVPKEY_Device_InstanceId, &type,
                                              reinterpret_cast<BYTE*>(instance), &size, 0) != CR_SUCCESS)
            return {};
        if (_wcsnicmp(instance, L"PCI\\VEN_10DE&", 13) != 0) return {}; // Hybrid: another GPU drives the panel.
        return Panel{path.targetInfo.adapterId, instance};
    }
    return {};
}
void GpuKeepAwake::enable(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    panel_ = on ? nvidiaPanel() : std::nullopt;
    log_(!on      ? "GPU keep-awake off"
         : panel_ ? "GPU keep-awake on; the NVIDIA GPU drives the internal panel"
                  : "GPU keep-awake on; idle, because the NVIDIA GPU does not drive the internal panel");
    watch();
    if (!on) release();
}
void GpuKeepAwake::display(DWORD state) {
    off_ = state == 0;
    if (!off_) {
        seenOn_ = true;
        asleep_ = false;
    }
    update();
}
void GpuKeepAwake::suspend(bool asleep) {
    asleep_ = asleep;
    if (!asleep) seenOn_ = !off_;
    update();
}
void GpuKeepAwake::goingToSleep() {
    if (!off_) return; // A stale notice delivered at wake-up.
    asleep_ = true;
    seenOn_ = false; // Cleared by the next screen-on.
    update();
}
void GpuKeepAwake::topology() {
    if (!enabled_) return;
    auto panel = nvidiaPanel();
    if (panel.has_value() != panel_.has_value())
        log_(panel ? "GPU keep-awake: the NVIDIA GPU now drives the internal panel"
                   : "GPU keep-awake: idle, because the NVIDIA GPU no longer drives the internal panel");
    panel_ = std::move(panel);
    watch();
}
// The subscription to the sleep event follows the panel check.
void GpuKeepAwake::watch() {
    if (panel_ && !subscription_) {
        subscription_ = EvtSubscribe(nullptr, nullptr, L"System",
                                     L"*[System[Provider[@Name='Microsoft-Windows-Kernel-Power'] and EventID=566]"
                                     L" and EventData[Data[@Name='NextSessionType']='2']]",
                                     nullptr, window_, onSleep, EvtSubscribeToFutureEvents);
        if (!subscription_)
            log_("GPU keep-awake: cannot watch for sleep (error " + std::to_string(GetLastError()) +
                 "); it relies on Windows freezing the app in sleep");
    } else if (!panel_ && subscription_) {
        EvtClose(subscription_);
        subscription_ = nullptr;
    }
    update();
}
void GpuKeepAwake::update() {
    bool want = enabled_ && panel_ && off_ && seenOn_ && !asleep_;
    if (want && !active_) {
        active_ = true;
        failed_ = false;
        ticks_ = 0;
        last_ = GetTickCount64();
        KillTimer(window_, releaseTimer);
        SetTimer(window_, tickTimer, tickMs, nullptr); // First tick in tickMs, not now.
        log_("GPU keep-awake: screen off, holding the GPU awake");
    } else if (!want && active_)
        stop(!enabled_ ? "turned off"
             : !panel_ ? "the NVIDIA GPU no longer drives the panel"
             : asleep_ ? "PC going to sleep"
                       : "screen on");
}
void GpuKeepAwake::stop(const std::string& why) {
    active_ = false;
    KillTimer(window_, tickTimer);
    SetTimer(window_, releaseTimer, releaseMs, nullptr); // Not now: leave the GPU alone while the screen comes back.
    log_("GPU keep-awake: stopped (" + why + ") after " + std::to_string(ticks_) + " ticks");
}
void GpuKeepAwake::timer(UINT_PTR id) {
    if (id == tickTimer) tick();
    if (id == releaseTimer) {
        KillTimer(window_, releaseTimer);
        if (!active_) release();
    }
}
void GpuKeepAwake::tick() {
    if (!active_) return;
    auto now = GetTickCount64();
    auto gap = now - last_;
    if (gap > staleMs) { // Frozen, or the PC slept: the GPU may be powered down by now.
        seenOn_ = false;
        return stop("paused " + std::to_string(gap) + " ms; waiting for the next screen-on");
    }
    last_ = now;
    if (int state = nv::Store::power_state(panel_->instance); state != 0) {
        seenOn_ = false;
        return stop((state < 0 ? std::string("GPU power state unknown") : "GPU already in D" + std::to_string(state)) +
                    "; waiting for the next screen-on");
    }
    if (!device_ && !create()) return;
    float color[4]{float(ticks_ & 1), 0, 0, 1};
    context_->ClearRenderTargetView(view_.Get(), color);
    context_->CopyResource(staging_.Get(), target_.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    auto hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped); // Waits until the GPU has done it.
    if (SUCCEEDED(hr))
        context_->Unmap(staging_.Get(), 0);
    else {
        log_("GPU keep-awake: read-back failed " + hex(hr) + "; the device is rebuilt on the next tick");
        release(); // For example, the device was lost in a driver reset.
    }
    ++ticks_;
}
bool GpuKeepAwake::create() {
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    auto hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->EnumAdapterByLuid(panel_->adapter, IID_PPV_ARGS(&adapter));
    if (SUCCEEDED(hr))
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_SINGLETHREADED |
                                   D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS,
                               nullptr, 0, D3D11_SDK_VERSION, &device_, nullptr, &context_);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (SUCCEEDED(hr)) hr = device_->CreateTexture2D(&desc, nullptr, &target_);
    if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(target_.Get(), nullptr, &view_);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr)) hr = device_->CreateTexture2D(&desc, nullptr, &staging_);
    if (SUCCEEDED(hr)) return true;
    if (!failed_) log_("GPU keep-awake: cannot create the Direct3D device " + hex(hr) + "; retrying each tick");
    failed_ = true;
    release();
    return false;
}
void GpuKeepAwake::release() {
    view_.Reset();
    staging_.Reset();
    target_.Reset();
    context_.Reset();
    device_.Reset();
}
}
