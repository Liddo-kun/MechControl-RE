#pragma once
#include "ec_session.h"
#include "settings.h"
#include "win_handle.h"

namespace mech {
using HidReport = std::array<uint8_t, 9>;
class Lighting {
public:
    using Send = std::function<void(int, const HidReport&)>;
    Lighting(EcSession&, config::ProfileStore&, std::function<void(const std::string&)>, bool hardware, Send send = {},
             std::function<void(std::function<void()>)> run_io = {},
             std::function<void(const std::vector<EcWrite>&)> batch = {});
    bool open();
    bool available(int) const;
    void zone(int, const config::Zone&, bool flash = false, bool color_only = false);
    void change(int, const config::Zone&, bool flash = false, bool color_only = false);
    config::Zone logo();
    void apply(const config::LightingSettings&);
    void capture(config::LightingSettings&);
    void idle(int);
    void resume(ULONGLONG now);
    void tick(ULONGLONG now, std::optional<DWORD> idle_ms = {});
    const config::LightingState& intent() const { return state_; }

private:
    bool open_device();
    void io(const std::function<void()>&);
    void send(int, const HidReport&);
    void restore(bool reopen);
    EcSession& session_;
    config::ProfileStore& store_;
    std::function<void(const std::string&)> log_;
    std::array<Handle, 4> handles_;
    bool hardware_, sleeping_ = false;
    ULONGLONG resume_at_ = 0;
    Send output_;
    std::function<void(std::function<void()>)> run_io_;
    std::function<void(const std::vector<EcWrite>&)> batch_;
    bool io_busy_ = false;
    config::LightingState state_;
};
bool local_dimming_available();
void set_local_dimming(bool);
void ensure_uefi_flags(const std::function<void(const std::string&)>&);
config::Json battery_telemetry(EcTransport&);
// What the EC's charge-voltage routine keeps (re/register_reference.md 11.6.1): the voltage the pack requests, the
// lower one the EC gives the charger, and the cell count. No profile or version number says which firmware has
// the routine and the charge hold beside it; these readings following its arithmetic do.
struct ChargeVoltage {
    int request_mv = 0, target_mv = 0, cells = 0;
    // A lithium pack's request, lowered by one of the routine's 50 mV-per-cell steps.
    bool recognized() const {
        int cell = cells ? request_mv / cells : 0, cut = request_mv - target_mv;
        return cell >= 3500 && cell <= 4600 && cut >= 0 && cut <= 250 * cells && cut % (50 * cells) == 0;
    }
};
ChargeVoltage charge_voltage(EcTransport&);
// Why the machine last powered down, as the EC recorded it. Empty when the EC does not record it this way.
std::string power_off_cause(EcTransport&);
}
