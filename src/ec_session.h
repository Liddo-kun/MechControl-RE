#pragma once
#include "ec.h"

namespace mech {
struct FanSnapshot {
    config::Curve cpu,gpu;
    std::array<uint8_t,16> probe{};
    uint8_t enable=0,mode=0;
    int active_entries() const;
    bool valid() const;
    std::optional<bool> gpu_present() const;
    // The caller must supply a firmware-qualified latch mask. No universal whole-byte equality.
    bool custom_mode(uint8_t latch_mask) const { return (enable&128)!=0 && (mode&latch_mask)==4; }
};
FanSnapshot read_fans(EcTransport&,const config::FanTable&);
struct Sensors {
    int cpu_rpm=0,gpu_rpm=0;
    std::optional<int> cpu_percent,gpu_percent,cpu_limit,battery_soc,battery_mode,battery_raw,cpu_temp,gpu_temp,
        system_watts,db_live,tgp_live;
    std::optional<double> adapter_amps;
    std::optional<bool> boost,curve_enabled;
};
Sensors read_sensors(EcTransport&,const config::MachineProfile&,bool lightweight,
    const std::optional<Sensors>& shared={});
using ChannelFactory=std::function<std::unique_ptr<EcChannel>(const std::wstring&)>;
class EcSession {
private:
    // Declared first so the guard and all dependent session state die before the transport.
    std::unique_ptr<EcTransport> ec_;
    std::map<int,uint8_t> startup_reads_;
    bool read_only_;
public:
    EcSession(ChannelFactory,const std::function<void(const std::string&)>& diagnostic,bool read_only,
        std::function<void(unsigned)> wait={},const std::filesystem::path& directory=config::profiles_directory(),
        std::optional<config::Identity> identity={},
        const std::function<std::optional<std::filesystem::path>(const std::vector<std::filesystem::path>&)>& manual={});
    Sensors sample(bool lightweight) { return read_sensors(*ec_,profile,lightweight); }
    EcTransport& transport() { return *ec_; }
    void snapshot_fans(); // Control startup calls this after activation; diagnostics never activate.
    config::Json diagnostic_report();
    config::Identity identity;
    config::Selection selection;
    config::MachineProfile profile;
    config::Hardware hardware;
    config::Validation validation;
    uint8_t battery_bit3=0;
    FanSnapshot fans;
    std::unique_ptr<WriteGuard> guard;
};
}
