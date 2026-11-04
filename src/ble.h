#pragma once
#include "settings.h"
#include <memory>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Radios.h>

namespace mech {
class Delivery;
enum class CoolerAction { scan,connect,disconnect,apply,firmware,auto_connect,availability };
enum class CoolerSection { all,fan,pump,rgb };
struct CoolerRequest {
    CoolerAction action=CoolerAction::scan;
    uint64_t address=0;
    config::CoolerSettings settings;
    bool auto_connect=false;
    bool persist=true; // Slot application changes live intent, not the standalone state file.
    CoolerSection section=CoolerSection::all; // Manual Apply must select one existing section.
};
// All GATT state belongs to one event-driven thread, never the EC worker or UI.
class Cooler {
public:
    // The optional resolver is a pending/late WinRT completion fixture seam; production never supplies it.
    using Resolver=std::function<winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Bluetooth::BluetoothLEDevice>(uint64_t)>;
    Cooler(config::ProfileStore&,std::function<void(const std::string&)>,Resolver resolver={},std::function<void(config::Json)> delivery={});
    ~Cooler();
    void request(const CoolerRequest&);
    void start(); // Called after startup slot intent is installed, never by the constructor.
    config::CoolerSettings intent() const;
    config::Json snapshot() const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
std::vector<std::vector<uint8_t>> cooler_packets(const config::CoolerSettings&,CoolerSection section=CoolerSection::all);
#ifdef MECHCONTROL_BLE_PROTOTYPE
// This owner is confined to the UI thread. The coroutine is static and holds only shared delivery.
class BleDiscovery {
public:
    explicit BleDiscovery(std::shared_ptr<Delivery> delivery) : delivery_(std::move(delivery)) {}
    ~BleDiscovery();
    void start();
    void cancel();
    winrt::Windows::Foundation::AsyncStatus status() const;
private:
    static winrt::Windows::Foundation::IAsyncAction discover(std::shared_ptr<Delivery> delivery);
    std::shared_ptr<Delivery> delivery_;
    winrt::Windows::Foundation::IAsyncAction action_{nullptr};
};
#endif
}
