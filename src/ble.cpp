#include "ble.h"
#include "worker.h"

namespace mech {
BleDiscovery::~BleDiscovery() { cancel(); }
void BleDiscovery::start() {
    if (action_ && action_.Status() == winrt::Windows::Foundation::AsyncStatus::Started) return;
    action_ = discover(delivery_);
}
void BleDiscovery::cancel() { if (action_) action_.Cancel(); }
winrt::Windows::Foundation::AsyncStatus BleDiscovery::status() const {
    return action_ ? action_.Status() : winrt::Windows::Foundation::AsyncStatus::Completed;
}
winrt::Windows::Foundation::IAsyncAction BleDiscovery::discover(std::shared_ptr<Delivery> delivery) {
    auto cancel = co_await winrt::get_cancellation_token();
    cancel.enable_propagation();
    try {
        // Never capture a UI apartment: BLE awaits run independently of the serialized worker.
        co_await winrt::resume_background();
        auto adapter = co_await winrt::Windows::Devices::Bluetooth::BluetoothAdapter::GetDefaultAsync();
        if (cancel()) co_return;
        if (!adapter) {
            delivery->post({ResultKind::ble, 0, L"No Bluetooth adapter available"});
            co_return;
        }
        auto radio = co_await adapter.GetRadioAsync();
        if (cancel()) co_return;
        auto state = radio && radio.State()==winrt::Windows::Devices::Radios::RadioState::On ? L"on" : L"off or unavailable";
        delivery->post({ResultKind::ble, 0, L"Bluetooth adapter discovered; LE=" +
            std::to_wstring(adapter.IsLowEnergySupported()) + L", radio state=" + state});
    } catch (const winrt::hresult_canceled&) {
        // No delivery is needed for an explicitly canceled operation.
    } catch (const winrt::hresult_error& error) {
        if (!cancel()) delivery->post({ResultKind::ble, 0, L"Bluetooth discovery: " + std::wstring(error.message())});
    }
}
}
