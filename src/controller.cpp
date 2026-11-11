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
static int round_even(float n) { // C# Math.Round(float promoted to double), midpoint-to-even.
    int v = static_cast<int>(std::floor(n));
    float f = n - v;
    return v + (f > 0.5f || (f == 0.5f && (v & 1)));
}
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
            return round_even(c.duty[i - 1] / 2 + static_cast<float>(t - c.temp_up[i - 1]) /
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
                        : round_even(duty + static_cast<float>(i - ramp) / (count - 1) * (100 - duty));
        c.duty.push_back(value * 2);
    }
    return c;
}
Controller::Controller(EcSession& session, std::filesystem::path directory,
                       std::function<void(const std::string&)> diagnostic, std::function<void(unsigned)> wait,
                       std::function<void(std::function<void()>)> run_blocking, bool nvidia_hardware,
                       std::unique_ptr<nv::Power> driver, bool peripheral_hardware,
                       std::function<void(Json)> cooler_delivery, Lighting::Send lighting_send)
    : session_(session),
      store_(std::move(directory), gpu_recovery_unavailable(session.profile, session.hardware.gpu_module()).empty(),
             diagnostic),
      diagnostic_(std::move(diagnostic)), wait_(std::move(wait)), run_blocking_(std::move(run_blocking)),
      cooler_delivery_(std::move(cooler_delivery)), lighting_send_(std::move(lighting_send)),
      peripheral_hardware_(peripheral_hardware) {
    if (!wait_) wait_ = [](unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
    if (!run_blocking_) run_blocking_ = [](std::function<void()> work) { work(); };
    nvidia_available_ = dlppc_available_ = nvidia_hardware && enabled("gpuPower") && nv::Store::present();
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
        if (enabled("cpuPower") && !p.cpu_fallback && p.limits.contains("PL1") && p.limits.contains("PL2") &&
            p.limits.contains("PL4")) {
            auto m = minimum_cpu();
            auto w = cpu_writes(p, m.pl1, m.pl2, m.pl4, m.temp_limit);
            cpu_minimum_ = m.pl1 > 0 && m.pl2 > 0 && m.pl4 > 0 &&
                           encoding::decode_pl4(encoding::pl4(m.pl4, p.pl4_doubled()), p.pl4_doubled()) == m.pl4;
            for (auto x : w) session_.guard->admit(x.address);
            for (size_t i = 0; i < 3; ++i)
                if (session_.transport().read(w[i].address) == 255) cpu_minimum_ = false;
        }
    } catch (const std::exception& ex) {
        diagnostic_(std::string("CPU emergency capability unavailable: ") + ex.what());
    }
    // The Thermal Failsafe tab shows why the GPU cannot be protected.
    try {
        gpu_unprotected_ = enabled("gpuPower") ? gpu_recovery_unavailable(p, session.hardware.gpu_module())
                                               : "GPU power control is disabled";
        if (gpu_unprotected_.empty() && !(p.limits.contains("TGP") && p.limits.at("TGP").offset))
            gpu_unprotected_ = "this model's GPU power register format is unsupported";
        if (gpu_unprotected_.empty()) {
            auto g = gpu();
            int minimum = control_limit(p, "TGP").min;
            auto w = gpu_writes(p, minimum, false, 0);
            gpu_minimum_ = minimum > 0 && g.tgp >= p.tgp_offset() && g.tgp <= control_limit(p, "TGP").max + 75 &&
                           g.dynamic_boost_watts <= control_limit(p, "dynamicBoost").max && read("tgpDbCtrl") != 255;
            for (auto x : w) session_.guard->admit(x.address);
            if (!gpu_minimum_) gpu_unprotected_ = "the EC's GPU power values are outside this model's range";
        }
    } catch (const std::exception& ex) {
        gpu_unprotected_ = ex.what();
        diagnostic_("GPU emergency capability unavailable: " + gpu_unprotected_);
    }
    try {
        session_.guard->admit(p.reg("fanControlMode"));
        fan_boost_ = enabled("fanControl") && read("fanControlMode") != 255;
    } catch (...) {
    }
    int supported = (cpu_minimum_ ? Cpu : 0) | (gpu_minimum_ ? Gpu : 0) | (fan_boost_ ? Fan : 0);
    diagnostic_("[Thermal] Emergency actions: GPU " +
                (gpu_minimum_ ? "TGP " + std::to_string(control_limit(p, "TGP").min) + " W, Dynamic Boost off"
                              : "unavailable (" + gpu_unprotected_ + ")") +
                "; CPU " +
                (cpu_minimum_ ? "PL1/PL2/PL4 " + std::to_string(control_limit(p, "PL1").min) + "/" +
                                    std::to_string(control_limit(p, "PL2").min) + "/" +
                                    std::to_string(control_limit(p, "PL4").min) + " W"
                              : std::string("unavailable")) +
                "; fan boost " + (fan_boost_ ? "available" : "unavailable") +
                (p.registers.contains("gpuTemp") ? "" : "; no GPU temperature register"));
    thermal_ = std::make_unique<ThermalProtection>(
        store_, supported, [this](int a) { emergency(a); }, [this](int a) { return matches(a); },
        [this] { return temperature("gpuTemp"); }, diagnostic_);
    session_.guard->recover = [this] { return recover("write verify failure"); };
    // The Power tab shows why the bypass is unavailable.
    if (!enabled("cpuPower"))
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
bool Controller::enabled(const char* feature) const { return session_.validation.enabled.contains(feature); }
bool Controller::curves_supported() const {
    return enabled("fanControl") && session_.profile.fan_table.format == "RamFan1";
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
    CpuSettings c{read("PL1"), read("PL2"), encoding::decode_pl4(read("PL4"), p.pl4_doubled()),
                  encoding::decode_cpu_temperature(read("cpuTempLimitRead"), p.cpu_encoding() == "direct", p.tj_max())};
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
    read("dbTotalPower");
    g.dynamic_boost_watts = read("dbMaxTgp");
    g.dynamic_boost_enabled = (ctrl & 2) != 0;
    // Passive reads never contact NVIDIA, even at the EC floor. Retain our
    // lower request while asleep; only report it when the EC still matches it.
    if (g.tgp == control_limit(session_.profile, "TGP").min && gpu_unlocked_ && driver_watts_) {
        int below = *driver_watts_ - (g.dynamic_boost_enabled ? g.dynamic_boost_watts : 0);
        if (below < g.tgp) g.tgp = std::max(below, gpu_min());
    }
    if (driver_read && gpu_unlocked_ && g.tgp <= control_limit(session_.profile, "TGP").min) {
        nv::PowerLimits power;
        nvidia_work([&] { power = nvidia_->limits(); });
        int floor = control_limit(session_.profile, "TGP").min;
        int below = power.requested
                        ? int(*power.requested / 1000) - (g.dynamic_boost_enabled ? g.dynamic_boost_watts : 0)
                        : floor;
        g.tgp = std::clamp(below, gpu_min(), floor);
    }
    return g;
}
CpuSettings Controller::minimum_cpu() {
    auto& p = session_.profile;
    auto t = read("cpuTempLimitRead");
    int limit = encoding::decode_cpu_temperature(t, p.cpu_encoding() == "direct", p.tj_max());
    auto bounds = control_limit(p, "cpuTempLimit");
    if (t == 255 || limit < bounds.min)
        throw std::runtime_error("Current CPU temperature limit invalid; no fallback replacement");
    // Firmware presets can sit above the ECSpec maximum (one Turbo
    // preset sets 103 C against 95 C); lower those to the maximum.
    return {control_limit(p, "PL1").min, control_limit(p, "PL2").min, control_limit(p, "PL4").min,
            std::min(limit, bounds.max)};
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
    bool direct = p.cpu_encoding() == "direct";
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
    struct Scope {
        bool& value;
        Scope(bool& v) : value(v) { value = true; }
        ~Scope() { value = false; }
    } scope(emergency_);
    if (action == Gpu) {
        if (!gpu_minimum_) throw std::runtime_error("Model-specific GPU minimum unavailable");
        GpuSettings g;
        g.tgp = control_limit(p, "TGP").min;
        apply_gpu(g);
    }
    if (action == Cpu) {
        if (!cpu_minimum_) throw std::runtime_error("Model-specific CPU minimum unavailable");
        auto c = minimum_cpu();
        batch(cpu_writes(p, c.pl1, c.pl2, c.pl4, c.temp_limit));
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
        w = gpu_writes(p, control_limit(p, "TGP").min, false, 0);
    }
    if (action == Cpu) {
        if (!cpu_minimum_) return false;
        auto c = minimum_cpu();
        w = cpu_writes(p, c.pl1, c.pl2, c.pl4, c.temp_limit);
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
        nvidia_work([&] { minimum = nvidia_->at_most(control_limit(p, "TGP").min); });
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
            auto l = control_limit(session_.profile, "TGP");
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
    if (!slot && enabled("fanControl") && !session_.fans.valid()) try {
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
    if (enabled("cpuPower")) {
        auto c = *profile.cpu;
        append(cpu_writes(p, c.pl1, c.pl2, c.pl4, c.temp_limit));
    }
    if (enabled("gpuPower")) {
        gpu_value = plan_gpu(*profile.gpu, automatic);
        auto g = gpu_value.settings;
        auto l = control_limit(p, "TGP");
        append(gpu_writes(p, std::max(g.tgp, l.min), g.dynamic_boost_enabled, g.dynamic_boost_watts,
                          gpu_value.unlock && gpu_extend() ? gpu_unlock_max() : l.max));
    }
    if (enabled("fanControl")) {
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
    if (enabled("battery")) {
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
    if (enabled("gpuPower")) configure_gpu(gpu_value); // Driver ceilings/currents precede other feature writes.
    if (curves_supported()) fans(*profile.fan);
    if (enabled("cpuPower")) {
        auto c = *profile.cpu;
        if (thermal_->holds(Cpu)) {
            if (!thermal_->verified(Cpu) || !matches(Cpu)) emergency(Cpu);
        } else {
            auto w = cpu_writes(p, c.pl1, c.pl2, c.pl4, c.temp_limit);
            log_cpu(w);
            batch(w);
        }
    }
    if (enabled("gpuPower")) {
        auto g = gpu_value.settings;
        if (thermal_->holds(Gpu)) {
            if (!thermal_->verified(Gpu) || !matches(Gpu)) emergency(Gpu);
        } else
            apply_gpu(g);
    }
    if (enabled("fanControl")) {
        boost(profile.fan->boost);
        speed(profile.fan->switch_enabled, profile.fan->switch_ms);
    }
    if (enabled("battery")) battery(profile.battery->mode);
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
    if (!profile.requires_save) saved_profile_ = profile;
    if (deliberate) store_.select(slot); // Hardware can be applied even when this save fails.
    diagnostic_("Slot " + std::to_string(slot) +
                " applied; peripheral failures are logged independently; BLE completion is asynchronous");
}
void Controller::boost(bool value) {
    if (!enabled("fanControl")) throw std::runtime_error("Fan control unavailable");
    if (thermal_->holds(Fan)) {
        value = true;
        if (thermal_->verified(Fan) && matches(Fan)) return;
    }
    batch({fan_boost_write(session_.profile, value)});
}
void Controller::battery(int mode) {
    if (!enabled("battery")) throw std::runtime_error("Battery control unavailable");
    if (mode < 0 || mode > 2) throw std::runtime_error("Battery mode must be 0-2");
    auto& p = session_.profile;
    batch({{p.reg("batteryMode"), encoding::battery(mode, *session_.battery_bit3),
            p.bitfields.at("batteryMode").get_mask()}});
}
void Controller::speed(bool enabled_value, int ms) {
    if (!enabled("fanControl")) throw std::runtime_error("Fan control unavailable");
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
    ++advanced_revision_;
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
        ++advanced_revision_;
        return false;
    }
    bypass_active_ = bypass_clearing_ = false;
    SetThreadExecutionState(ES_CONTINUOUS);
    bypass_status_ = "Off (" + reason + ")";
    ++advanced_revision_;
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
                ++advanced_revision_;
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
    ++advanced_revision_;
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
    if (!enabled("fanControl")) throw std::runtime_error("Fan control unavailable");
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
    ++fan_revision_;
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
            ++written_revision_;
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
        ++written_revision_;
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
    profile.cpu = enabled("cpuPower") ? cpu() : CpuSettings{45, 65, 90, 95};
    if (enabled("gpuPower"))
        profile.gpu = gpu(true);
    else {
        profile.gpu->tgp = 115;
        profile.gpu->dynamic_boost_enabled = false;
    }
    if (enabled("gpuPower") && nvidia_ && !nvidia_busy_) {
        if (gpu_driver_uncertain_) throw std::runtime_error(nvidia_error_);
        nv::Currents c;
        nvidia_work([&] { c = nvidia_->currents(); });
        auto& g = *profile.gpu;
        g.nvvdd_ma = c.nv.current;
        if (c.ms) g.msvdd_ma = c.ms->current;
        if (c.input) g.board_input_ma = c.input->current;
        auto l = control_limit(session_.profile, "TGP");
        g.bios_override_enabled =
            gpu_unlocked_ && (g.tgp < l.min || g.tgp > l.max ||
                              g.tgp + (g.dynamic_boost_enabled ? g.dynamic_boost_watts : 0) > nvidia_->stock_watts() ||
                              c.nv.current > c.nv.stock_max || (c.ms && c.ms->current > c.ms->stock_max) ||
                              (c.input && c.input->current > c.input->stock_max));
    }
    profile.protection = thermal_->settings();
    profile.fan = enabled("fanControl") ? fan_intent_ : FanSettings{};
    if (enabled("fanControl")) {
        profile.fan->boost = (read("fanControlMode") & session_.profile.bitfields.at("fanBoost").get_mask()) != 0;
        profile.fan->curve =
            (read("fanCurveEnable") & 128) != 0 && (session_.transport().read(0x7C6) & latch_mask_) == 4;
        profile.fan->linked = linked_;
        auto [en, ms] = encoding::decode_switch_speed(read("fanSwitchSpeed"));
        profile.fan->switch_enabled = en;
        profile.fan->switch_ms = ms;
    }
    auto placeholder = [&] {
        Curve c;
        c.temp_up.assign(session_.profile.fan_table.entries, session_.profile.fan_table.padding_temp);
        c.temp_down = c.temp_up;
        c.duty.assign(c.temp_up.size(), 0);
        if (!c.temp_up.empty()) c.temp_up[0] = c.temp_down[0] = 0;
        return c;
    };
    if (curves_supported()) {
        profile.fan->cpu_curve = saved_curve(*fan_intent_.cpu_curve, session_.profile.fan_table);
        profile.fan->gpu_curve = session_.profile.fan_table.has_gpu
                                     ? saved_curve(*fan_intent_.gpu_curve, session_.profile.fan_table)
                                     : placeholder();
    } else {
        profile.fan->cpu_curve = placeholder();
        profile.fan->gpu_curve = placeholder();
    }
    if (enabled("battery")) profile.battery->mode = encoding::decode_battery(read("batteryMode"));
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
    if (now >= watchdog_) {
        watchdog_ = now + 30000;
        try {
            recover("watchdog");
        } catch (const std::exception& ex) {
            diagnostic_(ex.what());
        }
        ensure_cooler_flag("watchdog check");
    }
    Sensors shared;
    bool safety = thermal_->monitoring();
    if (safety || linked_) {
        shared.gpu_temp = temperature("gpuTemp").value_or(0);
        thermal_->sample(shared.gpu_temp, nvidia_busy_);
    }
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
    if (visible_ && battery_page_ && enabled("battery") && now >= battery_at_) {
        battery_at_ = now + 2000;
        try {
            battery_sample_ = battery_telemetry(session_.transport());
        } catch (const std::exception&) {
            // Source telemetry is best effort.
        }
    }
    if (monitor_stopped_ && !thermal_->monitoring() && !linked_) {
        if (now < restart_at_) return {};
        monitor_stopped_ = false;
        telemetry_failures_ = 0;
    }
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
        if (telemetry_failures_ >= 5 && !safety && !linked_) {
            monitor_stopped_ = true;
            restart_at_ = now + 60000;
        }
        return {};
    }
}
std::optional<Json> Controller::take_battery() { return std::exchange(battery_sample_, {}); }
ControlResult Controller::execute(const ControlCommand& command) {
    auto state = [&](ControlResult result = ControlResult{0, "Completed"}) {
        result.active_slot = active_slot_;
        result.fan_application = fan_application_;
        result.fan_review_required = fan_review_required_;
        result.linked = linked_;
        return result;
    };
    // Lifecycle notifications may pass the exclusive NVIDIA wait. They do not
    // reset the pending command's partial-application accounting or call NVIDIA.
    if (auto c = std::get_if<Visibility>(&command)) {
        visible_ = c->visible;
        battery_at_ = GetTickCount64() + 2000;
        budget_at_ = 0;
        if (c->visible) {
            monitor_stopped_ = false;
            telemetry_failures_ = 0;
        } else
            battery_sample_.reset();
        return state();
    }
    if (auto c = std::get_if<BatteryPage>(&command)) {
        battery_page_ = c->active;
        battery_at_ = GetTickCount64() + 2000;
        if (!c->active) battery_sample_.reset();
        return state();
    }
    if (auto c = std::get_if<UiPage>(&command)) {
        ui_page_ = c->page;
        budget_at_ = 0;
        return state();
    }
    if (auto c = std::get_if<UiRead>(&command)) {
        auto r = state();
        Json data = Json::object();
        if (c->page == 4) data = *execute(PeripheralRead{}).peripherals;
        if (c->page == 3 && enabled("battery")) try {
                data["battery"] = battery_telemetry(session_.transport());
            } catch (const std::exception& e) {
                data["batteryError"] = e.what();
            }
        if (c->page == 5) {
            data["coolerAvailable"] = bool(cooler_);
            if (cooler_) {
                data["cooler"] = cooler_->snapshot();
                if (data["cooler"]["bluetoothAvailable"].is_null()) cooler_->request({CoolerAction::availability});
            }
        }
        data["ui"] = presentation(true, false, c->page);
        r.peripherals = std::move(data);
        return r;
    }
    if (auto c = std::get_if<AcEvent>(&command)) {
        if (!c->on_ac) clear_bypass("AC power lost");
        if (on_ac_ != c->on_ac) {
            on_ac_ = c->on_ac;
            gpu_check_at_ = GetTickCount64() + 5000;
        }
        return state();
    }
    if (auto c = std::get_if<PowerTransition>(&command)) {
        auto r = state();
        if (!clear_bypass(c->kind == PowerTransition::display_off ? "display off"
                          : c->kind == PowerTransition::suspend   ? "sleep"
                                                                  : "exit or Windows shutdown")) {
            r.code = 1;
            r.text = bypass_status_;
        }
        return r;
    }
    if (std::holds_alternative<Resume>(command)) {
        clear_bypass("resume");
        thermal_->invalidate();
        ++epoch_;
        fan_application_ = FanApplication::unapplied;
        resume_at_ = GetTickCount64() + 3000;
        watchdog_ = resume_at_;
        gpu_check_at_ = GetTickCount64() + 8000;
        if (lighting_) lighting_->resume(GetTickCount64());
        return state();
    }
    ControlResult result;
    writes_started_ = false;
    application_completed_ = false;
    try {
        std::visit(
            [&](const auto& c) {
                using T = std::decay_t<decltype(c)>;
                auto& p = session_.profile;
                if constexpr (std::is_same_v<T, CpuApply>) {
                    if (!enabled("cpuPower")) throw std::runtime_error("CPU control unavailable");
                    auto s = c.settings;
                    auto w = cpu_writes(p, s.pl1, s.pl2, s.pl4, s.temp_limit);
                    explicit_apply([&] {
                        log_cpu(w);
                        batch(w);
                    });
                    result.applied = true;
                } else if constexpr (std::is_same_v<T, GpuApply>) {
                    if (!enabled("gpuPower")) throw std::runtime_error("GPU control unavailable");
                    std::vector<EcWrite> cpu;
                    if (c.cpu) {
                        if (!enabled("cpuPower")) throw std::runtime_error("CPU control unavailable");
                        auto v = *c.cpu;
                        cpu = cpu_writes(p, v.pl1, v.pl2, v.pl4, v.temp_limit);
                    }
                    auto g = c.settings;
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
                    result.applied = true;
                } else if constexpr (std::is_same_v<T, UiPreflight>) {
                    if (c.cpu && enabled("cpuPower")) {
                        auto v = *c.cpu;
                        cpu_writes(p, v.pl1, v.pl2, v.pl4, v.temp_limit);
                    }
                    std::optional<UserProfile> profile;
                    if (c.slot) {
                        profile = store_.load(c.slot);
                        if (!profile) throw std::runtime_error("Requested slot is missing");
                    }
                    auto g = profile ? profile->gpu : c.gpu;
                    if (!g) throw std::runtime_error("GPU intent unavailable");
                    auto value = enabled("gpuPower") ? (profile ? plan(*profile, false) : plan_gpu(*g, false))
                                                     : nv::ProfilePlan{};
                    result.peripherals = Json{{"plan",
                                               {{"restart", value.restart},
                                                {"unlock", value.unlock},
                                                {"wasUnlocked", gpu_unlocked_},
                                                {"slot", c.slot},
                                                {"gpu", *g},
                                                {"stockWatts", nvidia_ ? nvidia_->stock_watts() : 0}}}};
                } else if constexpr (std::is_same_v<T, GpuUnlock>) {
                    unlock_gpu(c.enabled);
                    result.applied = true;
                } else if constexpr (std::is_same_v<T, DlppcApply>) {
                    dlppc(c.disabled);
                    result.applied = true;
                    result.saved = true;
                } else if constexpr (std::is_same_v<T, EcBypassApply>) {
                    if (c.enabled)
                        explicit_apply([&] { bypass(true); });
                    else
                        bypass(false);
                    result.applied = true;
                    result.text = c.enabled ? "EC power-limit bypass on (not saved)" : "EC power-limit bypass off";
                } else if constexpr (std::is_same_v<T, CoolerFlag>) {
                    cooler_flag(c.connected);
                    result.applied = true;
                } else if constexpr (std::is_same_v<T, FanBoost>) {
                    if (!c.enabled) {
                        thermal_->check_before_apply();
                        if (thermal_->holds(Fan)) throw std::runtime_error("Fan boost is held by thermal protection");
                    }
                    boost(c.enabled);
                    result.applied = true;
                } else if constexpr (std::is_same_v<T, FanMaster>) {
                    if (!enabled("fanControl")) throw std::runtime_error("Fan control unavailable");
                    auto mask = p.bitfields.at("fanCurveEnable").get_mask();
                    batch({{p.reg("fanCurveEnable"), uint8_t(c.enabled ? mask : 0), mask}});
                    fan_intent_.curve = c.enabled;
                    ++fan_revision_;
                    result.applied = true;
                } else if constexpr (std::is_same_v<T, FanEdit>) {
                    if (!linked_ || !curves_supported()) throw std::runtime_error("Linked editor is not active");
                    if (!c.settings.cpu_curve || !c.settings.gpu_curve)
                        throw std::runtime_error("Linked editor curves are null");
                    auto cpu = active_curve(*c.settings.cpu_curve, p.fan_table.entries),
                         gpu = active_curve(*c.settings.gpu_curve, p.fan_table.entries);
                    fan_intent_.cpu_curve = std::move(cpu);
                    fan_intent_.gpu_curve = std::move(gpu);
                    fan_application_ = FanApplication::unapplied;
                    ++fan_revision_;
                } else if constexpr (std::is_same_v<T, FanRead>) {
                    if (linked_) throw std::runtime_error("Disable linked fans before reading EC tables");
                    if (curves_supported()) {
                        auto snapshot = read_fans(session_.transport(), p.fan_table);
                        if (!snapshot.valid())
                            throw std::runtime_error("Current EC fan table is invalid; editor unchanged");
                        auto cpu = active_curve(snapshot.cpu, p.fan_table.entries);
                        std::optional<Curve> gpu;
                        if (p.fan_table.has_gpu) gpu = active_curve(snapshot.gpu, p.fan_table.entries);
                        fan_intent_.cpu_curve = std::move(cpu);
                        if (gpu) fan_intent_.gpu_curve = std::move(gpu);
                        fan_intent_.curve = snapshot.custom_mode(latch_mask_);
                        ++fan_revision_;
                    }
                    result.peripherals = Json{{"ui", presentation(true, false, 1)}};
                } else if constexpr (std::is_same_v<T, FanApply>) {
                    fans(c.settings);
                    result.applied = true;
                } else if constexpr (std::is_same_v<T, BatteryApply>) {
                    battery(c.mode);
                    result.applied = true;
                } else if constexpr (std::is_same_v<T, SwitchApply>) {
                    speed(c.enabled, c.milliseconds);
                    result.applied = true;
                } else if constexpr (std::is_same_v<T, LightingApply>) {
                    if (!lighting_) throw std::runtime_error("Lighting owner unavailable");
                    if (!lighting_->available(c.zone)) throw std::runtime_error("HID lighting zone unavailable");
                    writes_started_ = true;
                    lighting_->change(c.zone, c.settings, c.flash, c.color_only);
                    result.applied = true;
                    result.text = "Lighting applied; intent updated, persistence failures are logged";
                } else if constexpr (std::is_same_v<T, LightingIdle>) {
                    if (!lighting_) throw std::runtime_error("Lighting owner unavailable");
                    lighting_->idle(c.seconds);
                    store_.save_lighting(lighting_->intent());
                    result.text = "Lighting idle intent updated; persistence failures are logged";
                } else if constexpr (std::is_same_v<T, DimmingApply>) {
                    dimming(c.enabled);
                    result.applied = true;
                } else if constexpr (std::is_same_v<T, PeripheralRead>) {
                    Json j = {{"dimmingAvailable", dimming_available_}, {"coolerAvailable", bool(cooler_)}};
                    if (lighting_) {
                        j["lightingIntent"] = lighting_->intent();
                        j["zones"] = {lighting_->available(0), lighting_->available(2), lighting_->available(3)};
                        try {
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
                    if (c.battery && enabled("battery")) try {
                            j["battery"] = battery_telemetry(session_.transport());
                        } catch (const std::exception& e) {
                            j["batteryError"] = e.what();
                        }
                    result.peripherals = std::move(j);
                } else if constexpr (std::is_same_v<T, CoolerRequest>) {
                    if (!cooler_) throw std::runtime_error("Model/runtime cooler support unavailable");
                    cooler_->request(c);
                    result.text = "Cooler request queued; hardware completion is asynchronous";
                } else if constexpr (std::is_same_v<T, ProtectionApply>) {
                    thermal_->configure(c.settings);
                    thermal_->check_before_apply();
                    result.applied = true;
                } else if constexpr (std::is_same_v<T, SlotLoad>) {
                    load_slot(c.slot, c.deliberate);
                    result.applied = true;
                    result.saved = c.deliberate;
                    result.text =
                        thermal_->tripped()
                            ? "Profile applied with thermal-held substitutions; normal held power intent was not applied"
                            : "Profile applied; peripheral errors are log-only, BLE completion is asynchronous";
                    if (!gpu_notice_.empty()) result.text += "; " + gpu_notice_;
                } else if constexpr (std::is_same_v<T, SlotSave>) {
                    int slot = c.slot ? c.slot : 1;
                    auto profile = capture(slot);
                    if (c.editor_fan && curves_supported()) {
                        if (!c.editor_fan->cpu_curve || (p.fan_table.has_gpu && !c.editor_fan->gpu_curve))
                            throw std::runtime_error("Editor fan curves are null");
                        profile.fan->cpu_curve = saved_curve(*c.editor_fan->cpu_curve, p.fan_table);
                        if (p.fan_table.has_gpu)
                            profile.fan->gpu_curve = saved_curve(*c.editor_fan->gpu_curve, p.fan_table);
                        profile.fan->linked = c.editor_fan->linked && p.fan_table.has_gpu && p.fan_table.entries >= 10;
                    }
                    if (c.editor_cooler && cooler_) profile.cooler = *c.editor_cooler;
                    profile.name = store_.settings().names.at(slot - 1);
                    store_.save(slot, profile);
                    saved_profile_ = profile;
                    active_slot_ = slot;
                    result.saved = true;
                    result.slot_committed = true;
                    diagnostic_("Saved settings to slot " + std::to_string(slot));
                    result.text =
                        "Settings saved; peripheral reads are best effort, failures are logged; unavailable target intent preserved";
                } else if constexpr (std::is_same_v<T, SlotRename>) {
                    store_.rename(c.slot, c.name);
                    result.saved = true;
                } else if constexpr (std::is_same_v<T, SlotClear>) {
                    store_.clear(c.slot);
                    if (active_slot_ == c.slot) {
                        active_slot_ = 0;
                        saved_profile_.reset();
                    }
                    result.saved = true;
                    diagnostic_("Cleared slot " + std::to_string(c.slot));
                } else if constexpr (std::is_same_v<T, ThermalReset>) {
                    thermal_->reset();
                    if (active_slot_) load_slot(active_slot_, false, true);
                    result.applied = true;
                } else if constexpr (std::is_same_v<T, ThermalTest>) {
                    if (c.expected && Json(*c.expected) != Json(thermal_->settings()))
                        throw std::runtime_error(
                            "Applied settings changed during confirmation; review the actions and test again");
                    thermal_->test();
                    result.applied = thermal_->complete();
                    if (!result.applied) {
                        result.code = 1;
                        result.text = thermal_->status();
                    }
                } else if constexpr (std::is_same_v<T, DefaultsApply>) {
                    result.text = defaults(c.mode);
                    result.applied = true;
                } else if constexpr (std::is_same_v<T, CliCommand>) {
                    auto reply = cli(c.arguments);
                    result.code = reply.code;
                    result.text = reply.text;
                    result.applied = reply.code == 0 && writes_started_;
                    result.partial = reply.code != 0 && writes_started_;
                }
            },
            command);
        thermal_->enforce(true);
        if (result.text.empty())
            result.text = result.applied && thermal_->tripped()
                              ? "Completed with current thermal holds; normal held settings were not applied"
                              : "Completed";
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
    return state(std::move(result));
}
}
