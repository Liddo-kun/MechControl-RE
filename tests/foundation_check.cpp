#include "worker.h"
#include "counters.h"
#include <pdh.h>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* name) {
    if (!condition) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}
}
int wmain(int argc, wchar_t**) {
    try {
        auto delivery = std::make_shared<mech::Delivery>();
        delivery->post({mech::ResultKind::sample, 1, L"first"});
        delivery->post({mech::ResultKind::command, 7, L"command"});
        delivery->post({mech::ResultKind::sample, 2, L"latest"});
        auto pending = delivery->take();
        require(pending.size() == 2 && pending[0].sequence == 2 && pending[1].sequence == 7, "slow UI receives coalesced latest sample and every command");
        delivery->close();
        delivery->post({mech::ResultKind::ble, 0, L"late callback"});
        require(delivery->take().empty(), "closed delivery rejects late callback");
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
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
