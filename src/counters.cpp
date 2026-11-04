#include "counters.h"
// ctrpp owns the fixed provider and counter-set metadata, not application lifetimes.
#include <mechcontrol_counters.h>

namespace mech {
Counters::Counters() {
    error_ = PerfStartProvider(&MechControlProviderGuid, nullptr, &provider_);
    if (error_ != ERROR_SUCCESS) return;
    error_ = PerfSetCounterSetInfo(provider_, &MechControlCountersInfo.CounterSet,
        sizeof(MechControlCountersInfo));
    if (error_ != ERROR_SUCCESS) return;
    instance_ = PerfCreateInstance(provider_, &MechControlCountersGuid, L"", 0);
    if (!instance_) error_ = GetLastError();
}
Counters::~Counters() {
    if (instance_) PerfDeleteInstance(provider_, instance_);
    if (provider_) PerfStopProvider(provider_);
}
void Counters::publish(std::uint64_t cpu, std::uint64_t gpu) {
    if (!instance_) return;
    auto error = PerfSetULongLongCounterValue(provider_, instance_, 1, cpu);
    if (error == ERROR_SUCCESS) error = PerfSetULongLongCounterValue(provider_, instance_, 2, gpu);
    error_ = error;
}
}
