#include "controller.h"
#include "config_text.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <thread>
#include <typeinfo>

namespace mech {
using namespace config;
static constexpr int fixed_temps[]{19, 29, 39, 49, 57, 65, 73, 81, 97, 101};
static int ramp_start(int t) {
    return static_cast<int>(std::upper_bound(std::begin(fixed_temps), std::end(fixed_temps), t + 4) -
                            std::begin(fixed_temps));
}
static int nearest(float value) { return static_cast<int>(std::nearbyint(value)); } // Midpoints go to the even number.
Curve active_curve(const Curve& input, int entries) {
    Curve c;
    int previous = -1;
    for (size_t i = 0; i < input.temp_up.size(); ++i) {
        int t = input.temp_up[i];
        if (t == 0 || t >= 254 || t <= previous) break;
        if (i >= input.duty.size()) throw std::runtime_error("Fan duty array is incomplete");
        c.temp_up.push_back(t);
        c.duty.push_back(i ? input.duty[i] / 2 * 2 : 0);
        previous = t;
    }
    if (c.temp_up.size() > static_cast<size_t>(std::max(0, entries)))
        throw std::runtime_error("Fan curve has more active points than the model table");
    if (c.temp_up.empty()) throw std::runtime_error("Fan curve has no active points");
    for (size_t i = 0; i < c.temp_up.size(); ++i) {
        if (c.duty[i] < 0 || c.duty[i] > 200) throw std::runtime_error("Fan duty outside 0-200");
        if (i && (c.temp_up[i] - c.temp_up[i - 1] < 4 || c.duty[i] < c.duty[i - 1]))
            throw std::runtime_error("Fan points require 4 C spacing and monotonic duty");
        c.temp_down.push_back(i ? std::max(0, c.temp_up[i - 1] - 4) : 0);
    }
    return c;
}
Curve saved_curve(const Curve& input, const FanTable& ft) {
    auto c = active_curve(input, ft.entries);
    auto last = c.duty.back();
    while (c.temp_up.size() < static_cast<size_t>(ft.entries)) {
        c.temp_up.push_back(ft.padding_temp);
        c.temp_down.push_back(ft.padding_temp);
        c.duty.push_back(last);
    }
    return c;
}
int curve_duty(const Curve& c, int t) {
    if (t <= c.temp_up.front()) return c.duty.front() / 2;
    for (size_t i = 1; i < c.temp_up.size(); ++i)
        if (t <= c.temp_up[i])
            return nearest(c.duty[i - 1] / 2 + static_cast<float>(t - c.temp_up[i - 1]) /
                                                   (c.temp_up[i] - c.temp_up[i - 1]) *
                                                   (c.duty[i] / 2 - c.duty[i - 1] / 2));
    return c.duty.back() / 2;
}
Curve linked_curve(int duty, int ramp) {
    Curve c;
    duty = std::clamp(duty, 1, 100);
    int count = 10 - ramp;
    for (int i = 0; i < 10; ++i) {
        c.temp_up.push_back(fixed_temps[i]);
        c.temp_down.push_back(i ? fixed_temps[i - 1] - 4 : 0);
        int value = i == 0   ? 0
                    : i == 9 ? 100
                    : i < ramp || count <= 0
                        ? duty
                        : nearest(duty + static_cast<float>(i - ramp) / (count - 1) * (100 - duty));
        c.duty.push_back(value * 2);
    }
    return c;
}
Controller::Controller(EcSession& session, std::filesystem::path directory,
                       std::function<void(const std::string&)> diagnostic, std::function<void(unsigned)> wait,
                       std::function<void(std::function<void()>)> run_blocking, bool nvidia_hardware,
                       std::unique_ptr<nv::Power> driver, bool peripheral_hardware,
                       std::function<void(Json)> cooler_delivery, Lighting::Send lighting_send)
    : session_(session), features_(session.validation), limits_(control_limits(session.profile)),
      store_(std::move(directory), gpu_recovery_unavailable(session.profile, session.hardware.gpu_module()).empty(),
             diagnostic),
      diagnostic_(std::move(diagnostic)), wait_(std::move(wait)), run_blocking_(std::move(run_blocking)),
      cooler_delivery_(std::move(cooler_delivery)), lighting_send_(std::move(lighting_send)),
      peripheral_hardware_(peripheral_hardware) {
    if (!wait_) wait_ = [](unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
    if (!run_blocking_) run_blocking_ = [](std::function<void()> work) { work(); };
    nvidia_available_ = dlppc_available_ = nvidia_hardware && features_.gpu_power() && nv::Store::present();
    nvidia_ = std::move(driver);
    if (nvidia_) {
        nvidia_available_ = dlppc_available_ = true;
        gpu_unlocked_ = nvidia_->enabled;
        nvidia_min_watts_ = nvidia_->minimum_watts();
    }
    SYSTEM_POWER_STATUS power{};
    on_ac_ = GetSystemPowerStatus(&power) && power.ACLineStatus == 1;
    const auto& p = session_.profile;
    // A profile fanCurveLatch bitfield qualifies shared mode bits at 0x7C6.
    // Other profiles retain the reference whole-byte contract, not inferred bit semantics.
    if (auto i = p.bitfields.find("fanCurveLatch"); i != p.bitfields.end()) latch_mask_ = i->second.get_mask();
    try {
        if (features_.cpu_power() && !p.cpu_fallback && p.limits.contains("PL1") && p.limits.contains("PL2") &&
            p.limits.contains("PL4")) {
            auto m = minimum_cpu();
            auto w = cpu_writes(p, m);
            // The minimum must be written exactly; a doubled PL4 register cannot hold an odd one.
            cpu_minimum_ = m.pl1 > 0 && m.pl2 > 0 && m.pl4 > 0 && (!p.pl4_doubled() || m.pl4 % 2 == 0);
            for (auto x : w) session_.guard->admit(x.address);
            for (size_t i = 0; i < 3; ++i)
                if (session_.transport().read(w[i].address) == 255) cpu_minimum_ = false;
        }
    } catch (const std::exception& ex) {
        diagnostic_(std::string("CPU emergency capability unavailable: ") + ex.what());
    }
    // The Thermal Failsafe tab shows why the GPU cannot be protected.
    try {
        gpu_unprotected_ = features_.gpu_power() ? gpu_recovery_unavailable(p, session.hardware.gpu_module())
                                                 : "GPU power control is disabled";
        if (gpu_unprotected_.empty() && !(p.limits.contains("TGP") && p.limits.at("TGP").offset))
            gpu_unprotected_ = "this model's GPU power register format is unsupported";
        if (gpu_unprotected_.empty()) {
            auto g = gpu();
            int minimum = limits_.tgp.min;
            auto w = gpu_writes(p, minimum, false, 0);
            gpu_minimum_ = minimum > 0 && g.tgp >= p.tgp_offset() && g.tgp <= limits_.tgp.max + 75 &&
                           g.dynamic_boost_watts <= limits_.dynamic_boost.max && read("tgpDbCtrl") != 255;
            for (auto x : w) session_.guard->admit(x.address);
            if (!gpu_minimum_) gpu_unprotected_ = "the EC's GPU power values are outside this model's range";
        }
    } catch (const std::exception& ex) {
        gpu_unprotected_ = ex.what();
        diagnostic_("GPU emergency capability unavailable: " + gpu_unprotected_);
    }
    try {
        session_.guard->admit(p.reg("fanControlMode"));
        fan_boost_ = features_.fan_control() && read("fanControlMode") != 255;
    } catch (...) {
    }
    int supported = (cpu_minimum_ ? Cpu : 0) | (gpu_minimum_ ? Gpu : 0) | (fan_boost_ ? Fan : 0);
    diagnostic_("[Thermal] Emergency actions: GPU " +
                (gpu_minimum_ ? "TGP " + std::to_string(limits_.tgp.min) + " W, Dynamic Boost off"
                              : "unavailable (" + gpu_unprotected_ + ")") +
                "; CPU " +
                (cpu_minimum_ ? "PL1/PL2/PL4 " + std::to_string(limits_.pl1.min) + "/" +
                                    std::to_string(limits_.pl2.min) + "/" + std::to_string(limits_.pl4.min) + " W"
                              : std::string("unavailable")) +
                "; fan boost " + (fan_boost_ ? "available" : "unavailable") +
                (p.registers.contains("gpuTemp") ? "" : "; no GPU temperature register"));
    thermal_ = std::make_unique<ThermalProtection>(
        store_, supported, [this](int a) { emergency(a); }, [this](int a) { return matches(a); },
        [this] { return temperature("gpuTemp"); }, diagnostic_);
    session_.guard->recover = [this] { return recover("write verify failure"); };
    // The Power tab shows why the bypass is unavailable.
    if (!features_.cpu_power())
        bypass_unavailable_ = "CPU power control is disabled";
    else if (!p.ec_bypass)
        bypass_unavailable_ = "this model's EC firmware has not been checked";
    else
        try {
            for (const char* name : {"ecBypass", "ecBypassLimits", "acStatus", "ecRunState"})
                if (!p.registers.contains(name))
                    throw std::runtime_error(std::string("profile has no ") + name + " register");
            session_.guard->admit(p.reg("ecBypass"));
            auto major = session.hardware.at(0x454), minor = session.hardware.at(0x47D);
            if (!major || !minor) throw std::runtime_error("EC version unread");
            EcVersion live{*major, *minor};
            auto version = std::to_string(live.first) + "." + std::to_string(live.second);
            if (std::ranges::none_of(p.ec_bypass->ec_versions,
                                     [&](const auto& r) { return r.first <= live && live <= r.second; }))
                throw std::runtime_error("EC version " + version + " has not been checked");
            bypass_supported_ = true;
            diagnostic_("EC power-limit bypass available (EC version " + version + ")");
        } catch (const std::exception& ex) {
            bypass_unavailable_ = ex.what();
        }
    if (!bypass_supported_) diagnostic_("EC power-limit bypass unavailable: " + bypass_unavailable_);
}
Controller::~Controller() { session_.guard->recover = {}; } // Leave linked crash-safe tails and all EC state unchanged.
bool Controller::curves_supported() const {
    return features_.fan_control() && session_.profile.fan_table.format == "RamFan1";
}
void Controller::seed_fans() {
    if (!curves_supported()) return;
    const auto& p = session_.profile;
    const auto& s = session_.fans;
    fan_intent_.curve = s.custom_mode(latch_mask_);
    try {
        if (s.valid()) {
            fan_intent_.cpu_curve = active_curve(s.cpu, p.fan_table.entries);
            if (p.fan_table.has_gpu) fan_intent_.gpu_curve = active_curve(s.gpu, p.fan_table.entries);
            diagnostic_("Fan seed: EC snapshot");
            return;
        }
        if (p.default_fan && p.default_fan->cpu && p.default_fan->gpu) {
            fan_intent_.cpu_curve = active_curve(*p.default_fan->cpu, p.fan_table.entries);
            fan_intent_.gpu_curve = active_curve(*p.default_fan->gpu, p.fan_table.entries);
            diagnostic_("Fan seed: model defaults (EC snapshot invalid)");
            return;
        }
    } catch (const std::exception& ex) {
        diagnostic_(std::string("Fan seed unavailable: ") + ex.what());
    }
    // Reference editable starter, not a model-qualified automatic recovery table.
    Curve generic{{30, 40, 50, 60, 65, 70, 75, 80, 85, 90}, {}, {0, 50, 70, 90, 110, 130, 150, 170, 190, 200}};
    for (size_t i = 0; i < generic.temp_up.size(); ++i) generic.temp_down.push_back(i ? generic.temp_up[i - 1] - 4 : 0);
    fan_intent_.cpu_curve = generic;
    fan_intent_.gpu_curve = generic;
    fan_review_required_ = true;
    diagnostic_("Fan seed: generic editable starter; review before Apply, never used for automatic recovery");
}
uint8_t Controller::read(const char* name) { return session_.transport().read(session_.profile.reg(name)); }
std::optional<int> Controller::temperature(const char* name) {
    try {
        return read(name);
    } catch (...) {
        return {};
    }
}
// A PL register at 0 means no custom limit: the EC keeps one of its presets, and no known register says which.
// The Gaming preset (else the first) stands in, so Apply and Save never use 0 or the slider minimum.
CpuSettings Controller::cpu(std::string* preset) {
    auto& p = session_.profile;
    CpuSettings c{
        read("PL1"), read("PL2"), encoding::decode_pl4(read("PL4"), p.pl4_doubled()),
        encoding::decode_cpu_temperature(read("cpuTempLimitRead"), session_.profile.cpu_direct(), p.tj_max())};
    if ((!c.pl1 || !c.pl2 || !c.pl4) && p.modes && !p.modes->empty()) {
        auto mode = p.modes->find("gaming");
        if (mode == p.modes->end()) mode = p.modes->begin();
        bool filled = false;
        auto fill = [&](int& v, std::optional<int> d, const char* key) {
            if (!v && d && *d) {
                auto l = control_limit(p, key);
                v = std::clamp(*d, l.min, l.max);
                filled = true;
            }
        };
        fill(c.pl1, mode->second.pl1, "PL1");
        fill(c.pl2, mode->second.pl2, "PL2");
        fill(c.pl4, mode->second.pl4, "PL4");
        if (filled && preset) *preset = mode->first;
    }
    return c;
}
GpuSettings Controller::gpu(bool driver_read) {
    GpuSettings g;
    auto ctrl = read("tgpDbCtrl");
    g.tgp = read("tgp") + session_.profile.tgp_offset();
    g.dynamic_boost_watts = read("dbMaxTgp");
    g.dynamic_boost_enabled = (ctrl & 2) != 0;
    // Passive reads never contact NVIDIA, even at the EC floor. Retain our
    // lower request while asleep; only report it when the EC still matches it.
    if (g.tgp == limits_.tgp.min && gpu_unlocked_ && driver_watts_) {
        int below = *driver_watts_ - g.boost();
        if (below < g.tgp) g.tgp = std::max(below, gpu_min());
    }
    if (driver_read && gpu_unlocked_ && g.tgp <= limits_.tgp.min) {
        nv::PowerLimits power;
        nvidia_work([&] { power = nvidia_->limits(); });
        int floor = limits_.tgp.min;
        int below = power.requested ? int(*power.requested / 1000) - g.boost() : floor;
        g.tgp = std::clamp(below, gpu_min(), floor);
    }
    return g;
}
CpuSettings Controller::minimum_cpu() {
    auto& p = session_.profile;
    auto t = read("cpuTempLimitRead");
    int limit = encoding::decode_cpu_temperature(t, session_.profile.cpu_direct(), p.tj_max());
    const auto& bounds = limits_.cpu_temperature;
    if (t == 255 || limit < bounds.min)
        throw std::runtime_error("Current CPU temperature limit invalid; no fallback replacement");
    // Firmware presets can sit above the ECSpec maximum (one Turbo
    // preset sets 103 C against 95 C); lower those to the maximum.
    return {limits_.pl1.min, limits_.pl2.min, limits_.pl4.min, std::min(limit, bounds.max)};
}
void Controller::batch(const std::vector<EcWrite>& writes) {
    for (auto w : writes) session_.guard->admit(w.address);
    if (bypass_clearing_ && !emergency_)
        throw std::runtime_error(
            "The EC power-limit bypass could not be cleared; other EC writes are refused until it is");
    int epoch = epoch_;
    writes_started_ = true;
    session_.guard->apply(writes);
    if (epoch != epoch_)
        throw std::runtime_error("EC reactivated during application; complete group must be reapplied");
}
void Controller::log_cpu(const std::vector<EcWrite>& w) { // PL1, PL2, PL4, temperature limit from cpu_writes.
    auto& p = session_.profile;
    bool direct = session_.profile.cpu_direct();
    diagnostic_("[CPU Power] PL1=" + hex_text(w[0].value) + "(" + std::to_string(w[0].value) +
                "W) PL2=" + hex_text(w[1].value) + "(" + std::to_string(w[1].value) + "W) PL4=" + hex_text(w[2].value) +
                "(" + std::to_string(encoding::decode_pl4(w[2].value, p.pl4_doubled())) +
                "W effective) Temp=" + hex_text(w[3].value) + "(" +
                std::to_string(encoding::decode_cpu_temperature(w[3].value, direct, p.tj_max())) + "C, " +
                (direct ? std::string("absolute") : "offset, tjMax=" + std::to_string(p.tj_max())) + ")");
}
void Controller::peripheral_io(const std::function<void()>& work) {
    peripheral_busy_ = true;
    try {
        run_blocking_(work);
    } catch (...) {
        peripheral_busy_ = false;
        throw;
    }
    peripheral_busy_ = false;
}
void Controller::dimming(bool value) {
    if (!peripheral_hardware_) throw std::runtime_error("Local dimming hardware unavailable");
    int epoch = epoch_;
    writes_started_ = true;
    peripheral_io([&] { set_local_dimming(value); });
    if (epoch != epoch_) throw std::runtime_error("EC reactivated during local dimming; reapply required");
    if (((session_.transport().read(0x72B) & 2) != 0) != value)
        throw std::runtime_error("Local dimming EC readback mismatch");
    dimming_available_ = true;
}
void Controller::emergency(int action) {
    auto& p = session_.profile;
    clear_bypass("thermal failsafe trip");
    // Emergency reductions proceed even while a failed bypass clear refuses normal writes.
    Raised scope(emergency_);

    if (action == Gpu) {
        if (!gpu_minimum_) throw std::runtime_error("Model-specific GPU minimum unavailable");
        GpuSettings g;
        g.tgp = limits_.tgp.min;
        apply_gpu(g);
    }
    if (action == Cpu) {
        if (!cpu_minimum_) throw std::runtime_error("Model-specific CPU minimum unavailable");
        auto c = minimum_cpu();
        batch(cpu_writes(p, c));
    }
    if (action == Fan) {
        if (!fan_boost_) throw std::runtime_error("Fan boost unavailable");
        batch({fan_boost_write(p, true)});
    }
}
bool Controller::matches(int action) {
    if (action == Gpu && nvidia_busy_) return false;
    auto& p = session_.profile;
    std::vector<EcWrite> w;
    if (action == Gpu) {
        if (!gpu_minimum_) return false;
        w = gpu_writes(p, limits_.tgp.min, false, 0);
    }
    if (action == Cpu) {
        if (!cpu_minimum_) return false;
        auto c = minimum_cpu();
        w = cpu_writes(p, c);
        w.pop_back();
    }
    if (action == Fan) {
        if (!fan_boost_) return false;
        w = {fan_boost_write(p, true)};
    }
    for (auto x : w)
        if (!session_.guard->verifies_same_address(x.address) ||
            (session_.transport().read(x.address) & x.mask) != (x.value & x.mask))
            return false;
    if (action == Gpu && (gpu_unlocked_ || gpu_driver_uncertain_)) {
        bool minimum = false;
        nvidia_work([&] { minimum = nvidia_->at_most(limits_.tgp.min); });
        return minimum;
    }
    return true;
}
bool Controller::initialize(std::vector<std::string>& errors) {
    // Each reference init step runs even after an independent step fails.
    const std::vector<std::vector<EcWrite>> steps = {{{0x709, 5}, {0x708, 1}},
                                                     {{0x741, 5}},
                                                     {{0x748, 129}},
                                                     {{0x726, 128, 128}, {0x727, 64, 64}},
                                                     {{0x7C5, 96, 96}},
                                                     {{0x7C7, uint8_t(cooler_flag_ ? 0x0E : 0x0C)}},
                                                     {{0x706, 65}}};
    std::string log = "--- EC Initialization ---";
    for (const auto& step : steps) {
        std::string label, detail;
        for (const auto& w : step) {
            auto address = hex_text(w.address, 3);
            label += (label.empty() ? "" : ", ") + address;
            detail += (detail.empty() ? "" : ", ") + address;
            if (w.mask == 255) {
                detail += "=" + hex_text(w.value);
                continue;
            }
            try {
                auto v = session_.transport().read(w.address);
                detail += ": " + hex_text(v) + " -> " + hex_text((v & ~w.mask) | (w.value & w.mask));
            } catch (const std::exception&) {
                detail += " |= " + hex_text(w.value);
            }
        }
        try {
            session_.guard->apply(step);
            log += "\n  [OK] " + detail;
        } catch (const std::exception& ex) {
            errors.push_back(label + ": " + ex.what());
            log += "\n  [FAIL] " + detail + ": " + ex.what();
        }
        wait_(150);
    }
    bool acknowledged = false;
    uint8_t state = 0;
    for (unsigned i = 0; i <= 30 && !acknowledged; ++i) {
        state = session_.transport().read(0x741);
        acknowledged = (state & 5) == 1;
        if (!acknowledged && i != 30) wait_(100);
    }
    if (!acknowledged)
        errors.push_back(
            "EC activation not acknowledged (0x741 bit 0 set, bit 2 cleared required); the EC may ignore power limit and fan writes");
    log += "\n  0x741 = " + hex_text(state) +
           (acknowledged ? ": activation acknowledged"
                         : ": activation not acknowledged (bit 0 set, bit 2 cleared required)");
    diagnostic_(log + (errors.empty() ? "\n  All steps completed"
                                      : "\n  Completed with " + std::to_string(errors.size()) + " error(s)"));
    return acknowledged;
}
bool Controller::recover(const char* reason) {
    if (recovering_) return false;
    if (auto state = session_.transport().read(0x741); state & 1)
        return false;
    else
        diagnostic_("EC activation lost (0x741 = " + hex_text(state) + ", detected via " + reason +
                    "); re-running EC init");
    recovering_ = true;
    ++epoch_;
    thermal_->invalidate();
    last_duty_ = -1;
    fan_application_ = FanApplication::unapplied;
    try {
        // Reinitialization restores the custom CPU gates even if this clear failed and keeps retrying.
        clear_bypass("EC reinitialization");
        std::vector<std::string> errors;
        if (!initialize(errors)) throw std::runtime_error(errors.back());
        recovering_ = false;
        reapply_ = true;
        watchdog_ = GetTickCount64() + 30000;
        return true;
    } catch (...) {
        recovering_ = false;
        throw;
    }
}
std::vector<std::string> Controller::start() {
    std::vector<std::string> init_errors;
    recovering_ = true;
    // As in the source, a missing acknowledgement is only a warning here; the watchdog retries it.
    try {
        // The bypass never survives a restart: a value left by a crash is cleared before EC init.
        if (bypass_supported_ && session_.transport().read(session_.profile.reg("ecBypass")) ==
                                     uint8_t(hex(session_.profile.ec_bypass->value))) {
            bypass_active_ = true;
            clear_bypass("left set by a previous run");
        }
        initialize(init_errors);
    } catch (...) {
        recovering_ = false;
        throw;
    }
    recovering_ = false;
    watchdog_ = GetTickCount64() + 30000;
    auto& ft = session_.profile.fan_table;
    int profile_active = ft.active;
    bool profile_gpu = ft.has_gpu;
    session_.snapshot_fans();
    {
        const auto& s = session_.fans;
        auto log =
            describe(s, "Fan Table Dump") + "\n  Custom curve mode: " + (s.custom_mode(latch_mask_) ? "on" : "off");
        if (!s.valid())
            log +=
                "\n  Fan table detection skipped: snapshot is not a valid curve (cold boot or wiped EC); keeping profile defaults activeEntries=" +
                std::to_string(ft.active) + ", hasGpuFan=" + (ft.has_gpu ? "true" : "false");
        else {
            log += "\n  Active entries: " + std::to_string(ft.active) +
                   (ft.active == profile_active ? " (matches profile)"
                                                : " (profile default was " + std::to_string(profile_active) + ")");
            auto gpu = s.gpu_present();
            log += "\n  GPU fan table: " + (gpu ? *gpu ? "present (has curve data)" : "not detected (all 0xFF)"
                                                : std::string("ambiguous; keeping profile default hasGpuFan=") +
                                                      (profile_gpu ? "true" : "false"));
        }
        if (ft.format != "RamFan1")
            log += "\n  Fan table format '" + ft.format + "' is not RamFan1; fan curve editing disabled";
        diagnostic_(log);
    }
    seed_fans();
    thermal_->enforce(); // Persisted obligations outrank optional NVIDIA startup work.
    lighting_ = std::make_unique<Lighting>(
        session_, store_, diagnostic_, peripheral_hardware_, lighting_send_, [this](auto work) { peripheral_io(work); },
        [this](const auto& writes) { batch(writes); });
    if (peripheral_hardware_) {
        peripheral_io([&] { dimming_available_ = local_dimming_available(); });
        diagnostic_(std::string("Local dimming WMI ") + (dimming_available_ ? "available" : "unavailable"));
        if (session_.hardware.liquid_cooling())
            cooler_ = std::make_unique<Cooler>(store_, diagnostic_, Cooler::Resolver{}, cooler_delivery_);
        diagnostic_(cooler_ ? "Liquid cooler support enabled (0x7A4 bit 7 set)"
                            : "Liquid cooler support not shown (0x7A4 bit 7 clear or unread)");
    }
    if (nvidia_available_ && (nv::Store::saved() || store_.settings().disable_dlppc)) {
        try {
            open_nvidia();
        } catch (const std::exception& ex) {
            diagnostic_(std::string("Saved NVIDIA state not applied: ") + ex.what());
        }
        if (nvidia_ && gpu_extend()) {
            const auto& l = limits_.tgp;
            diagnostic_(std::string("GPU TGP unlock: ") + (gpu_unlocked_ ? "ON" : "OFF") + " (normal TGP " +
                        std::to_string(l.min) + "-" + std::to_string(l.max) + " W, unlocked " +
                        std::to_string(gpu_unlock_min()) + "-" + std::to_string(gpu_unlock_max()) + " W)");
        }
        if (store_.settings().disable_dlppc) try {
                open_dlppc();
                nvidia_work([&] { write_dlppc(false); });
                dlppc_disabled_ = true;
            } catch (const std::exception& ex) {
                diagnostic_(std::string("Saved DLPPC setting not applied: ") + ex.what());
            }
    }
    int slot = store_.settings().last_slot;
    std::optional<UserProfile> startup;
    nv::ProfilePlan gpu_plan_value;
    if (slot) try {
            startup = store_.load(slot);
            if (startup) gpu_plan_value = plan(*startup, true);
        } catch (const std::exception& ex) {
            startup.reset();
            diagnostic_(std::string("Startup profile preflight refused: ") + ex.what());
        }
    thermal_->enforce(); // Recovered obligations precede all automatic normal settings.
    if (startup) try {
            apply_slot(*startup, gpu_plan_value, slot, false, false);
        } catch (const std::exception& ex) {
            startup.reset();
            diagnostic_(std::string("Startup profile not applied: ") + ex.what());
        }
    // 0x7C6 skips write verification, so this readback is the only proof that custom mode stuck.
    if (startup && curves_supported()) try {
            auto s = read_fans(session_.transport(), session_.profile.fan_table);
            diagnostic_(describe(s, "Fan Table Dump (post-write)") +
                        "\n  Custom curve mode: " + (s.custom_mode(latch_mask_) ? "on" : "off"));
        } catch (const std::exception& ex) {
            diagnostic_(std::string("Fan table verify-read failed: ") + ex.what());
        }
    if (!slot && features_.fan_control() && !session_.fans.valid()) try {
            batch({{session_.profile.reg("fanCurveEnable"), 0, 128}});
            fan_intent_.curve = false;
            diagnostic_("EC fan table invalid at startup; custom mode disabled without applying seed curves");
        } catch (const std::exception& ex) {
            diagnostic_(std::string("EC fan table invalid at startup; custom mode not disabled: ") + ex.what());
        }
    if (cooler_) cooler_->start(); // Selected-slot intent is installed before auto-connect can write it.
    return init_errors;
}
void Controller::explicit_apply(const std::function<void()>& apply) {
    thermal_->check_before_apply();
    if (thermal_->tripped()) throw std::runtime_error("Thermal failsafe is tripped; Reset or load a profile first");
    try {
        apply();
    } catch (...) {
        thermal_->enforce();
        throw;
    }
}
nv::ProfilePlan Controller::plan(const UserProfile& profile, bool automatic) {
    profile.preflight(session_.profile.project_id);
    thermal_->validate(*profile.protection);
    const auto& p = session_.profile;
    std::vector<EcWrite> writes;
    nv::ProfilePlan gpu_value;
    auto append = [&](std::vector<EcWrite> w) { writes.insert(writes.end(), w.begin(), w.end()); };
    if (features_.cpu_power() && profile.cpu) append(cpu_writes(p, *profile.cpu));
    if (features_.gpu_power() && profile.gpu) {
        gpu_value = plan_gpu(*profile.gpu, automatic);
        auto g = gpu_value.settings;
        const auto& l = limits_.tgp;
        append(gpu_writes(p, std::max(g.tgp, l.min), g.dynamic_boost_enabled, g.dynamic_boost_watts,
                          gpu_value.unlock && gpu_extend() ? gpu_unlock_max() : l.max));
    }
    if (features_.fan_control() && profile.fan) {
        if (curves_supported()) {
            auto c = active_curve(*profile.fan->cpu_curve, p.fan_table.entries);
            std::optional<Curve> g;
            if (p.fan_table.has_gpu) g = active_curve(*profile.fan->gpu_curve, p.fan_table.entries);
            append(fan_curve_writes(p, c, g, profile.fan->curve, latch_mask_));
        }
        append({fan_boost_write(p, profile.fan->boost)});
        append(
            {{p.reg("fanSwitchSpeed"), encoding::switch_speed(profile.fan->switch_enabled, profile.fan->switch_ms)}});
    }
    if (features_.battery() && profile.battery) {
        if (profile.battery->mode < 0 || profile.battery->mode > 2)
            throw std::runtime_error("Battery mode must be 0-2");
        append({{p.reg("batteryMode"), encoding::battery(profile.battery->mode, *session_.battery_bit3),
                 p.bitfields.at("batteryMode").get_mask()}});
    }
    for (auto w : writes) session_.guard->admit(w.address);
    return gpu_value;
}
void Controller::apply_profile(const UserProfile& profile, const nv::ProfilePlan& gpu_value) {
    auto& p = session_.profile;
    bool gpu_saved = features_.gpu_power() && profile.gpu, fan_saved = features_.fan_control() && profile.fan;
    if (gpu_saved) configure_gpu(gpu_value); // Driver ceilings/currents precede other feature writes.
    if (fan_saved && curves_supported()) fans(*profile.fan);
    if (features_.cpu_power() && profile.cpu) {
        if (thermal_->holds(Cpu)) {
            if (!thermal_->verified(Cpu) || !matches(Cpu)) emergency(Cpu);
        } else {
            auto w = cpu_writes(p, *profile.cpu);
            log_cpu(w);
            batch(w);
        }
    }
    if (gpu_saved) {
        auto g = gpu_value.settings;
        if (thermal_->holds(Gpu)) {
            if (!thermal_->verified(Gpu) || !matches(Gpu)) emergency(Gpu);
        } else
            apply_gpu(g);
    }
    if (fan_saved) {
        boost(profile.fan->boost);
        speed(profile.fan->switch_enabled, profile.fan->switch_ms);
    }
    if (features_.battery() && profile.battery) battery(profile.battery->mode);
    if (session_.hardware.liquid_cooling() && profile.liquid_cooler_connected != cooler_flag_)
        cooler_flag(profile.liquid_cooler_connected);
    // Peripheral failures are independent and log-only, as in the source.
    if (lighting_) lighting_->apply(*profile.lighting);
    if (peripheral_hardware_) try {
            dimming(profile.lighting->local_dimming);
        } catch (const std::exception& e) {
            diagnostic_(std::string("Slot local dimming failed: ") + e.what());
        }
    if (cooler_ && profile.cooler) cooler_->request({CoolerAction::apply, 0, *profile.cooler, false, false});
}
void Controller::load_slot(int slot, bool deliberate, bool preserve) {
    auto profile = store_.load(slot);
    if (!profile) throw std::runtime_error("Slot is empty");
    auto gpu_value = plan(*profile, !deliberate); // Complete enabled plans before release, curves, or unrelated writes.
    apply_slot(*profile, gpu_value, slot, deliberate, preserve);
}
void Controller::apply_slot(const UserProfile& profile, const nv::ProfilePlan& gpu_value, int slot, bool deliberate,
                            bool preserve) {
    if (deliberate) thermal_->release(); // A failed durable release cannot authorize any normal write.
    if (!preserve) thermal_->configure(*profile.protection);
    active_slot_ = 0;
    saved_profile_.reset();
    thermal_->check_before_apply();
    if (deliberate && thermal_->tripped()) throw std::runtime_error("Failsafe tripped before profile apply");
    try {
        apply_profile(profile, gpu_value);
    } catch (...) {
        thermal_->enforce();
        throw;
    }
    application_completed_ = true;
    active_slot_ = slot;
    saved_profile_ = profile;
    if (deliberate) store_.select(slot); // Hardware can be applied even when this save fails.
    auto unsaved = unsaved_groups(profile);
    diagnostic_("Slot " + std::to_string(slot) +
                " applied; peripheral failures are logged independently; BLE completion is asynchronous" +
                (unsaved.empty() ? "" : "; not saved in this slot, left unchanged: " + unsaved));
}
// The groups a slot does not hold although this run could apply them.
std::string Controller::unsaved_groups(const UserProfile& profile) const {
    std::string text;
    for (auto [missing, name] : {std::pair(features_.cpu_power() && !profile.cpu, "CPU power"),
                                 std::pair(features_.gpu_power() && !profile.gpu, "GPU power"),
                                 std::pair(features_.fan_control() && !profile.fan, "fans"),
                                 std::pair(features_.battery() && !profile.battery, "battery mode")})
        if (missing) text += (text.empty() ? "" : ", ") + std::string(name);
    return text;
}
void Controller::boost(bool value) {
    if (!features_.fan_control()) throw std::runtime_error("Fan control unavailable");
    if (thermal_->holds(Fan)) {
        value = true;
        if (thermal_->verified(Fan) && matches(Fan)) return;
    }
    batch({fan_boost_write(session_.profile, value)});
}
void Controller::battery(int mode) {
    if (!features_.battery()) throw std::runtime_error("Battery control unavailable");
    if (mode < 0 || mode > 2) throw std::runtime_error("Battery mode must be 0-2");
    auto& p = session_.profile;
    batch({{p.reg("batteryMode"), encoding::battery(mode, *session_.battery_bit3),
            p.bitfields.at("batteryMode").get_mask()}});
}
void Controller::speed(bool enabled_value, int ms) {
    if (!features_.fan_control()) throw std::runtime_error("Fan control unavailable");
    batch({{session_.profile.reg("fanSwitchSpeed"), encoding::switch_speed(enabled_value, ms)}});
}
// EC power-limit bypass: the verified firmware skips its adaptive CPU-limit reduction and publishes the
// existing custom PL1/PL2/PL4 instead, but also runs the fans at full and changes protection outputs. Outside
// its normal running state the EC hangs with it set, so every power transition clears it first.
void Controller::bypass(bool on) {
    if (!on) {
        if (!clear_bypass("turned off"))
            throw std::runtime_error("EC power-limit bypass not cleared: " + bypass_status_);
        return;
    }
    if (!bypass_supported_)
        throw std::runtime_error("EC power-limit bypass is not available for this model and EC version");
    if (bypass_clearing_) throw std::runtime_error("The previous EC power-limit bypass has not been cleared");
    if (bypass_active_) return;
    auto& p = session_.profile;
    auto& ec = session_.transport();
    auto address = p.reg("ecBypass");
    auto value = uint8_t(hex(p.ec_bypass->value));
    if (!(ec.read(p.reg("acStatus")) & 1)) throw std::runtime_error("AC power is not connected");
    if (ec.read(p.reg("ecRunState")) != 1) throw std::runtime_error("The EC is not in its normal running state");
    if (ec.read(p.reg("ecBypassLimits")) != 0 || ec.read(address) != 0)
        throw std::runtime_error("EC bypass registers are not in their normal state");
    if (!(ec.read(0x741) & 1) || !(ec.read(0x727) & 0x40))
        throw std::runtime_error("Custom CPU control is not active (0x741 bit 0, 0x727 bit 6)");
    writes_started_ = true;
    bypass_active_ = true;
    try {
        session_.guard->write(address, value);
        if (ec.read(address) != value) throw std::runtime_error("EC power-limit bypass readback mismatch");
    } catch (...) {
        clear_bypass("turning on failed");
        throw;
    }
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    bypass_status_ = "Active";
    diagnostic_("[EC Bypass] " + hex_text(address, 3) + " = " + hex_text(value) +
                " verified; existing CPU limits apply within about 24 s; automatic sleep blocked");
}
// Returns false while the clear keeps failing; the tick retries it and batch() refuses normal writes.
bool Controller::clear_bypass(const std::string& reason) {
    if (!bypass_active_ && !bypass_clearing_) return true;
    auto address = session_.profile.reg("ecBypass");
    try {
        session_.guard->write(address, 0);
        if (session_.transport().read(address) != 0) throw std::runtime_error("readback mismatch");
    } catch (const std::exception& ex) {
        if (!bypass_clearing_)
            diagnostic_("[EC Bypass] Clearing failed (" + reason + "): " + ex.what() +
                        "; retrying every second, other EC writes refused");
        bypass_clearing_ = true;
        bypass_reason_ = reason;
        bypass_status_ = "NOT CLEARED (" + reason + "): " + ex.what();
        return false;
    }
    bypass_active_ = bypass_clearing_ = false;
    SetThreadExecutionState(ES_CONTINUOUS);
    bypass_status_ = "Off (" + reason + ")";
    diagnostic_("[EC Bypass] Cleared (" + reason + "); " + hex_text(address, 3) + " = 0x00 verified");
    return true;
}
void Controller::watch_bypass() {
    if (bypass_clearing_) {
        clear_bypass(bypass_reason_);
        return;
    }
    if (!bypass_active_) return;
    auto& p = session_.profile;
    auto& ec = session_.transport();
    std::string reason;
    if (thermal_->tripped())
        reason = "thermal failsafe trip";
    else
        try {
            auto current = ec.read(p.reg("ecBypass"));
            if (current == 0) {
                bypass_active_ = false;
                SetThreadExecutionState(ES_CONTINUOUS);
                bypass_status_ = "Off (cleared by the EC)";
                diagnostic_("[EC Bypass] The EC cleared it");
                return;
            }
            if (current != uint8_t(hex(p.ec_bypass->value)))
                reason = "unexpected EC value " + hex_text(current);
            else if (!(ec.read(p.reg("acStatus")) & 1))
                reason = "AC power lost";
            else if (ec.read(p.reg("ecRunState")) != 1)
                reason = "EC left its normal running state";
            else if (!(ec.read(0x741) & 1) || !(ec.read(0x727) & 0x40))
                reason = "custom CPU control lost";
        } catch (const std::exception& ex) {
            reason = std::string("EC read failed: ") + ex.what();
        }
    if (!reason.empty()) clear_bypass(reason);
}
// Host-reported liquid cooler connection (LCSE). Only bit 1 changes; the EC owns the OC level in bits 2-4.
void Controller::cooler_flag(bool connected) {
    if (!session_.hardware.liquid_cooling())
        throw std::runtime_error("The EC does not report liquid cooler support (0x7A4 bit 7)");
    batch({{0x7C7, uint8_t(connected ? 2 : 0), 2}});
    if (((session_.transport().read(0x7C7) & 2) != 0) != connected)
        throw std::runtime_error("Liquid cooler flag readback mismatch (0x7C7 bit 1)");
    cooler_flag_ = connected;
    diagnostic_(std::string("[Cooler flag] 0x7C7 bit 1 ") + (connected ? "set" : "cleared"));
}
// The EC clears the flag on power transitions; EC reinitialization writes it with the init byte.
void Controller::ensure_cooler_flag(const char* when) {
    if (!cooler_flag_) return;
    try {
        if (session_.transport().read(0x7C7) & 2) return;
        diagnostic_(std::string("Liquid cooler flag cleared by the EC; reapplying after ") + when);
        cooler_flag(true);
    } catch (const std::exception& ex) {
        diagnostic_(std::string("Liquid cooler flag not reapplied: ") + ex.what());
    }
}
void Controller::fans(const FanSettings& f) {
    auto& p = session_.profile;
    if (!features_.fan_control()) throw std::runtime_error("Fan control unavailable");
    if (!curves_supported()) throw std::runtime_error("Fan curve format unsupported");
    if (!f.cpu_curve || (p.fan_table.has_gpu && !f.gpu_curve)) throw std::runtime_error("Fan curves are null");
    auto c = active_curve(*f.cpu_curve, p.fan_table.entries);
    std::optional<Curve> g;
    if (p.fan_table.has_gpu) g = active_curve(*f.gpu_curve, p.fan_table.entries);
    auto w =
        fan_curve_writes(p, c, g, f.curve, latch_mask_); // Preflight even linked intent before changing live state.
    for (auto x : w) session_.guard->admit(x.address);
    linked_ = false;
    last_duty_ = -1;
    fan_intent_ = f;
    fan_intent_.cpu_curve = c;
    if (g) fan_intent_.gpu_curve = g;
    fan_review_required_ = false;
    fan_application_ = FanApplication::unapplied;
    // Source single-fan UI has no linked controller; a saved linked flag cannot create one.
    fan_intent_.linked = f.linked && p.fan_table.has_gpu && p.fan_table.entries >= 10;
    if (fan_intent_.linked) {
        link(temperature("cpuTemp"), temperature("gpuTemp"));
        linked_ = true;
    } else {
        try {
            batch(w);
            written_cpu_ = c;
            written_gpu_ = g;
            fan_application_ = FanApplication::applied;
        } catch (...) {
            fan_application_ = FanApplication::partial;
            throw;
        }
    }
}
void Controller::link(std::optional<int> c, std::optional<int> g) {
    if (!c || !g || *c <= 0 || *g <= 0 || *c >= 128 || *g >= 128)
        throw std::runtime_error("Linked temperatures unavailable; retaining EC safety ramps");
    int cpu_duty = curve_duty(*fan_intent_.cpu_curve, *c), gpu_duty = curve_duty(*fan_intent_.gpu_curve, *g),
        duty = std::max(cpu_duty, gpu_duty);
    int cr = ramp_start(*c), gr = ramp_start(*g);
    auto ramp = [](int start) { return start < 10 ? fixed_temps[start] : 0; };
    linked_status_ = {{"duty", duty},  {"cpuTemp", *c},       {"cpuDuty", cpu_duty}, {"cpuRamp", ramp(cr)},
                      {"gpuTemp", *g}, {"gpuDuty", gpu_duty}, {"gpuRamp", ramp(gr)}};
    if (last_duty_ >= 0 && cr <= last_cpu_ramp_ && gr <= last_gpu_ramp_ &&
        (std::abs(duty - last_duty_) < 2 || (duty <= 7 && last_duty_ <= 7)))
        return;
    last_duty_ = -1;
    auto cpu = linked_curve(duty, cr), gpu = linked_curve(duty, gr);
    try {
        batch(fan_curve_writes(session_.profile, cpu, gpu, true, latch_mask_));
        written_cpu_ = std::move(cpu);
        written_gpu_ = std::move(gpu);
        fan_application_ = FanApplication::applied;
    } catch (...) {
        fan_application_ = FanApplication::partial;
        throw;
    }
    last_duty_ = duty;
    last_cpu_ramp_ = cr;
    last_gpu_ramp_ = gr;
}
UserProfile Controller::capture(int slot) {
    if (thermal_->tripped()) throw std::runtime_error("Reset thermal protection before saving normal profile settings");
    if (curves_supported() && fan_review_required_)
        throw std::runtime_error("Review and Apply the editable fallback fan curves before saving a profile");
    UserProfile profile;
    profile.project_id = session_.profile.project_id;
    profile.saved_at = timestamp_now();
    // A group whose feature is unavailable has nothing to read and is saved as null.
    profile.cpu = features_.cpu_power() ? std::optional(cpu()) : std::nullopt;
    profile.gpu = features_.gpu_power() ? std::optional(gpu(true)) : std::nullopt;
    if (profile.gpu && nvidia_ && !nvidia_busy_) {
        if (gpu_driver_uncertain_) throw std::runtime_error(nvidia_error_);
        nv::Currents c;
        nvidia_work([&] { c = nvidia_->currents(); });
        auto& g = *profile.gpu;
        g.nvvdd_ma = c.nv.current;
        if (c.ms) g.msvdd_ma = c.ms->current;
        if (c.input) g.board_input_ma = c.input->current;
        const auto& l = limits_.tgp;
        g.bios_override_enabled =
            gpu_unlocked_ &&
            (g.tgp < l.min || g.tgp > l.max || g.total() > nvidia_->stock_watts() || c.nv.current > c.nv.stock_max ||
             (c.ms && c.ms->current > c.ms->stock_max) || (c.input && c.input->current > c.input->stock_max));
    }
    profile.protection = thermal_->settings();
    profile.fan.reset();
    if (features_.fan_control()) {
        profile.fan = fan_intent_;
        profile.fan->boost = (read("fanControlMode") & session_.profile.bitfields.at("fanBoost").get_mask()) != 0;
        profile.fan->curve =
            (read("fanCurveEnable") & 128) != 0 && (session_.transport().read(0x7C6) & latch_mask_) == 4;
        profile.fan->linked = linked_;
        auto [en, ms] = encoding::decode_switch_speed(read("fanSwitchSpeed"));
        profile.fan->switch_enabled = en;
        profile.fan->switch_ms = ms;
        // A curve this model does not use is saved with no active points, which no load can apply.
        const auto& table = session_.profile.fan_table;
        Curve unused;
        unused.temp_up.assign(table.entries, table.padding_temp);
        unused.temp_down = unused.temp_up;
        unused.duty.assign(unused.temp_up.size(), 0);
        if (!unused.temp_up.empty()) unused.temp_up[0] = unused.temp_down[0] = 0;
        bool curves = curves_supported();
        profile.fan->cpu_curve = curves ? saved_curve(*fan_intent_.cpu_curve, table) : unused;
        profile.fan->gpu_curve = curves && table.has_gpu ? saved_curve(*fan_intent_.gpu_curve, table) : unused;
    }
    profile.battery.reset();
    if (features_.battery()) profile.battery = BatterySettings{encoding::decode_battery(read("batteryMode"))};
    profile.liquid_cooler_connected = cooler_flag_;
    // Unavailable owners must not discard target-slot peripheral intent. An unreadable
    // existing slot refuses capture rather than being silently replaced with defaults.
    if (auto previous = store_.load(slot)) {
        profile.lighting = previous->lighting;
        profile.cooler = previous->cooler;
    } else {
        auto lighting = store_.load_lighting();
        if (!lighting.logo || !lighting.keyboard || !lighting.light_bar)
            throw std::runtime_error("Cannot capture explicit-null saved lighting zones as off defaults");
        if (lighting.logo) profile.lighting->logo = *lighting.logo;
        if (lighting.keyboard) profile.lighting->keyboard = *lighting.keyboard;
        if (lighting.light_bar) profile.lighting->light_bar = *lighting.light_bar;
        profile.lighting->idle_seconds = lighting.idle_seconds;
    }
    if (lighting_ && (peripheral_hardware_ || lighting_send_)) {
        if (!profile.lighting) profile.lighting = LightingSettings{};
        lighting_->capture(*profile.lighting);
    }
    if (peripheral_hardware_) {
        if (!dimming_available_) peripheral_io([&] { dimming_available_ = local_dimming_available(); });
        if (dimming_available_) try {
                if (!profile.lighting) profile.lighting = LightingSettings{};
                profile.lighting->local_dimming = (session_.transport().read(0x72B) & 2) != 0;
            } catch (const std::exception& e) {
                diagnostic_(std::string("Dimming capture failed; target intent retained: ") + e.what());
            }
    }
    if (cooler_) profile.cooler = cooler_->intent(); // Requested settings, never GATT readback.
    if (thermal_->tripped())
        throw std::runtime_error("Thermal protection tripped during capture; normal slot was not overwritten");
    return profile;
}
void Controller::recover_settings() {
    reapply_ = false;
    thermal_->enforce();
    int slot = active_slot_ ? active_slot_ : store_.settings().last_slot;
    if (slot) {
        diagnostic_("Re-applying slot " + std::to_string(slot) + " after EC re-activation");
        load_slot(slot, false, true);
        return;
    }
    if (curves_supported() && !read_fans(session_.transport(), session_.profile.fan_table).valid()) {
        if (!fan_review_required_ && fan_intent_.cpu_curve && !fan_intent_.cpu_curve->temp_up.empty()) {
            fans(fan_intent_);
            diagnostic_("EC fan table invalid after re-activation; rewrote the editor curves");
        } else {
            batch({{session_.profile.reg("fanCurveEnable"), 0, 128}});
            diagnostic_(
                "EC fan table invalid after re-activation and no reviewed curves; custom fan curve mode disabled");
        }
    }
}
void Controller::watchdog(ULONGLONG now) {
    if (now < watchdog_) return;
    watchdog_ = now + 30000;
    try {
        recover("watchdog");
    } catch (const std::exception& ex) {
        diagnostic_(ex.what());
    }
    ensure_cooler_flag("watchdog check");
}
void Controller::sample_thermal(Sensors& shared, bool defer_maintenance) {
    if (!thermal_->monitoring() && !linked_) return;
    shared.gpu_temp = temperature("gpuTemp").value_or(0);
    thermal_->sample(shared.gpu_temp, defer_maintenance);
}
void Controller::poll_battery(ULONGLONG now) {
    if (!visible_ || !battery_page_ || !features_.battery() || now < battery_at_) return;
    battery_at_ = now + 2000;
    try {
        battery_sample_ = battery_telemetry(session_.transport());
    } catch (const std::exception&) {
        // Source telemetry is best effort.
    }
}
std::optional<Sensors> Controller::telemetry(ULONGLONG now, const Sensors& shared) {
    bool subscribed = thermal_->monitoring() || linked_;
    if (monitor_stopped_ && !subscribed) {
        if (now < restart_at_) return {};
        monitor_stopped_ = false;
        telemetry_failures_ = 0;
    }
    try {
        auto detail = !visible_       ? Detail::rpm
                      : ui_page_ == 6 ? Detail::full
                      : ui_page_ == 1 ? Detail::temperatures
                                      : Detail::monitor;
        auto s = read_sensors(session_.transport(), session_.profile, detail, shared);
        telemetry_failures_ = 0;
        monitor_stopped_ = false;
        return s;
    } catch (const std::exception& ex) {
        ++telemetry_failures_;
        if (telemetry_failures_ <= 5 || telemetry_failures_ % 60 == 0)
            diagnostic_(std::string("EC telemetry failed: ") + ex.what());
        if (telemetry_failures_ >= 5 && !subscribed) {
            monitor_stopped_ = true;
            restart_at_ = now + 60000;
        }
        return {};
    }
}
std::optional<Sensors> Controller::tick(ULONGLONG now) {
    watch_bypass();
    if (resume_at_ && now >= resume_at_ && !nvidia_busy_) { // In-flight NVIDIA work cannot verify the GPU hold.
        resume_at_ = 0;
        watchdog_ = now + 30000;
        try {
            if (!recover("resume")) thermal_->enforce();
        } catch (const std::exception& ex) {
            diagnostic_(ex.what());
        }
        ensure_cooler_flag("resume");
    }
    watchdog(now);
    Sensors shared;
    sample_thermal(shared, nvidia_busy_);
    if (lighting_ && !peripheral_busy_) lighting_->tick(now);
    if (reapply_ && !nvidia_busy_ && !peripheral_busy_) try {
            recover_settings();
        } catch (const std::exception& ex) {
            diagnostic_(std::string("Recovery not applied: ") + ex.what());
        }
    if (gpu_check_at_ && now >= gpu_check_at_ && !nvidia_busy_ && !peripheral_busy_) {
        gpu_check_at_ = 0;
        try {
            check_gpu();
        } catch (const std::exception& ex) {
            diagnostic_(std::string("NVIDIA delayed check failed: ") + ex.what());
        }
        thermal_->enforce(true);
    }
    poll_battery(now);
    if (linked_ && !nvidia_busy_) { // Only emergency writes may interleave with exclusive NVIDIA work.
        shared.cpu_temp = temperature("cpuTemp").value_or(0);
        try {
            link(shared.cpu_temp, shared.gpu_temp);
            linked_error_.clear();
        } catch (const std::exception& ex) {
            if (linked_error_.empty()) diagnostic_(ex.what());
            linked_error_ = ex.what();
        }
    }
    return telemetry(now, shared);
}
std::optional<Json> Controller::take_battery() { return std::exchange(battery_sample_, {}); }
ControlResult Controller::execute(const ControlCommand& command) {
    auto handle = [&] { return std::visit([&](const auto& c) { return run(c); }, command); };
    ControlResult result;
    // Lifecycle notifications may pass the exclusive NVIDIA wait. They and page reads skip the shared bookkeeping
    // below, so they do not reset the pending command's partial-application accounting.
    if (command_traits(command).notification)
        result = handle();
    else if (std::holds_alternative<UiRead>(command))
        try {
            result = handle();
        } catch (const std::exception& ex) {
            // A failed tab read must not end the worker, which also runs the thermal failsafe.
            result = {1, std::string("Read failed: ") + ex.what()};
            diagnostic_(result.text);
        }
    else {
        writes_started_ = false;
        application_completed_ = false;
        try {
            result = handle();
            thermal_->enforce(true);
        } catch (const std::exception& ex) {
            result.code = 1;
            result.partial = writes_started_;
            result.text = (writes_started_ ? "Partial application possible: " : "") + std::string(ex.what());
            if (dynamic_cast<const SlotSelectionSaveError*>(&ex)) {
                result.slot_committed = true;
                result.partial = true;
            }
            if (std::holds_alternative<SlotLoad>(command) && application_completed_) result.applied = true;
            if (std::holds_alternative<DefaultsApply>(command))
                result.text = "Defaults not fully completed: " + result.text;
            std::string_view name = std::visit([](const auto& c) { return typeid(c).name(); }, command);
            if (name.starts_with("struct mech::")) name.remove_prefix(13);
            diagnostic_("Controller command not completed (" + std::string(name) + "): " + result.text);
            if (thermal_->tripped() && writes_started_) thermal_->enforce();
        }
    }
    if (result.text.empty())
        result.text = result.applied && thermal_->tripped()
                          ? "Completed with current thermal holds; normal held settings were not applied"
                          : "Completed";
    result.active_slot = active_slot_;
    result.fan_application = fan_application_;
    result.fan_review_required = fan_review_required_;
    result.linked = linked_;
    return result;
}
ControlResult Controller::run(const Visibility& c) {
    visible_ = c.visible;
    battery_at_ = GetTickCount64() + 2000;
    budget_at_ = 0;
    if (c.visible) {
        monitor_stopped_ = false;
        telemetry_failures_ = 0;
    } else
        battery_sample_.reset();
    return {};
}
ControlResult Controller::run(const BatteryPage& c) {
    battery_page_ = c.active;
    battery_at_ = GetTickCount64() + 2000;
    if (!c.active) battery_sample_.reset();
    return {};
}
ControlResult Controller::run(const UiPage& c) {
    ui_page_ = c.page;
    budget_at_ = 0;
    return {};
}
ControlResult Controller::run(const UiRead& c) {
    Json data = Json::object();
    if (c.page == 4) data = *execute(PeripheralRead{}).peripherals;
    if (c.page == 3 && features_.battery()) try {
            data["battery"] = battery_telemetry(session_.transport());
        } catch (const std::exception& e) {
            data["batteryError"] = e.what();
        }
    if (c.page == 5) {
        data["coolerAvailable"] = bool(cooler_);
        if (cooler_) {
            data["cooler"] = cooler_->snapshot();
            if (data["cooler"]["bluetoothAvailable"].is_null()) cooler_->request({CoolerAction::availability});
        }
    }
    data["ui"] = presentation(true, false, c.page);
    return {.peripherals = std::move(data)};
}
ControlResult Controller::run(const AcEvent& c) {
    if (!c.on_ac) clear_bypass("AC power lost");
    if (on_ac_ != c.on_ac) {
        on_ac_ = c.on_ac;
        gpu_check_at_ = GetTickCount64() + 5000;
    }
    return {};
}
ControlResult Controller::run(const PowerTransition& c) {
    if (clear_bypass(c.kind == PowerTransition::display_off ? "display off"
                     : c.kind == PowerTransition::suspend   ? "sleep"
                                                            : "exit or Windows shutdown"))
        return {};
    return {1, bypass_status_};
}
ControlResult Controller::run(const Resume&) {
    clear_bypass("resume");
    thermal_->invalidate();
    ++epoch_;
    fan_application_ = FanApplication::unapplied;
    resume_at_ = GetTickCount64() + 3000;
    watchdog_ = resume_at_;
    gpu_check_at_ = GetTickCount64() + 8000;
    if (lighting_) lighting_->resume(GetTickCount64());
    return {};
}
ControlResult Controller::run(const CpuApply& c) {
    if (!features_.cpu_power()) throw std::runtime_error("CPU control unavailable");
    auto w = cpu_writes(session_.profile, c.settings);
    explicit_apply([&] {
        log_cpu(w);
        batch(w);
    });
    return {.applied = true};
}
ControlResult Controller::run(const GpuApply& c) {
    if (!features_.gpu_power()) throw std::runtime_error("GPU control unavailable");
    std::vector<EcWrite> cpu;
    if (c.cpu) {
        if (!features_.cpu_power()) throw std::runtime_error("CPU control unavailable");
        cpu = cpu_writes(session_.profile, *c.cpu);
    }
    const auto& g = c.settings;
    if (nvidia_busy_) throw std::runtime_error("NVIDIA operation in progress");
    if (c.power) gpu_plan(g);
    // Thermal sampling continues during NVIDIA work; a
    // CPU hold latched there outranks the prepared writes.
    auto cpu_apply = [&] {
        if (!cpu.empty() && !thermal_->holds(Cpu)) {
            log_cpu(cpu);
            batch(cpu);
        }
    };
    if (c.restart_confirmed) {
        auto plan = plan_gpu(g, false);
        explicit_apply([&] {
            configure_gpu(plan);
            cpu_apply();
            if (c.power) apply_gpu(g);
        });
    } else {
        validate_currents(g);
        explicit_apply([&] {
            if (g.nvvdd_ma || g.msvdd_ma || g.board_input_ma)
                nvidia_work([&] { nvidia_->configure(gpu_unlocked_, g, false); });
            cpu_apply();
            if (c.power) apply_gpu(g);
        });
    }
    return {.applied = true};
}
ControlResult Controller::run(const UiPreflight& c) {
    if (c.cpu && features_.cpu_power()) cpu_writes(session_.profile, *c.cpu);
    std::optional<UserProfile> profile;
    if (c.slot) {
        profile = store_.load(c.slot);
        if (!profile) throw std::runtime_error("Requested slot is missing");
    }
    auto g = profile ? profile->gpu : c.gpu;
    if (!profile && !g) throw std::runtime_error("GPU intent unavailable");
    auto value = !features_.gpu_power() ? nv::ProfilePlan{} : profile ? plan(*profile, false) : plan_gpu(*g, false);
    return {.peripherals = Json{{"plan",
                                 {{"restart", value.restart},
                                  {"unlock", value.unlock},
                                  {"wasUnlocked", gpu_unlocked_},
                                  {"slot", c.slot},
                                  {"gpu", nullable(g)},
                                  {"stockWatts", nvidia_ ? nvidia_->stock_watts() : 0}}}}};
}
ControlResult Controller::run(const EcBypassApply& c) {
    if (c.enabled)
        explicit_apply([&] { bypass(true); });
    else
        bypass(false);
    return {.text = c.enabled ? "EC power-limit bypass on (not saved)" : "EC power-limit bypass off", .applied = true};
}
ControlResult Controller::run(const CoolerFlag& c) {
    cooler_flag(c.connected);
    return {.applied = true};
}
ControlResult Controller::run(const FanBoost& c) {
    if (!c.enabled) {
        thermal_->check_before_apply();
        if (thermal_->holds(Fan)) throw std::runtime_error("Fan boost is held by thermal protection");
    }
    boost(c.enabled);
    return {.applied = true};
}
ControlResult Controller::run(const FanMaster& c) {
    if (!features_.fan_control()) throw std::runtime_error("Fan control unavailable");
    auto& p = session_.profile;
    auto mask = p.bitfields.at("fanCurveEnable").get_mask();
    batch({{p.reg("fanCurveEnable"), uint8_t(c.enabled ? mask : 0), mask}});
    fan_intent_.curve = c.enabled;
    return {.applied = true};
}
ControlResult Controller::run(const FanEdit& c) {
    if (!linked_ || !curves_supported()) throw std::runtime_error("Linked editor is not active");
    if (!c.settings.cpu_curve || !c.settings.gpu_curve) throw std::runtime_error("Linked editor curves are null");
    auto entries = session_.profile.fan_table.entries;
    auto cpu = active_curve(*c.settings.cpu_curve, entries), gpu = active_curve(*c.settings.gpu_curve, entries);
    fan_intent_.cpu_curve = std::move(cpu);
    fan_intent_.gpu_curve = std::move(gpu);
    fan_application_ = FanApplication::unapplied;
    return {};
}
ControlResult Controller::run(const FanRead&) {
    if (linked_) throw std::runtime_error("Disable linked fans before reading EC tables");
    if (curves_supported()) {
        auto& table = session_.profile.fan_table;
        auto snapshot = read_fans(session_.transport(), table);
        if (!snapshot.valid()) throw std::runtime_error("Current EC fan table is invalid; editor unchanged");
        auto cpu = active_curve(snapshot.cpu, table.entries);
        std::optional<Curve> gpu;
        if (table.has_gpu) gpu = active_curve(snapshot.gpu, table.entries);
        fan_intent_.cpu_curve = std::move(cpu);
        if (gpu) fan_intent_.gpu_curve = std::move(gpu);
        fan_intent_.curve = snapshot.custom_mode(latch_mask_);
    }
    return {.peripherals = Json{{"ui", presentation(true, false, 1)}}};
}
ControlResult Controller::run(const FanApply& c) {
    fans(c.settings);
    return {.applied = true};
}
ControlResult Controller::run(const BatteryApply& c) {
    battery(c.mode);
    return {.applied = true};
}
ControlResult Controller::run(const SwitchApply& c) {
    speed(c.enabled, c.milliseconds);
    return {.applied = true};
}
ControlResult Controller::run(const LightingApply& c) {
    if (!lighting_) throw std::runtime_error("Lighting owner unavailable");
    if (c.zone == 3 && !features_.lighting())
        throw std::runtime_error("Logo control is disabled: " + session_.validation.disabled.at("lighting"));
    if (!lighting_->available(c.zone)) throw std::runtime_error("HID lighting zone unavailable");
    writes_started_ = true;
    lighting_->change(c.zone, c.settings, c.flash, c.color_only);
    return {.text = "Lighting applied; intent updated, persistence failures are logged", .applied = true};
}
ControlResult Controller::run(const LightingIdle& c) {
    if (!lighting_) throw std::runtime_error("Lighting owner unavailable");
    lighting_->idle(c.seconds);
    store_.save_lighting(lighting_->intent());
    return {.text = "Lighting idle intent updated; persistence failures are logged"};
}
ControlResult Controller::run(const DimmingApply& c) {
    dimming(c.enabled);
    return {.applied = true};
}
ControlResult Controller::run(const PeripheralRead& c) {
    Json j = {{"dimmingAvailable", dimming_available_}, {"coolerAvailable", bool(cooler_)}};
    if (lighting_) {
        j["lightingIntent"] = lighting_->intent();
        j["zones"] = {lighting_->available(0), lighting_->available(2), lighting_->available(3)};
        if (features_.lighting()) try {
                j["logo"] = lighting_->logo();
            } catch (const std::exception& e) {
                j["logoError"] = e.what();
            }
    }
    if (peripheral_hardware_ && !dimming_available_)
        peripheral_io([&] { dimming_available_ = local_dimming_available(); });
    j["dimmingAvailable"] = dimming_available_;
    if (dimming_available_) try {
            j["localDimming"] = (session_.transport().read(0x72B) & 2) != 0;
        } catch (const std::exception& e) {
            j["dimmingError"] = e.what();
        }
    if (cooler_) j["cooler"] = cooler_->snapshot();
    if (c.battery && features_.battery()) try {
            j["battery"] = battery_telemetry(session_.transport());
        } catch (const std::exception& e) {
            j["batteryError"] = e.what();
        }
    return {.peripherals = std::move(j)};
}
ControlResult Controller::run(const CoolerRequest& c) {
    if (!cooler_) throw std::runtime_error("Model/runtime cooler support unavailable");
    cooler_->request(c);
    return {.text = "Cooler request queued; hardware completion is asynchronous"};
}
ControlResult Controller::run(const ProtectionApply& c) {
    thermal_->configure(c.settings);
    thermal_->check_before_apply();
    return {.applied = true};
}
ControlResult Controller::run(const SlotLoad& c) {
    load_slot(c.slot, c.deliberate);
    std::string text = thermal_->tripped()
                           ? "Profile applied with thermal-held substitutions; normal held power intent was not applied"
                           : "Profile applied; peripheral errors are log-only, BLE completion is asynchronous";
    if (!gpu_notice_.empty()) text += "; " + gpu_notice_;
    if (auto unsaved = unsaved_groups(*saved_profile_); !unsaved.empty())
        text += "; not saved in this profile, left unchanged: " + unsaved;
    return {.text = text, .applied = true, .saved = c.deliberate};
}
ControlResult Controller::run(const SlotSave& c) {
    auto& table = session_.profile.fan_table;
    int slot = c.slot ? c.slot : 1;
    auto profile = capture(slot);
    if (c.editor_fan && curves_supported()) {
        if (!c.editor_fan->cpu_curve || (table.has_gpu && !c.editor_fan->gpu_curve))
            throw std::runtime_error("Editor fan curves are null");
        profile.fan->cpu_curve = saved_curve(*c.editor_fan->cpu_curve, table);
        if (table.has_gpu) profile.fan->gpu_curve = saved_curve(*c.editor_fan->gpu_curve, table);
        profile.fan->linked = c.editor_fan->linked && table.has_gpu && table.entries >= 10;
    }
    if (c.editor_cooler && cooler_) profile.cooler = *c.editor_cooler;
    profile.name = store_.settings().names.at(slot - 1);
    store_.save(slot, profile);
    saved_profile_ = profile;
    active_slot_ = slot;
    diagnostic_("Saved settings to slot " + std::to_string(slot));
    return {
        .text =
            "Settings saved; peripheral reads are best effort, failures are logged; unavailable target intent preserved",
        .saved = true,
        .slot_committed = true};
}
ControlResult Controller::run(const SlotRename& c) {
    store_.rename(c.slot, c.name);
    return {.saved = true};
}
ControlResult Controller::run(const SlotClear& c) {
    store_.clear(c.slot);
    if (active_slot_ == c.slot) {
        active_slot_ = 0;
        saved_profile_.reset();
    }
    diagnostic_("Cleared slot " + std::to_string(c.slot));
    return {.saved = true};
}
ControlResult Controller::run(const ThermalReset&) {
    thermal_->reset();
    if (active_slot_) load_slot(active_slot_, false, true);
    return {.applied = true};
}
ControlResult Controller::run(const ThermalTest& c) {
    if (c.expected && Json(*c.expected) != Json(thermal_->settings()))
        throw std::runtime_error("Applied settings changed during confirmation; review the actions and test again");
    thermal_->test();
    if (thermal_->complete()) return {.applied = true};
    return {1, thermal_->status()};
}
// An empty mode restores firmware defaults.
ControlResult Controller::run(const DefaultsApply& request) {
    const auto& key = request.mode;
    auto& p = session_.profile;
    std::vector<EcWrite> w;
    auto append = [&](const std::vector<EcWrite>& more) { w.insert(w.end(), more.begin(), more.end()); };
    std::optional<FanSettings> fan;
    std::optional<GpuSettings> gpu_default;
    bool full_curves = false;
    if (!key.empty()) {
        if (!p.modes || !p.modes->contains(key))
            throw std::runtime_error("Requested defaults unavailable for this model");
        auto m = p.modes->at(key);
        if (features_.cpu_power()) {
            auto c = cpu();
            auto cw = cpu_writes(p, {m.pl1, m.pl2, m.pl4.value_or(c.pl4), m.tcc_offset.value_or(c.temp_limit)});
            log_cpu(cw);
            append(cw);
        }
        if (features_.gpu_power() && m.ctgp) {
            GpuSettings g;
            g.tgp = *m.ctgp;
            g.dynamic_boost_enabled = m.dynamic_boost.value_or(0) > 0;
            g.dynamic_boost_watts = m.dynamic_boost.value_or(0);
            gpu_plan(g);
            gpu_default = g;
        }
        if (curves_supported() && m.fan_curve && m.fan_curve->cpu) {
            auto c = active_curve(*m.fan_curve->cpu, p.fan_table.entries);
            std::optional<Curve> g;
            if (p.fan_table.has_gpu && m.fan_curve->gpu) g = active_curve(*m.fan_curve->gpu, p.fan_table.entries);
            append(fan_curve_writes(p, c, g, true, latch_mask_));
            fan = fan_intent_;
            fan->cpu_curve = c;
            if (g) fan->gpu_curve = g;
            fan->curve = true;
            fan->linked = false;
            full_curves = !p.fan_table.has_gpu || g.has_value();
        }
    } else {
        if (features_.cpu_power()) {
            int temp = limits_.cpu_temperature.max;
            w = {{p.reg("PL1"), 0},
                 {p.reg("PL2"), 0},
                 {p.reg("PL4"), 0},
                 {p.reg("cpuTempLimitWrite"),
                  encoding::cpu_temperature(temp, session_.profile.cpu_direct(), p.tj_max())}};
        }
        if (features_.gpu_power()) {
            auto mask = p.bitfields.at("dynamicBoostEn").get_mask();
            append(
                {{p.reg("tgpDbCtrl"), 0, mask}, {p.reg("tgp"), 0}, {p.reg("dbTotalPower"), 0}, {p.reg("dbMaxTgp"), 0}});
        }
        if (features_.fan_control()) {
            append({fan_boost_write(p, false), {p.reg("fanCurveEnable"), 0, 128}});
            fan = fan_intent_;
            fan->linked = false;
            fan->curve = false;
        }
        if (curves_supported() && p.default_fan && p.default_fan->cpu && p.default_fan->gpu) {
            fan->cpu_curve = active_curve(*p.default_fan->cpu, p.fan_table.entries);
            auto g = active_curve(*p.default_fan->gpu, p.fan_table.entries);
            if (p.fan_table.has_gpu) fan->gpu_curve = g;
            append(fan_curve_writes(p, *fan->cpu_curve, p.fan_table.has_gpu ? std::optional(g) : std::nullopt, false,
                                    latch_mask_));
            full_curves = true;
        }
        if (features_.battery())
            append({{p.reg("batteryMode"), encoding::battery(0, *session_.battery_bit3),
                     p.bitfields.at("batteryMode").get_mask()}});
    }
    for (auto x : w) session_.guard->admit(x.address);
    explicit_apply([&] {
        if (fan) {
            // Stop linked tracking before the first write, including partial failure.
            linked_ = false;
            last_duty_ = -1;
            fan_intent_ = *fan;
            fan_application_ = FanApplication::unapplied;
        }
        try {
            batch(w);
            if (fan && (full_curves || !fan->curve)) fan_application_ = FanApplication::applied;
        } catch (...) {
            if (fan) fan_application_ = FanApplication::partial;
            throw;
        }
        // Disabling custom mode alone does not qualify an editable generic seed.
        if (full_curves) fan_review_required_ = false;
        if (gpu_default) apply_gpu(*gpu_default);
    });
    // Match source ordering: finish the EC restore before attempting NVIDIA.
    // There are no normal EC writes after a potentially slow driver operation.
    std::string driver_failure;
    if (key.empty() && nvidia_) {
        try {
            thermal_->check_before_apply();
            if (thermal_->holds(Gpu))
                throw std::runtime_error("NVIDIA defaults not applied: GPU held by thermal protection");
            if (gpu_unlocked_) {
                nv::ProfilePlan plan;
                plan.restart = true;
                configure_gpu(plan);
            } else {
                nv::Currents c;
                nvidia_work([&] { c = nvidia_->currents(); });
                if (thermal_->holds(Gpu))
                    throw std::runtime_error("NVIDIA defaults not applied: GPU held by thermal protection");
                GpuSettings g;
                g.nvvdd_ma = c.nv.def;
                if (c.ms) g.msvdd_ma = c.ms->def;
                if (c.input) g.board_input_ma = c.input->def;
                nvidia_work([&] { nvidia_->configure(false, g, false); });
            }
            driver_watts_ = {};
            last_gpu_ = {};
        } catch (const std::exception& e) {
            thermal_->enforce();
            driver_failure = e.what();
            diagnostic_("NVIDIA defaults failed independently: " + driver_failure);
        }
        thermal_->enforce();
    }
    bool logo_defaults = key.empty() && peripheral_hardware_ && p.default_logo && features_.lighting();
    if (logo_defaults) {
        // No held CPU/GPU/fan setting is rewritten after the slow NVIDIA wait.
        thermal_->enforce();
        auto logo = *p.default_logo;
        batch({{p.reg("logoTrigger"), 0},
               {p.reg("logoR"), logo.r},
               {p.reg("logoG"), logo.g},
               {p.reg("logoB"), logo.b},
               {p.reg("logoOnOff"), logo.on_off, 0x80}});
    }
    if (!driver_failure.empty())
        throw std::runtime_error("Core defaults completed; NVIDIA restoration failed: " + driver_failure);
    std::string text = "Core defaults applied (unsaved); ";
    if (key.empty()) text += nvidia_ ? "NVIDIA defaults restored; " : "NVIDIA defaults not active; ";
    text += logo_defaults ? "model logo defaults restored; other peripheral intent unchanged"
                          : "peripheral intent unchanged";
    if (thermal_->tripped()) text += "; thermal holds supersede normal defaults";
    if (!curves_supported())
        text += "; unsupported fan curves skipped";
    else if (fan && fan->curve && !full_curves)
        text += "; preset has no GPU curve, GPU table unchanged and full fan intent not applied";
    if (fan_review_required_) text += "; editable fan seed still requires review";
    diagnostic_((key.empty() ? std::string("Firmware") : key) + " defaults: " + text);
    return {.text = text, .applied = true};
}
}
