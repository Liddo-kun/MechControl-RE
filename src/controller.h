#pragma once
#include "ec_session.h"
#include "thermal.h"
#include "protocol.h"
#include "nvidia.h"
#include "peripherals.h"
#include "ble.h"
#include <variant>

namespace mech {
struct CpuApply {
    config::CpuSettings settings;
};
struct GpuApply {
    config::GpuSettings settings;
    bool power = true;
    bool restart_confirmed = false;
    std::optional<config::CpuSettings> cpu;
};
struct GpuUnlock {
    bool enabled;
}; // Deliberate, confirmed UI action; never exposed by CLI.
struct DlppcApply {
    bool disabled;
};
struct AcEvent {
    bool on_ac;
};
struct FanApply {
    config::FanSettings settings;
}; // Curves/master/link; boost and switch have separate commands.
struct FanBoost {
    bool enabled;
};
struct FanMaster {
    bool enabled;
}; // Unsupported table formats still expose the existing master bit.
struct FanEdit {
    config::FanSettings settings;
}; // Linked editor updates only; the shared tick decides whether to write.
struct FanRead {};
struct BatteryApply {
    int mode;
};
struct LightingApply {
    int zone;
    config::Zone settings;
    bool flash = false, color_only = false;
};
struct LightingIdle {
    int seconds;
};
struct DimmingApply {
    bool enabled;
};
struct PeripheralRead {
    bool battery = false;
};
struct BatteryPage {
    bool active;
}; // Bind the existing visible-page two-second telemetry subscription.
struct UiRead {
    int page = -2;
}; // -2: initial metadata; -1: changed catalog; 0..6: explicit page read.
struct UiPreflight {
    std::optional<config::GpuSettings> gpu;
    int slot = 0;
    std::optional<config::CpuSettings> cpu;
};
struct UiPage {
    int page;
}; // The visible tab: Power=0 polls GPU telemetry, System Info=6 every EC sensor.
struct SwitchApply {
    bool enabled;
    int milliseconds;
};
struct ProtectionApply {
    config::ProtectionSettings settings;
};
struct SlotLoad {
    int slot;
    bool deliberate = true;
};
struct SlotSave {
    int slot;
    std::optional<config::FanSettings> editor_fan;
    std::optional<config::CoolerSettings> editor_cooler;
};
struct SlotRename {
    int slot;
    std::string name;
};
struct SlotClear {
    int slot;
};
struct DefaultsApply {
    std::string mode;
}; // Empty means firmware defaults.
struct ThermalReset {};
struct ThermalTest {
    std::optional<config::ProtectionSettings> expected;
};
struct Visibility {
    bool visible;
};
struct Resume {};
// Lifecycle events that turn the EC bypass off.
struct PowerTransition {
    enum Kind { display_off, suspend, shutdown } kind;
};
struct EcBypassApply {
    bool enabled;
}; // Never saved; see Controller::bypass.
struct CoolerFlag {
    bool connected;
}; // EC 0x7C7 bit 1; live until the slot is saved.
struct CliCommand {
    std::vector<std::string> arguments;
};
using ControlCommand =
    std::variant<CpuApply, GpuApply, FanApply, FanBoost, BatteryApply, SwitchApply, ProtectionApply, SlotLoad, SlotSave,
                 SlotRename, SlotClear, DefaultsApply, ThermalReset, ThermalTest, Visibility, Resume, AcEvent,
                 GpuUnlock, DlppcApply, CliCommand, LightingApply, LightingIdle, DimmingApply, PeripheralRead,
                 CoolerRequest, BatteryPage, UiRead, UiPage, UiPreflight, FanMaster, FanEdit, FanRead, PowerTransition,
                 EcBypassApply, CoolerFlag>;
// Status of the current curve/master/link request, not boost/switch, physical cooling or saved state.
enum class FanApplication { unapplied, applied, partial };
struct ControlResult {
    int code = 0;
    std::string text;
    bool applied = false, saved = false, partial = false, slot_committed = false;
    int active_slot = 0;
    FanApplication fan_application = FanApplication::unapplied;
    bool fan_review_required = false;
    bool linked = false;
    std::optional<config::Json> peripherals;
};
config::Curve active_curve(const config::Curve&, int entries);
config::Curve saved_curve(const config::Curve&, const config::FanTable&);
config::Curve linked_curve(int duty, int ramp);
int curve_duty(const config::Curve&, int temperature);

// One instance on ControlWorker. No UI callbacks and no second transport/write path.
class Controller {
public:
    Controller(EcSession&, std::filesystem::path state_directory, std::function<void(const std::string&)> diagnostic,
               std::function<void(unsigned)> wait = {}, std::function<void(std::function<void()>)> run_blocking = {},
               bool nvidia_hardware = true, std::unique_ptr<nv::Power> driver = {}, bool peripheral_hardware = true,
               std::function<void(config::Json)> cooler_delivery = {}, Lighting::Send lighting_send = {});
    ~Controller();
    std::vector<std::string> start(); // Returns EC init errors; startup continues with them.
    ControlResult execute(const ControlCommand&);
    std::optional<Sensors> tick(ULONGLONG now = GetTickCount64());
    std::optional<config::Json> take_battery();
    const ThermalProtection& thermal() const { return *thermal_; }
    int active_slot() const { return active_slot_; }
    bool linked() const { return linked_; }
    bool visible() const { return visible_; }
    bool monitor_stopped() const { return monitor_stopped_; }
    const config::FanSettings& fan_intent() const { return fan_intent_; } // Editor curve/master/link fields only.
    FanApplication fan_application() const { return fan_application_; }
    bool fan_review_required() const { return fan_review_required_; }
    config::Json presentation(bool read = false, bool gpu_live = false, int page = -1);
    int ui_page() const { return ui_page_; }
    bool gpu_presentation() const {
        return visible_ && ui_page_ == 0 && enabled("gpuPower");
    } // The Power tab's live table.
private:
    bool enabled(const char*) const;
    uint8_t read(const char*);
    std::optional<int> temperature(const char*);
    config::CpuSettings cpu(std::string* preset = nullptr);
    config::GpuSettings gpu(bool driver_read = false);
    bool cpu_minimum_ = false, gpu_minimum_ = false, fan_boost_ = false;
    config::CpuSettings minimum_cpu();
    void emergency(int);
    bool matches(int);
    bool initialize(std::vector<std::string>& errors);
    void seed_fans();
    bool curves_supported() const;
    bool recover(const char* reason);
    void recover_settings();
    void batch(const std::vector<EcWrite>&);
    void log_cpu(const std::vector<EcWrite>&);
    void peripheral_io(const std::function<void()>&);
    void dimming(bool);
    void explicit_apply(const std::function<void()>&);
    nv::ProfilePlan plan(const config::UserProfile&, bool automatic);
    void load_slot(int, bool, bool preserve_protection = false);
    void apply_profile(const config::UserProfile&, const nv::ProfilePlan&);
    config::UserProfile capture(int slot);
    void fans(const config::FanSettings&);
    void link(std::optional<int>, std::optional<int>);
    void boost(bool);
    void battery(int);
    void speed(bool, int);
    std::string defaults(const std::string&);
    cli::Reply cli(const std::vector<std::string>&);
    config::Json status();
    config::Json cpu_status(), gpu_status(bool presentation = false), fans_status(), gpu_telemetry(), gpu_budget();
    void apply_slot(const config::UserProfile&, const nv::ProfilePlan&, int, bool, bool);
    bool gpu_extend() const;
    int gpu_min() const;
    int gpu_max() const;
    int gpu_unlock_min() const;
    int gpu_unlock_max() const;
    nv::Power& open_nvidia();
    void nvidia_work(const std::function<void()>&);
    std::string nvidia_unavailable() const;
    nv::ProfilePlan plan_gpu(const config::GpuSettings&, bool automatic);
    std::vector<EcWrite> gpu_plan(const config::GpuSettings&);
    void configure_gpu(const nv::ProfilePlan&);
    void apply_gpu(config::GpuSettings);
    void validate_currents(const config::GpuSettings&);
    void check_gpu();
    void unlock_gpu(bool);
    void dlppc(bool);
    void bypass(bool);
    bool clear_bypass(const std::string& reason);
    void watch_bypass();
    void cooler_flag(bool connected);
    void ensure_cooler_flag(const char* when);
    config::Json advanced_status() const;
    void open_dlppc();
    std::optional<nv::Dlppc> read_dlppc();
    void write_dlppc(bool active);
    std::unique_ptr<nv::Power> nvidia_;
    std::unique_ptr<nv::Api>
        dlppc_api_; // DLPPC needs only NVAPI; used while nvidia_ is unavailable (unsupported BIOS limits).
    bool nvidia_available_ = false, nvidia_busy_ = false, dlppc_available_ = false, dlppc_disabled_ = false,
         on_ac_ = false;
    bool gpu_unlocked_ = false, gpu_driver_uncertain_ = false;
    int nvidia_min_watts_ = 1;
    std::string nvidia_error_, gpu_notice_;
    std::optional<int> driver_watts_;
    std::optional<config::GpuSettings> last_gpu_;
    ULONGLONG gpu_check_at_ = 0;
    bool bypass_supported_ = false, bypass_active_ = false, bypass_clearing_ = false, emergency_ = false,
         cooler_flag_ = false;
    std::string bypass_unavailable_, bypass_status_, bypass_reason_, gpu_unprotected_;
    unsigned advanced_revision_ = 0, presented_advanced_ = ~0u;
    std::function<void(std::function<void()>)> run_blocking_; // Existing joined wait path; never moves EC work.
    EcSession& session_;
    config::ProfileStore store_;
    std::function<void(const std::string&)> diagnostic_;
    std::function<void(unsigned)> wait_;
    std::unique_ptr<Lighting> lighting_;
    std::unique_ptr<Cooler> cooler_;
    std::function<void(config::Json)> cooler_delivery_;
    Lighting::Send lighting_send_;
    bool peripheral_busy_ = false, battery_page_ = false;
    ULONGLONG battery_at_ = 0, budget_at_ = 0;
    std::optional<config::Json> battery_sample_;
    bool peripheral_hardware_ = true, dimming_available_ = false;
    std::unique_ptr<ThermalProtection> thermal_;
    config::FanSettings fan_intent_;
    unsigned fan_revision_ = 0, presented_fan_ = ~0u;
    std::optional<config::UserProfile> saved_profile_;
    std::optional<config::Curve> written_cpu_, written_gpu_; // Last complete write, never hardware readback confidence.
    unsigned written_revision_ = 0, presented_written_ = ~0u;
    FanApplication fan_application_ = FanApplication::unapplied;
    bool fan_review_required_ = false;
    int active_slot_ = 0, epoch_ = 0, last_duty_ = -1, last_cpu_ramp_ = -1, last_gpu_ramp_ = -1;
    bool linked_ = false, visible_ = false, recovering_ = false, reapply_ = false;
    bool writes_started_ = false, monitor_stopped_ = false;
    bool application_completed_ = false;
    int ui_page_ = 0;
    unsigned telemetry_failures_ = 0;
    ULONGLONG watchdog_ = 0, resume_at_ = 0, restart_at_ = 0;
    uint8_t latch_mask_ = 255;
    std::string linked_error_;
    config::Json linked_status_; // The latest linked duty, temperatures and safety ramps, for the Fan Control tab.
};
}
