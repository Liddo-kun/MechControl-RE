#include "ble.h"
#include "worker.h"
#include "counters.h"
#include <pdh.h>
#include <iostream>
#include <functional>
#include <algorithm>
#include <stdexcept>

using namespace std::chrono_literals;
namespace {
void require(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}
bool waitFor(const std::function<bool()>& ready, std::chrono::milliseconds limit = 3s) {
    auto deadline = std::chrono::steady_clock::now() + limit;
    do { if (ready()) return true; std::this_thread::sleep_for(10ms); } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
}
int wmain(int argc, wchar_t**) {
    try {
        winrt::init_apartment();
        wchar_t path[32768]; GetModuleFileNameW(nullptr, path, 32768);
        mech::Log log(std::filesystem::path(path).parent_path() / L"foundation-check.log");
        auto delivery = std::make_shared<mech::Delivery>();
        {
            mech::ControlWorker worker(delivery, log,{.fixture=true});
            require(waitFor([&] { return !delivery->take().empty(); }), "first synthetic monitor sample");
            worker.submit({1, 10, 1200ms});
            worker.submit({2, 20, 0ms});
            std::vector<std::uint64_t> commands;
            require(waitFor([&] {
                for (auto& result : delivery->take()) if (result.kind == mech::ResultKind::command) commands.push_back(result.sequence);
                return commands.size() == 2;
            }), "serialized commands complete");
            require(commands == std::vector<std::uint64_t>{1, 2}, "command FIFO ordering");
            std::this_thread::sleep_for(2200ms);
            auto pending = delivery->take();
            require(std::count_if(pending.begin(), pending.end(), [](auto& r) { return r.kind == mech::ResultKind::sample; }) == 1,
                "slow UI receives coalesced latest sample");
            mech::BleDiscovery ble(delivery);
            ble.start();
            require(waitFor([&] { return ble.status() != winrt::Windows::Foundation::AsyncStatus::Started; }, 10s), "real WinRT adapter discovery terminates");
            for (auto& result : delivery->take()) std::wcout << L"BLE: " << result.text << L'\n';
            // Repeated cancellation followed by delivery destruction catches escaped UI/object ownership.
            for (int i = 0; i < 20; ++i) {
                mech::BleDiscovery canceled(delivery); canceled.start(); canceled.cancel();
                require(waitFor([&] { return canceled.status() != winrt::Windows::Foundation::AsyncStatus::Started; }), "BLE cancellation settles");
            }
            worker.submit({3, 30, 30s});
            std::this_thread::sleep_for(50ms);
            auto start = std::chrono::steady_clock::now();
            delivery->close();
            worker.stop();
            require(std::chrono::steady_clock::now() - start < 1s, "outstanding command shutdown is interruptible");
            require(!worker.submit({4, 40}), "closed worker refuses commands");
            delivery->post({mech::ResultKind::ble, 0, L"late callback"});
            require(delivery->take().empty(), "closed delivery rejects late callback");
        }
        if (argc > 1) {
            mech::Counters counters;
            require(counters.error() == ERROR_SUCCESS, "native PerfLib provider starts");
            counters.publish(1234, 2345);
            PDH_HQUERY query = nullptr;
            require(PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS, "PDH query opens");
            struct Query { PDH_HQUERY value; ~Query() { PdhCloseQuery(value); } } cleanup{query};
            PDH_HCOUNTER cpu, gpu;
            require(PdhAddEnglishCounterW(query, L"\\MechControl\\CPU Fan RPM", 0, &cpu) == ERROR_SUCCESS, "CPU counter exact path");
            require(PdhAddEnglishCounterW(query, L"\\MechControl\\GPU Fan RPM", 0, &gpu) == ERROR_SUCCESS, "GPU counter exact path");
            require(PdhCollectQueryData(query) == ERROR_SUCCESS, "PDH collects native counters");
            for (auto [counter, expected] : {std::pair{cpu, 1234LL}, std::pair{gpu, 2345LL}}) {
                PDH_FMT_COUNTERVALUE value{};
                require(PdhGetFormattedCounterValue(counter, PDH_FMT_LARGE, nullptr, &value) == ERROR_SUCCESS &&
                    value.CStatus == ERROR_SUCCESS && value.largeValue == expected, "PDH reads published 64-bit raw RPM");
            }
            counters.publish(3456, 4567);
            require(PdhCollectQueryData(query) == ERROR_SUCCESS, "PDH collects update");
            PDH_FMT_COUNTERVALUE value{};
            require(PdhGetFormattedCounterValue(cpu, PDH_FMT_LARGE, nullptr, &value) == ERROR_SUCCESS && value.largeValue == 3456,
                "PDH observes subsequent publication");
        }
        winrt::uninit_apartment();
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
