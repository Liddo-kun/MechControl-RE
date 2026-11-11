#pragma once
#include "profiles.h"
#include "win_handle.h"
#include <span>

namespace mech {
inline constexpr uint32_t ecReadIoctl = 0x9C40A488, ecWriteIoctl = 0x9C40A48C, ecSpecIoctl = 0x9C40A500;
// The driver ABI is signed int32 addresses and uint32 output, not native long/pointers.
// Channel injection checks request words above the Windows wrapper without opening hardware.
class EcChannel {
public:
    virtual ~EcChannel() = default;
    virtual uint32_t ioctl(uint32_t code, std::span<const int32_t> input) = 0;
    virtual void reopen() = 0;
};
std::unique_ptr<EcChannel> open_ec_channel(const std::wstring& path, HANDLE stopping);
class WriteGuard;
class EcTransport {
public:
    EcTransport(std::unique_ptr<EcChannel> channel, std::function<void(const std::string&)> diagnostic = {});
    uint8_t read(int32_t address);
    uint32_t spec(int index);

private:
    friend class WriteGuard;
    void write(int32_t address, uint8_t value);
    uint32_t retry(uint32_t code, std::span<const int32_t> input);
    std::unique_ptr<EcChannel> channel_;
    std::function<void(const std::string&)> diagnostic_;
};
class VerifyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
struct EcWrite {
    int32_t address;
    uint8_t value;
    uint8_t mask = 255;
};
class WriteGuard {
public:
    WriteGuard(EcTransport&, const config::MachineProfile&, bool read_only, std::function<void(unsigned)> wait = {});
    void admit(int32_t address) const;
    bool verifies_same_address(int32_t address) const;
    void write(int32_t address, uint8_t value);
    void bitfield(int32_t address, uint8_t mask, uint8_t value);
    // Preflight every address before any write. A driver failure is still a partial apply.
    void apply(std::span<const EcWrite> writes);
    std::function<bool()> recover;

private:
    void write_verify(int32_t address, uint8_t value);
    EcTransport& ec_;
    bool read_only_, recovering_ = false;
    std::set<int> allow_, skip_;
    std::map<int, config::Redirect> redirects_;
    std::function<void(unsigned)> wait_;
};
// Complete, model-range-checked EC groups. NVIDIA/thermal policy is the caller's responsibility.
config::Limit control_limit(const config::MachineProfile&, const std::string& key);
// Every control's limit, resolved once for the controller.
struct ControlLimits {
    config::Limit pl1, pl2, pl4, cpu_temperature, tgp, dynamic_boost;
};
ControlLimits control_limits(const config::MachineProfile&);
std::vector<EcWrite> cpu_writes(const config::MachineProfile&, int pl1, int pl2, int pl4, int temperature);
std::vector<EcWrite> gpu_writes(const config::MachineProfile&, int tgp, bool dynamic_boost, int db,
                                std::optional<int> maximum = {});
EcWrite fan_boost_write(const config::MachineProfile&, bool enabled);
// Curves contain active entries only, with raw 0..200 duty as in saved profiles.
// The caller qualifies latch_mask: 0x04 for shared-bit firmware, 0xFF for the
// reference whole-byte latch. There is deliberately no universal default.
std::vector<EcWrite> fan_curve_writes(const config::MachineProfile&, const config::Curve& cpu,
                                      const std::optional<config::Curve>& gpu, bool enabled, uint8_t latch_mask);
}
