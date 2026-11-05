#pragma once
#include "settings.h"
#include <memory>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Radios.h>

namespace mech {
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
}
