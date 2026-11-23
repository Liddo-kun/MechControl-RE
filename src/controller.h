#pragma once
#include "ec_session.h"
#include "thermal.h"
#include "protocol.h"
#include "vf_curve.h"
#include "peripherals.h"
#include "ble.h"
#include <algorithm>
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
// Deliberate, confirmed UI action; never exposed by CLI.
struct GpuUnlock {
    bool enabled;
};
struct DlppcApply {
    bool disabled;
};
struct AcEvent {
    bool on_ac;
};
// Curves/master/link; boost and switch have separate commands.
struct FanApply {
    config::FanSettings settings;
};
struct FanBoost {
    bool enabled;
};
// Unsupported table formats still expose the existing master bit.
struct FanMaster {
    bool enabled;
};
// Linked editor updates only; the shared tick decides whether to write.
struct FanEdit {
    config::FanSettings settings;
};
struct FanRead {};
struct BatteryApply {
    int mode;
};
// Percent at which charging stops; 100 is no limit. Kept with the application settings, not in a slot.
struct ChargeLimitApply {
    int percent;
};
// Power on when a power source is attached (EC 0x726 bit 3). Written now and kept with the application settings.
struct AcAutoBootApply {
    bool enabled;
};
// Application setting; the UI owns the keep-awake itself (GpuKeepAwake).
struct GpuKeepAwakeApply {
    bool enabled;
};
// Application setting; the UI runs the daily update check itself (update.h).
struct UpdateCheckApply {
    bool enabled;
};
// Millivolts below stock, stored in the BIOS and not in a slot; see VoltageOffset.
struct VoltageOffsetApply {
    int millivolts;
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
// Bind the existing visible-page two-second telemetry subscription.
struct BatteryPage {
    bool active;
};
// -2: initial metadata; -1: changed catalog; 0..6: explicit page read.
struct UiRead {
    int page = -2;
};
struct UiPreflight {
    std::optional<config::GpuSettings> gpu;
    int slot = 0;
    std::optional<config::CpuSettings> cpu;
};
// The visible tab: Power=0 polls GPU telemetry, System Info=6 every EC sensor.
struct UiPage {
    int page;
};
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
// Empty means firmware defaults.
struct DefaultsApply {
    std::string mode;
};
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
// Never saved; see Controller::bypass.
struct EcBypassApply {
    bool enabled;
};
// EC 0x7C7 bit 1; live until the slot is saved.
struct CoolerFlag {
    bool connected;
};
struct CliCommand {
    std::vector<std::string> arguments;
};
// The V/F Curve tab: reads the driver's equations and curve, and on the tab's first use opens the tuner (vf_tuner.h).
struct VfRead {
    bool open = false;
};
// Writes a V/F curve (vf::apply), or with none the stock equations and zero offsets (vf::restore), then reads as VfRead.
// The tune is the tab's state it was made from, which a profile Save stores beside the curve.
struct VfApply {
    std::optional<nv::vf::Curve> curve;
    config::Json tune;
};
// Removes the V/F curve from the loaded slot and forgets the one this run wrote or found, so that loading the slot
// and a later Save leave the GPU's curve as it is. Writes nothing to the GPU.
struct VfForget {};
// Application setting: false once the V/F Curve tab's intro card was hidden for good.
struct VfIntro {
    bool open;
};
using ControlCommand =
    std::variant<CpuApply, GpuApply, FanApply, FanBoost, BatteryApply, SwitchApply, ProtectionApply, SlotLoad, SlotSave,
                 SlotRename, SlotClear, DefaultsApply, ThermalReset, ThermalTest, Visibility, Resume, AcEvent,
                 GpuUnlock, DlppcApply, CliCommand, LightingApply, LightingIdle, DimmingApply, PeripheralRead,
                 CoolerRequest, BatteryPage, UiRead, UiPage, UiPreflight, FanMaster, FanEdit, FanRead, PowerTransition,
                 EcBypassApply, CoolerFlag, ChargeLimitApply, AcAutoBootApply, GpuKeepAwakeApply, UpdateCheckApply,
                 VoltageOffsetApply, VfRead, VfApply, VfForget, VfIntro>;
// How the worker treats each command. Every command needs an entry: command_traits() fails to compile without one.
struct CommandTraits {
    bool notification = false; // Lifecycle event, run even during the wait for a blocking device operation.
    bool posts = true;         // Its result goes to the UI.
    bool if_written = false;   // Posts and marks tabs only when it wrote; a CLI query or refusal changes nothing.
    unsigned stale = 0;        // Tabs to re-read once it writes; any tab here also refreshes the presentation.
    unsigned fresh = 0;        // Tabs it has just read, when it succeeds.
    bool catalog = false;      // Changes slot names or contents.
    bool present = false;      // Refreshes the presentation even without stale tabs.
};
// Tabs as bits, in UI order.
namespace tab {
inline constexpr unsigned power = 1, fans = 2, failsafe = 4, battery = 8, lighting = 16, all = 0x7f;
}
inline CommandTraits traits(const CpuApply&) { return {.stale = tab::power}; }
inline CommandTraits traits(const GpuApply&) { return {.stale = tab::power}; }
inline CommandTraits traits(const GpuUnlock&) { return {.stale = tab::power}; }
inline CommandTraits traits(const DlppcApply&) { return {.stale = tab::power}; }
inline CommandTraits traits(const EcBypassApply&) { return {.stale = tab::power}; }
inline CommandTraits traits(const CoolerFlag&) { return {.stale = tab::power}; }
inline CommandTraits traits(const UiPreflight&) { return {}; }
inline CommandTraits traits(const FanApply&) { return {.stale = tab::fans}; }
inline CommandTraits traits(const FanMaster&) { return {.stale = tab::fans}; }
inline CommandTraits traits(const FanBoost&) { return {.stale = tab::fans}; }
inline CommandTraits traits(const SwitchApply&) { return {.stale = tab::fans}; }
inline CommandTraits traits(const FanEdit&) { return {.posts = false}; }
inline CommandTraits traits(const FanRead&) { return {.fresh = tab::fans}; }
inline CommandTraits traits(const ProtectionApply&) { return {.stale = tab::failsafe}; }
inline CommandTraits traits(const BatteryApply&) { return {.stale = tab::battery}; }
inline CommandTraits traits(const ChargeLimitApply&) { return {.stale = tab::battery}; }
inline CommandTraits traits(const AcAutoBootApply&) { return {.stale = tab::battery}; }
inline CommandTraits traits(const GpuKeepAwakeApply&) { return {.present = true}; }
inline CommandTraits traits(const UpdateCheckApply&) { return {.present = true}; }
inline CommandTraits traits(const VoltageOffsetApply&) { return {.present = true}; }
inline CommandTraits traits(const LightingApply&) { return {.stale = tab::lighting}; }
inline CommandTraits traits(const LightingIdle&) { return {.stale = tab::lighting}; }
inline CommandTraits traits(const DimmingApply&) { return {.stale = tab::lighting}; }
inline CommandTraits traits(const PeripheralRead&) { return {}; }
inline CommandTraits traits(const CoolerRequest&) { return {}; }
inline CommandTraits traits(const UiRead& c) { return {.fresh = c.page >= 0 ? 1u << c.page : 0}; }
inline CommandTraits traits(const SlotLoad&) { return {.stale = tab::all, .catalog = true}; }
inline CommandTraits traits(const SlotSave&) { return {.catalog = true, .present = true}; }
inline CommandTraits traits(const SlotRename&) { return {.catalog = true}; }
inline CommandTraits traits(const SlotClear&) { return {.catalog = true}; }
inline CommandTraits traits(const DefaultsApply&) { return {.stale = tab::all}; }
inline CommandTraits traits(const ThermalReset&) { return {.stale = tab::all}; }
inline CommandTraits traits(const ThermalTest&) { return {.stale = tab::all}; }
inline CommandTraits traits(const CliCommand& c) {
    auto group = std::ranges::find_if(c.arguments, [](const std::string& a) { return a != "--json"; });
    return {.if_written = true, .stale = group != c.arguments.end() && *group == "fan" ? tab::fans : tab::power};
}
inline CommandTraits traits(const Visibility&) { return {.notification = true, .posts = false}; }
inline CommandTraits traits(const UiPage&) { return {.notification = true, .posts = false}; }
inline CommandTraits traits(const BatteryPage&) { return {.notification = true, .posts = false}; }
inline CommandTraits traits(const Resume&) { return {.notification = true, .posts = false}; }
inline CommandTraits traits(const PowerTransition&) { return {.notification = true, .posts = false}; }
inline CommandTraits traits(const AcEvent&) { return {.notification = true}; }
inline CommandTraits traits(const VfRead&) { return {}; }
inline CommandTraits traits(const VfApply&) { return {}; }
inline CommandTraits traits(const VfForget&) { return {.catalog = true}; }
inline CommandTraits traits(const VfIntro&) { return {.posts = false}; }
inline CommandTraits command_traits(const ControlCommand& command) {
    return std::visit([](const auto& c) { return traits(c); }, command);
}
namespace nv::vf {
class Tuner;
}
// What the V/F Curve tab reads: the tuner when the read opened it, the GPU the driver's equations belong to, its live
// equation records and curve points, how many equations and offsets a VfApply wrote and whether it wrote the memory
// offset, and the tune of the curve this run last wrote (from the tab or a profile; null when none).
struct VfReading {
    std::shared_ptr<const nv::vf::Tuner> tuner;
    nv::vf::Gpu gpu;
    nv::Bytes records;
    nv::vf::Points points;
    int equations = 0, offsets = 0;
    bool memory = false; // the memory offset was written
    bool found = false;  // the tune is that of the curve found on the GPU, which Save stores (Controller::vf_found_)
    config::Json tune;
};
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
    std::shared_ptr<const VfReading> vf;
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
    // The tick for the worker's wait on a blocking device operation; tick() itself never runs inside one.
    std::optional<Sensors> wait_tick(ULONGLONG now = GetTickCount64());
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
    // The Power tab's live table.
    bool gpu_presentation() const { return visible_ && ui_page_ == 0 && features_.gpu_power(); }
    // System Info's smoothed input power. Only while System Info is shown, the worker calls input_power_poll on a
    // timer of its own: it reads 0x449 and 0x701, feeds the filter once per EC update (every 100 ms) and returns the
    // milliseconds until the next call, which keeps the reads just after the EC's updates.
    bool input_power_wanted() const { return visible_ && ui_page_ == 6; }
    unsigned input_power_poll(ULONGLONG now);
    void input_power_stop();

private:
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
    bool watchdog(ULONGLONG now);
    void rewrite_holds();
    void sample_thermal(Sensors& shared, bool defer_maintenance);
    void poll_battery(ULONGLONG now);
    std::optional<Sensors> telemetry(ULONGLONG now, const Sensors& shared);
    void batch(const std::vector<EcWrite>&);
    void log_cpu(const std::vector<EcWrite>&);
    void dimming(bool);
    void explicit_apply(const std::function<void()>&);
    nv::ProfilePlan plan(const config::UserProfile&, bool automatic);
    void load_slot(int, bool, bool preserve_protection = false);
    void apply_profile(const config::UserProfile&, const nv::ProfilePlan&);
    config::UserProfile capture(int slot);
    std::string unsaved_groups(const config::UserProfile&) const;
    void fans(const config::FanSettings&);
    void link(std::optional<int>, std::optional<int>);
    void boost(bool);
    void battery(int);
    void charge_limit(ULONGLONG now);
    void auto_boot(bool);
    void ensure_auto_boot(const char* when);
    std::string charge_unavailable();
    config::Json battery_status();
    void speed(bool, int);
    // One handler per command; execute() adds the shared bookkeeping.
    ControlResult run(const CpuApply&), run(const GpuApply&), run(const FanApply&), run(const FanBoost&),
        run(const BatteryApply&), run(const SwitchApply&), run(const ProtectionApply&), run(const SlotLoad&),
        run(const SlotSave&), run(const SlotRename&), run(const SlotClear&), run(const DefaultsApply&),
        run(const ThermalReset&), run(const ThermalTest&), run(const Visibility&), run(const Resume&),
        run(const AcEvent&), run(const GpuUnlock&), run(const DlppcApply&), run(const CliCommand&),
        run(const LightingApply&), run(const LightingIdle&), run(const DimmingApply&), run(const PeripheralRead&),
        run(const CoolerRequest&), run(const BatteryPage&), run(const UiRead&), run(const UiPage&),
        run(const UiPreflight&), run(const FanMaster&), run(const FanEdit&), run(const FanRead&),
        run(const PowerTransition&), run(const EcBypassApply&), run(const CoolerFlag&), run(const ChargeLimitApply&),
        run(const AcAutoBootApply&), run(const GpuKeepAwakeApply&), run(const UpdateCheckApply&),
        run(const VoltageOffsetApply&), run(const VfRead&), run(const VfApply&), run(const VfForget&), run(const VfIntro&);
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
    void bypass(bool);
    bool clear_bypass(const std::string& reason);
    void watch_bypass();
    void cooler_flag(bool connected);
    void ensure_cooler_flag(const char* when);
    config::Json advanced_status() const;
    void open_api(bool& available, const std::string& unavailable, const char* feature);
    void open_dlppc();
    std::optional<nv::Dlppc> read_dlppc();
    void write_dlppc(bool active);
    bool gpu_awake();
    nv::TempTarget& read_temp_target();
    void apply_temp_target(int celsius);
    config::Json temp_target_status();
    nv::Api& vf_api();
    std::filesystem::path vf_directory() const;
    std::shared_ptr<VfReading> vf_read(nv::Api&);
    void apply_vf(const nv::vf::Curve&, const char* what);
    bool vf_overclock();
    std::unique_ptr<nv::Power> nvidia_;
    // DLPPC and the temperature target need only NVAPI; used while nvidia_ is unavailable (an older GPU or
    // unsupported BIOS limits).
    std::unique_ptr<nv::Api> api_;
    bool nvidia_available_ = false, nvidia_busy_ = false, dlppc_available_ = false, dlppc_disabled_ = false,
         on_ac_ = false;
    // The temperature target works on any NVIDIA GPU whose driver has a client thermal policy.
    bool temp_target_available_ = false;
    bool vf_available_ = false;          // Any NVIDIA GPU; the driver decides on first use.
    std::filesystem::path vf_directory_; // Stock V/F records; empty: vf::store_directory().
    // The V/F curve this run last wrote, from the tab or a slot (stock is a curve without records), and its tune. A
    // slot Save stores them; the delayed GPU check writes the curve again after a GPU restart, which clears it.
    std::optional<nv::vf::Curve> vf_curve_;
    config::Json vf_tune_;
    // Without one, the curve found on the GPU when the tab opened (another tool's offsets or equations), with vf_tune_
    // made from its offsets; Save stores it, but nothing writes it.
    std::optional<nv::vf::Curve> vf_found_;
    // Offsets that no curve of this run wrote, looked for once by the first GPU status: the status bar's notice.
    bool vf_looked_ = false, vf_seen_ = false;
    bool vf_foreign_ = false; // a slot's curve replaced another tool's offsets: the status bar's warning
    std::optional<nv::TempTarget> temp_target_; // As last read or written.
    std::optional<int> temp_target_request_;    // What this run applied; the delayed GPU check restores it.
    bool nvidia_offered_ = false; // Fixed at startup; nvidia_available_ also goes false when the driver refuses.
    bool gpu_unlocked_ = false, gpu_driver_uncertain_ = false;
    int nvidia_min_watts_ = 1;
    std::string nvidia_error_, gpu_notice_;
    std::optional<int> driver_watts_;
    std::optional<config::GpuSettings> last_gpu_;
    ULONGLONG gpu_check_at_ = 0;
    bool bypass_supported_ = false, bypass_active_ = false, bypass_clearing_ = false, emergency_ = false,
         cooler_flag_ = false;
    std::string bypass_unavailable_, bypass_status_, bypass_reason_, gpu_unprotected_;
    VoltageOffset voltage_{.unavailable = "not read"}; // What the BIOS held when last read or written.
    std::string voltage_note_;
    std::function<void(std::function<void()>)> run_blocking_; // Existing joined wait path; never moves EC work.
    EcSession& session_;
    const config::Validation& features_;
    const ControlLimits limits_;
    config::ProfileStore store_;
    std::function<void(const std::string&)> diagnostic_;
    std::function<void(unsigned)> wait_;
    std::unique_ptr<Lighting> lighting_;
    std::unique_ptr<Cooler> cooler_;
    std::function<void(config::Json)> cooler_delivery_;
    Lighting::Send lighting_send_;
    // See Controller::charge_limit. Unsupported lasts until the app restarts.
    enum class Hold { unknown, probing, confirmed, unsupported } hold_ = Hold::unknown;
    ULONGLONG charge_at_ = 0;
    int probe_checks_ = 0;
    uint8_t probe_restore_ = 0;
    std::string charge_reason_, charge_note_, power_off_;
    bool battery_page_ = false;
    ULONGLONG battery_at_ = 0, budget_at_ = 0;
    std::optional<config::Json> battery_sample_;
    bool peripheral_hardware_ = true, dimming_available_ = false;
    std::unique_ptr<ThermalProtection> thermal_;
    config::FanSettings fan_intent_;
    std::optional<config::UserProfile> saved_profile_;
    std::optional<config::Curve> written_cpu_, written_gpu_; // Last complete write, never hardware readback confidence.
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
    InputPowerFilter input_power_;
    std::optional<std::array<uint8_t, 2>> input_power_bytes_; // 0x449 and 0x701 at the last EC update.
    std::optional<double> input_volts_;                       // 0x448 from the latest System Info sample.
    ULONGLONG input_power_at_ = 0;
};
}
