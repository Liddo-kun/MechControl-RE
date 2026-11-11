#include "ble.h"
#include "config_text.h"
#include "win_handle.h"
#include "ble_wait.h"
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <algorithm>
#include <deque>
#include <mutex>
#include <thread>

namespace mech {
using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::Advertisement;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;
using config::CoolerSettings;
std::vector<std::vector<uint8_t>> cooler_packets(const CoolerSettings& s, CoolerSection section) {
    std::vector<std::vector<uint8_t>> packets{
        {0xFE, 0x1B, uint8_t(s.fan_on), uint8_t(s.fan_on ? std::clamp(s.fan_duty, 0, 100) : 0), 0, 0, 0, 0xEF},
        {0xFE, 0x1C, uint8_t(s.pump_on), uint8_t(s.pump_on ? std::clamp(s.pump_duty, 0, 100) : 0),
         uint8_t(s.pump_on ? s.voltage : config::PumpVoltage::V11), 0, 0, 0xEF},
        {0xFE, 0x1E, uint8_t(s.rgb_on), uint8_t(s.rgb_on ? s.r : 0), uint8_t(s.rgb_on ? s.g : 0),
         uint8_t(s.rgb_on ? s.b : 0), uint8_t(s.rgb_on ? s.rgb_mode : config::CoolerRgbMode::Static), 0xEF},
        {0xFE, 0x33, 0, 0, 0, 0, 0, 0xEF}};
    if (section == CoolerSection::all) return packets;
    return {packets.at(static_cast<size_t>(section) - 1), packets.back()};
}
namespace {
constexpr guid service_id{0x6e400001, 0xb5a3, 0xf393, {0xe0, 0xa9, 0xe5, 0x0e, 0x24, 0xdc, 0xca, 0x9e}};
constexpr guid tx_id{0x6e400002, 0xb5a3, 0xf393, {0xe0, 0xa9, 0xe5, 0x0e, 0x24, 0xdc, 0xca, 0x9e}};
constexpr guid rx_id{0x6e400003, 0xb5a3, 0xf393, {0xe0, 0xa9, 0xe5, 0x0e, 0x24, 0xdc, 0xca, 0x9e}};
constexpr DWORD backoff[]{8000, 8000, 8000, 8000, 20000, 40000, 300000};
using ble_detail::Event;
struct ScanData {
    std::mutex mutex;
    config::Json devices = config::Json::array();
    Handle found{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
};
void merge(CoolerSettings& target, const CoolerSettings& value, CoolerSection section) {
    if (section == CoolerSection::all)
        target = value;
    else if (section == CoolerSection::fan) {
        target.fan_on = value.fan_on;
        target.fan_duty = value.fan_duty;
    } else if (section == CoolerSection::pump) {
        target.pump_on = value.pump_on;
        target.pump_duty = value.pump_duty;
        target.voltage = value.voltage;
    } else {
        target.rgb_on = value.rgb_on;
        target.r = value.r;
        target.g = value.g;
        target.b = value.b;
        target.rgb_mode = value.rgb_mode;
    }
}
// Requests and the saved cooler file get the same limits; unknown voltages use the source's V8 default.
void normalize(CoolerSettings& s) {
    s.fan_duty = std::clamp(s.fan_duty, 0, 100);
    s.pump_duty = std::clamp(s.pump_duty, 0, 100);
    if (uint8_t(s.voltage) > 3) s.voltage = config::PumpVoltage::V8;
    s.rgb_mode = static_cast<config::CoolerRgbMode>(std::clamp<int>(uint8_t(s.rgb_mode), 0, 3));
}
}
struct Cooler::Impl {
    struct Queue {
        std::mutex mutex;
        std::deque<CoolerRequest> pending;
        bool closed = false, lost = false;
        Handle wake{CreateEventW(nullptr, FALSE, FALSE, nullptr)}, stop{CreateEventW(nullptr, TRUE, FALSE, nullptr)},
            superseded{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        config::CoolerState state;
        CoolerSettings intent;
        config::Json devices = config::Json::array();
        std::string status = "Disconnected", firmware;
        bool connected = false;
        std::optional<bool> bluetooth_available;
        uint64_t generation = 0;
    };
    std::shared_ptr<Queue> queue = std::make_shared<Queue>();
    config::ProfileStore& store;
    std::function<void(const std::string&)> log;
    Resolver resolver;
    std::function<void(config::Json)> delivery;
    bool started = false;
    BluetoothLEDevice device{nullptr};
    GattDeviceService service{nullptr};
    GattCharacteristic tx{nullptr}, rx{nullptr};
    event_token lost_token{};
    bool subscribed = false;
    GattWriteOption mode = GattWriteOption::WriteWithResponse;
    ULONGLONG retry_at = 0;
    size_t attempt = 0;
    std::jthread thread;
    Impl(config::ProfileStore& s, std::function<void(const std::string&)> l, Resolver r,
         std::function<void(config::Json)> d)
        : store(s), log(std::move(l)), resolver(std::move(r)), delivery(std::move(d)) {
        queue->state = store.load_cooler();
        normalize(queue->state);
        queue->intent = queue->state;
        if (!queue->wake || !queue->stop || !queue->superseded)
            throw std::runtime_error("Cannot create cooler wait handles");
    }
    void start() {
        if (!thread.joinable()) thread = std::jthread([this] { run(); });
    }
    ~Impl() {
        {
            std::lock_guard lock(queue->mutex);
            queue->closed = true;
            queue->pending.clear();
        }
        SetEvent(queue->stop.get());
        if (thread.joinable()) thread.join();
    }
    config::Json snapshot() const {
        std::lock_guard lock(queue->mutex);
        return {{"connected", queue->connected},
                {"status", queue->status},
                {"firmware", queue->firmware},
                {"devices", queue->devices},
                {"state", queue->state},
                {"intent", queue->intent},
                {"bluetoothAvailable", config::nullable(queue->bluetooth_available)}};
    }
    void notify(std::optional<CoolerAction> action = {}, bool ok = true) {
        if (!delivery) return;
        auto value = snapshot();
        value["completed"] = action.has_value();
        value["ok"] = ok;
        if (action) value["action"] = static_cast<int>(*action);
        delivery(std::move(value));
    }
    void status(std::string text) {
        {
            std::lock_guard lock(queue->mutex);
            if (queue->closed) return;
            queue->status = text;
        }
        log("[BLE] " + text);
        notify();
    }
    config::CoolerState state() const {
        std::lock_guard lock(queue->mutex);
        return queue->state;
    }
    CoolerSettings intent() const {
        std::lock_guard lock(queue->mutex);
        return queue->intent;
    }
    void save() { store.save_cooler(state()); }
    template <class Op> auto get(const Op& op, ULONGLONG deadline) {
        return ble_detail::await(op, queue->stop.get(), queue->superseded.get(), deadline);
    }
    bool pause(DWORD ms) {
        HANDLE waits[]{queue->stop.get(), queue->superseded.get()};
        return WaitForMultipleObjects(2, waits, FALSE, ms) == WAIT_TIMEOUT;
    }
    bool bluetooth(ULONGLONG deadline = GetTickCount64() + 5000) {
        bool available = false;
        try {
            auto adapter = get(BluetoothAdapter::GetDefaultAsync(), deadline);
            if (adapter && adapter.IsLowEnergySupported()) {
                auto radio = get(adapter.GetRadioAsync(), deadline);
                available = radio && radio.State() == Windows::Devices::Radios::RadioState::On;
            }
        } catch (...) {
            // Transient OS errors remain retryable.
        }
        {
            std::lock_guard lock(queue->mutex);
            queue->bluetooth_available = available;
        }
        return available;
    }
    bool scan(uint64_t target = 0) {
        auto data = std::make_shared<ScanData>();
        BluetoothLEAdvertisementWatcher watcher;
        watcher.ScanningMode(BluetoothLEScanningMode::Active);
        auto token =
            watcher.Received([data, target](auto const&, BluetoothLEAdvertisementReceivedEventArgs const& args) {
                if (target) {
                    if (args.BluetoothAddress() == target) SetEvent(data->found.get());
                    return;
                }
                auto name = to_string(args.Advertisement().LocalName());
                auto lowered = config::lower_invariant(config::wide(name));
                if (lowered.find(L"lct21001") == std::wstring::npos && lowered.find(L"lct22002") == std::wstring::npos)
                    return;
                std::lock_guard lock(data->mutex);
                auto& list = data->devices;
                auto i = std::find_if(list.begin(), list.end(),
                                      [&](auto& d) { return d["address"] == args.BluetoothAddress(); });
                config::Json value = {
                    {"address", args.BluetoothAddress()}, {"name", name}, {"rssi", args.RawSignalStrengthInDBm()}};
                if (i == list.end())
                    list.push_back(value);
                else
                    *i = value;
            });
        struct Watch {
            BluetoothLEAdvertisementWatcher w;
            event_token t;
            ~Watch() {
                try {
                    w.Stop();
                    w.Received(t);
                } catch (...) {
                }
            }
        } cleanup{watcher, token};
        watcher.Start();
        HANDLE waits[]{queue->stop.get(), queue->superseded.get(), data->found.get()};
        auto result = WaitForMultipleObjects(3, waits, FALSE, 5000);
        if (result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1) throw std::runtime_error("BLE scan canceled");
        if (target) return result == WAIT_OBJECT_0 + 2;
        {
            std::lock_guard lock(data->mutex);
            std::lock_guard qlock(queue->mutex);
            queue->devices = data->devices;
        }
        return true;
    }
    void close() {
        {
            std::lock_guard lock(queue->mutex);
            ++queue->generation;
            queue->lost = false;
            queue->connected = false;
            queue->firmware.clear();
        }
        tx = nullptr;
        rx = nullptr;
        if (device && subscribed) {
            device.ConnectionStatusChanged(lost_token);
            subscribed = false;
        }
        if (service) {
            service.Close();
            service = nullptr;
        }
        if (device) {
            device.Close();
            device = nullptr;
        }
    }
    void write(std::vector<uint8_t> bytes) {
        if (!tx) throw std::runtime_error("Cooler is not connected");
        DataWriter writer;
        writer.WriteBytes(bytes);
        if (get(tx.WriteValueAsync(writer.DetachBuffer(), mode), GetTickCount64() + 5000) !=
            GattCommunicationStatus::Success)
            throw std::runtime_error("Cooler GATT write failed");
    }
    void apply(const CoolerSettings& settings, CoolerSection section = CoolerSection::all) {
        if (!tx) throw std::runtime_error("Cooler is not connected; settings were not applied or saved");
        for (auto& packet : cooler_packets(settings, section)) write(packet);
        status("Cooler settings written and committed");
    }
    void firmware() {
        if (!rx) {
            status("Cooler RX unavailable; firmware not read");
            return;
        }
        struct Response {
            std::mutex mutex;
            std::vector<uint8_t> bytes;
            Event event;
        };
        auto response = std::make_shared<Response>();
        auto token = rx.ValueChanged([response](auto const&, GattValueChangedEventArgs const& args) {
            auto reader = DataReader::FromBuffer(args.CharacteristicValue());
            std::lock_guard lock(response->mutex);
            response->bytes.resize(reader.UnconsumedBufferLength());
            reader.ReadBytes(response->bytes);
            SetEvent(response->event.handle.get());
        });
        struct Notification {
            GattCharacteristic rx;
            event_token token;
            ~Notification() { rx.ValueChanged(token); }
        } cleanup{rx, token};
        write({0x73, 0x77});
        HANDLE waits[]{queue->stop.get(), queue->superseded.get(), response->event.handle.get()};
        if (WaitForMultipleObjects(3, waits, FALSE, 5000) == WAIT_OBJECT_0 + 2) {
            std::lock_guard lock(response->mutex);
            auto& bytes = response->bytes;
            while (!bytes.empty() && bytes.back() == 0) bytes.pop_back();
            std::lock_guard qlock(queue->mutex);
            queue->firmware.assign(bytes.begin(), bytes.end());
        } else
            status("Cooler firmware response timed out or canceled");
    }
    void connect(uint64_t address) {
        auto deadline = GetTickCount64() + 15000;
        close();
        status("Connecting to cooler");
        if (!resolver && !bluetooth(std::min(deadline, GetTickCount64() + 5000)))
            throw std::runtime_error("Bluetooth is off or unavailable");
        if (!resolver && !scan(address)) throw std::runtime_error("Cooler advertisement not found");
        for (int tries = 0; tries < 3; ++tries) {
            close();
            device =
                get(resolver ? resolver(address) : BluetoothLEDevice::FromBluetoothAddressAsync(address), deadline);
            if (!device) throw std::runtime_error("Cooler unavailable in Windows advertisement cache");
            auto services = get(device.GetGattServicesForUuidAsync(service_id), deadline);
            if (services.Status() == GattCommunicationStatus::Success && services.Services().Size()) {
                service = services.Services().GetAt(0);
                auto chars = get(service.GetCharacteristicsForUuidAsync(tx_id), deadline);
                if (chars.Status() == GattCommunicationStatus::Success && chars.Characteristics().Size())
                    tx = chars.Characteristics().GetAt(0);
            }
            if (tx) break;
            if (tries == 2 || !pause(1500)) throw std::runtime_error("Cooler Nordic UART service/TX unavailable");
        }
        auto props = tx.CharacteristicProperties();
        if ((props & GattCharacteristicProperties::Write) != GattCharacteristicProperties::None)
            mode = GattWriteOption::WriteWithResponse;
        else if ((props & GattCharacteristicProperties::WriteWithoutResponse) != GattCharacteristicProperties::None)
            mode = GattWriteOption::WriteWithoutResponse;
        else
            throw std::runtime_error("Cooler TX has no supported write mode");
        try {
            auto chars = get(service.GetCharacteristicsForUuidAsync(rx_id), deadline);
            if (chars.Status() == GattCommunicationStatus::Success && chars.Characteristics().Size()) {
                rx = chars.Characteristics().GetAt(0);
                if (get(rx.WriteClientCharacteristicConfigurationDescriptorAsync(
                            GattClientCharacteristicConfigurationDescriptorValue::Notify),
                        deadline) != GattCommunicationStatus::Success)
                    log("[BLE] Notification enable failed");
            }
        } catch (const hresult_error& e) {
            log("[BLE] Optional notifications unavailable: " + to_string(e.message()));
        } catch (const std::exception& e) {
            log(std::string("[BLE] Optional notifications unavailable: ") + e.what());
        } catch (...) {
            log("[BLE] Optional notifications unavailable");
        }
        // Every wait must complete for this attempt before it can publish connection state.
        if (WaitForSingleObject(queue->stop.get(), 0) == WAIT_OBJECT_0 ||
            WaitForSingleObject(queue->superseded.get(), 0) == WAIT_OBJECT_0 || GetTickCount64() >= deadline)
            throw std::runtime_error("Obsolete cooler connection discarded");
        std::weak_ptr<Queue> weak = queue;
        uint64_t generation;
        {
            std::lock_guard lock(queue->mutex);
            generation = queue->generation;
        }
        lost_token = device.ConnectionStatusChanged([weak, generation](BluetoothLEDevice const& d, auto const&) {
            if (d.ConnectionStatus() != BluetoothConnectionStatus::Disconnected) return;
            if (auto q = weak.lock()) {
                std::lock_guard lock(q->mutex);
                if (q->closed || generation != q->generation) return;
                q->lost = true;
                SetEvent(q->wake.get());
            }
        });
        subscribed = true;
        {
            std::lock_guard lock(queue->mutex);
            if (queue->closed || WaitForSingleObject(queue->superseded.get(), 0) == WAIT_OBJECT_0)
                throw std::runtime_error("Obsolete cooler connection discarded");
            queue->connected = true;
            queue->state.address = address;
            queue->state.device_name = to_string(device.Name());
        }
        attempt = 0;
        retry_at = 0;
        save();
        status("Connected to cooler");
        try {
            apply(intent());
        } catch (const hresult_error& e) {
            log("[BLE] Settings apply failed: " + to_string(e.message()));
        } catch (const std::exception& e) {
            log(std::string("[BLE] Settings apply failed: ") + e.what());
        }
        try {
            firmware();
        } catch (const hresult_error& e) {
            log("[BLE] Optional firmware read failed: " + to_string(e.message()));
        } catch (const std::exception& e) {
            log(std::string("[BLE] Optional firmware read failed: ") + e.what());
        }
    }
    void retry() {
        if (state().address) retry_at = GetTickCount64() + backoff[std::min(attempt++, std::size(backoff) - 1)];
    }
    void run() noexcept {
        try {
            init_apartment(apartment_type::multi_threaded);
            for (;;) {
                auto now = GetTickCount64();
                HANDLE waits[]{queue->stop.get(), queue->wake.get()};
                auto ready = WaitForMultipleObjects(2, waits, FALSE,
                                                    retry_at ? DWORD(retry_at > now ? retry_at - now : 0) : INFINITE);
                if (ready == WAIT_OBJECT_0) break;
                std::optional<CoolerRequest> command;
                bool lost = false, retrying = false;
                {
                    std::lock_guard lock(queue->mutex);
                    lost = std::exchange(queue->lost, false);
                    if (!queue->pending.empty()) {
                        command = std::move(queue->pending.front());
                        queue->pending.pop_front();
                        if (!queue->pending.empty()) SetEvent(queue->wake.get());
                    }
                    ResetEvent(queue->superseded.get());
                }
                try {
                    if (lost) {
                        close();
                        status("Cooler connection lost");
                        attempt = 0;
                        if (state().auto_connect) retry();
                    }
                    if (!command && ready == WAIT_TIMEOUT) {
                        retry_at = 0;
                        retrying = true;
                        // Match the source's quiet radio-off retry gate.
                        if (!resolver && !bluetooth()) {
                            retry();
                            continue;
                        }
                        command = CoolerRequest{CoolerAction::connect, state().address};
                    }
                    if (!command) continue;
                    auto c = *command;
                    if (c.action == CoolerAction::scan) {
                        retry_at = 0;
                        attempt = 0;
                        if (!bluetooth())
                            throw std::runtime_error("Bluetooth is off or unavailable");
                        else {
                            status("Scanning for coolers");
                            scan();
                            status("Cooler scan complete");
                        }
                    } else if (c.action == CoolerAction::connect) {
                        if (!retrying) attempt = 0;
                        retry_at = 0;
                        connect(c.address);
                    } else if (c.action == CoolerAction::disconnect) {
                        retry_at = 0;
                        attempt = 0;
                        if (tx) try {
                                write({0xFE, 0x19, 0, 1, 0, 0, 0, 0xEF});
                            } catch (...) {
                            }
                        close();
                        status("Disconnected");
                    } else if (c.action == CoolerAction::apply) {
                        if (c.persist && c.section == CoolerSection::all)
                            throw std::runtime_error("Manual cooler Apply requires fan, pump or RGB section");
                        if (c.persist || tx)
                            apply(c.settings, c.section);
                        else
                            status("Cooler slot intent updated; disconnected, hardware not applied");
                        if (c.persist) {
                            {
                                std::lock_guard lock(queue->mutex);
                                merge(queue->state, c.settings, c.section);
                            }
                            save();
                        }
                    } else if (c.action == CoolerAction::firmware)
                        firmware();
                    else if (c.action == CoolerAction::auto_connect)
                        save(); // Preference changes alone do not initiate/stop retries.
                    else if (c.action == CoolerAction::availability)
                        status(bluetooth() ? "Bluetooth available" : "Bluetooth is off or unavailable");
                    notify(c.action);
                } catch (const hresult_error& e) {
                    if (command && command->action == CoolerAction::connect) {
                        close();
                        retry();
                    }
                    status("Cooler operation failed: " + to_string(e.message()));
                    if (command) notify(command->action, false);
                } catch (const std::exception& e) {
                    if (command && command->action == CoolerAction::connect) {
                        close();
                        retry();
                    }
                    status(std::string("Cooler operation failed: ") + e.what());
                    if (command) notify(command->action, false);
                }
            }
            // Best-effort reset, bounded separately; never revive a pending connection.
            if (tx) {
                try {
                    DataWriter writer;
                    writer.WriteBytes(std::vector<uint8_t>{0xFE, 0x19, 0, 1, 0, 0, 0, 0xEF});
                    auto op = tx.WriteValueAsync(writer.DetachBuffer(), mode);
                    auto done = std::make_shared<Event>();
                    op.Completed([done](auto const&, AsyncStatus) { SetEvent(done->handle.get()); });
                    if (WaitForSingleObject(done->handle.get(), 1000) != WAIT_OBJECT_0) op.Cancel();
                } catch (...) {
                }
            }
            close();
            uninit_apartment();
        } catch (const hresult_error& e) {
            status("BLE owner failed: " + to_string(e.message()));
        } catch (const std::exception& e) {
            status(e.what());
        }
    }
};
Cooler::Cooler(config::ProfileStore& store, std::function<void(const std::string&)> log, Resolver resolver,
               std::function<void(config::Json)> delivery)
    : impl_(std::make_unique<Impl>(store, std::move(log), std::move(resolver), std::move(delivery))) {}
Cooler::~Cooler() = default;
void Cooler::start() {
    if (impl_->started) return;
    impl_->started = true;
    auto s = impl_->state();
    if (s.auto_connect && s.address) request({CoolerAction::connect, s.address});
}
void Cooler::request(const CoolerRequest& request) {
    auto c = request;
    normalize(c.settings);
    auto q = impl_->queue;
    std::unique_lock lock(q->mutex);
    if (q->closed) return;
    if (c.action == CoolerAction::apply) {
        if (c.persist && c.section == CoolerSection::all)
            throw std::invalid_argument("Manual cooler Apply requires fan, pump or RGB section");
        if (c.persist && !q->connected)
            throw std::runtime_error("Cooler is not connected; settings were not applied or saved");
        merge(q->intent, c.settings, c.section);
        if (!c.persist && !q->connected) {
            // A disconnected slot only installs intent. A pending connect reads this
            // under the same mutex; a later connected request queues actual output.
            q->status = "Cooler slot intent updated; disconnected, hardware not applied";
            lock.unlock();
            impl_->notify(c.action);
            return;
        }
    }
    if (c.action == CoolerAction::auto_connect) q->state.auto_connect = c.auto_connect;
    if (c.action == CoolerAction::connect || c.action == CoolerAction::disconnect || c.action == CoolerAction::scan) {
        SetEvent(q->superseded.get());
        std::erase_if(q->pending, [](auto& r) {
            return r.action == CoolerAction::connect || r.action == CoolerAction::disconnect ||
                   r.action == CoolerAction::scan;
        });
    }
    q->pending.push_back(c);
    SetEvent(q->wake.get());
    impl_->start();
}
CoolerSettings Cooler::intent() const { return impl_->intent(); }
config::Json Cooler::snapshot() const { return impl_->snapshot(); }
}
