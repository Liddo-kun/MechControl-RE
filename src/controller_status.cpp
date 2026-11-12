#include "controller.h"
#include "config_text.h"
#include <algorithm>

// What the UI and mcctl status show: owned state and live readings as JSON.
namespace mech {
using namespace config;
static Json setting(int v, const Limit& l) { return {{"value", v}, {"min", l.min}, {"max", l.max}}; }
Json Controller::presentation(bool read, bool gpu_live, int page) {
    auto& p = session_.profile;
    Json j = {{"activeSlot", active_slot_},
              {"warning", store_.warning()},
              {"monitorStopped", monitor_stopped_},
              {"fanApplication", int(fan_application_)},
              {"fanReview", fan_review_required_},
              {"linked", linked_},
              {"protection", thermal_->settings()},
              {"tripped", thermal_->tripped()},
              {"complete", thermal_->complete()},
              {"thermalLostAlerts", thermal_->lost_alerts()},
              {"thermalStatus", thermal_->status()},
              {"temperature", nullable(thermal_->temperature())},
              {"average", nullable(thermal_->average())},
              {"peak", nullable(thermal_->peak_temperature())},
              {"dlppcDisabled", dlppc_disabled_}};
    if (read) j["readPage"] = page;
    if (read && page < 0) {
        j["names"] = store_.settings().names;
        j["saved"] = nullable(saved_profile_);
        j["slots"] = Json::array();
        for (int i = 1; i <= 5; ++i) j["slots"].push_back(store_.exists(i));
    }
    if (read && page == -2) {
        j["model"] = p.project_id;
        j["enabled"] = session_.validation.enabled();
        j["disabled"] = session_.validation.disabled;
        j["curvesSupported"] = curves_supported();
        j["fanFormat"] = p.fan_table.format;
        j["hasGpuFan"] = p.fan_table.has_gpu;
        j["linkAvailable"] = curves_supported() && p.fan_table.has_gpu && p.fan_table.entries >= 10;
        j["thermalActions"] = thermal_->supported_actions();
        j["thermalReason"] = gpu_unprotected_;
        j["actionLimits"] = {{"gpu", features_.gpu_power() ? limits_.tgp.min : 0},
                             {"pl1", cpu_minimum_ ? limits_.pl1.min : 0},
                             {"pl2", cpu_minimum_ ? limits_.pl2.min : 0},
                             {"pl4", cpu_minimum_ ? limits_.pl4.min : 0}};
        j["dutyBaseCpu"] = hex(p.fan_table.cpu_duty);
        j["dutyBaseGpu"] = hex(p.fan_table.gpu_duty);
        if (features_.battery()) j["batteryRegister"] = p.reg("batteryMode");
        for (std::string key : {"cpuTemp", "gpuTemp"})
            if (p.registers.contains(key)) j[key + "Register"] = p.reg(key);
        j["modes"] = Json::array();
        if (p.modes)
            for (auto& [key, value] : *p.modes) {
                (void)value;
                j["modes"].push_back(key);
            }
        j["identity"] = {{"baseboard", session_.identity.baseboard}, {"cpu", session_.identity.cpu_name}};
    }
    // Owned state goes out whole every time: the worker's mailbox keeps only the newest presentation.
    j["fan"] = fan_intent_;
    j["advanced"] = advanced_status();
    if (read && (page == 2 || page == -2)) j["thermal"] = thermal_->state();
    if (page == 1 && linked_) {
        j["writtenCpu"] = nullable(written_cpu_);
        j["writtenGpu"] = nullable(written_gpu_);
        j["linkedStatus"] = linked_status_;
    }
    // Independent groups never erase a successful sibling read or manufacture zero intent.
    if (read && page == 0) {
        try {
            j["cpu"] = cpu_status();
        } catch (const std::exception& e) {
            j["cpuError"] = e.what();
        }
        try {
            j["gpu"] = gpu_status(true);
        } catch (const std::exception& e) {
            j["gpuError"] = e.what();
        }
    }
    if (read && page == 1 && features_.fan_control()) try {
            j["boost"] = (this->read("fanControlMode") & p.bitfields.at("fanBoost").get_mask()) != 0;
            auto speed = encoding::decode_switch_speed(this->read("fanSwitchSpeed"));
            j["switchEnabled"] = speed.first;
            j["switchMs"] = speed.second;
        } catch (const std::exception& e) {
            j["fanError"] = e.what();
        }
    if (read && page == 3 && features_.battery()) try {
            j["batteryMode"] = encoding::decode_battery(this->read("batteryMode"));
        } catch (const std::exception& e) {
            j["batteryError"] = e.what();
        }
    if (gpu_live && page == 0) j["gpuLive"] = gpu_telemetry();
    if (page == 6 && visible_ && (read || GetTickCount64() >= budget_at_)) {
        budget_at_ = GetTickCount64() + 5000;
        j["gpuBudget"] = gpu_budget();
    }
    return j;
}
static Json maybe(std::optional<uint32_t> value) { return value ? Json(*value) : Json(nullptr); }
// Driver readings shared by the Power tab's editor read and its twice-a-second live refresh.
static void live_fields(Json& g, const nv::Telemetry& t) {
    auto mhz = [&](std::optional<uint32_t> khz) { return khz ? Json(*khz / 1000) : Json(nullptr); };
    g["currentCapMhz"] = mhz(t.current_khz);
    g["powerCapMhz"] = mhz(t.power_khz);
    g["maxClockMhz"] = mhz(t.ceiling_khz);
    g["clockMhz"] = maybe(t.clock_mhz);
    g["gpuPowerMw"] = maybe(t.gpu_mw);
    g["corePowerMw"] = maybe(t.core_mw);
    if (t.ipc) g["boardInputLimiterActivity"] = *t.ipc;
}
// The live refresh reads driver telemetry only, like the source: no EC, limit, budget or DLPPC reads.
Json Controller::gpu_telemetry() {
    if (!features_.gpu_power()) return nullptr;
    Json g = {{"nvidia", nvidia_unavailable()}};
    if (nvidia_busy_) {
        g["nvidia"] = "NVIDIA operation in progress";
        return g;
    }
    if (gpu_driver_uncertain_) {
        g["nvidia"] = nvidia_error_;
        return g;
    }
    if (!nvidia_) return g;
    if (!nvidia_->awake()) {
        g["nvidia"] = "GPU asleep, not read";
        return g;
    }
    g["nvidia"] = nullptr;
    try {
        nv::Telemetry t;
        nvidia_work([&] { t = nvidia_->telemetry(); });
        g["nvvdd"] = {{"measuredMa", t.nv}, {"liveLimitMa", maybe(t.nv_limit)}};
        if (t.ms) g["msvdd"] = {{"measuredMa", *t.ms}, {"liveLimitMa", maybe(t.ms_limit)}};
        if (t.input || t.input_limit)
            g["boardInput"] = {{"measuredMa", maybe(t.input)}, {"liveLimitMa", maybe(t.input_limit)}};
        g["enforcedMw"] = maybe(t.enforced_mw);
        live_fields(g, t);
    } catch (const std::exception& e) {
        g["telemetryError"] = e.what();
    }
    thermal_->enforce(true);
    return g;
}
// System Info's driver budgets, at most every 5 s while that tab is shown, like the source.
Json Controller::gpu_budget() {
    if (!features_.gpu_power()) return nullptr;
    auto reason = [](const std::string& text) { return Json{{"reason", text}}; };
    if (nvidia_busy_) return reason("NVIDIA operation in progress");
    if (gpu_driver_uncertain_) return reason(nvidia_error_);
    if (!nvidia_) return reason("NVIDIA not connected");
    if (!nvidia_->awake()) return reason("GPU asleep, not read");
    Json j;
    try {
        nv::Budget b;
        nvidia_work([&] { b = nvidia_->budget(); });
        j = {{"laptopWatts", b.f7 / 1000}, {"platformWatts", b.platform / 1000}};
    } catch (const std::exception& e) {
        j = reason(e.what());
    }
    thermal_->enforce(true);
    return j;
}
Json Controller::cpu_status() {
    if (!features_.cpu_power()) return nullptr;
    std::string preset;
    auto v = cpu(&preset);
    Json j = {{"pl1", setting(v.pl1, limits_.pl1)},
              {"pl2", setting(v.pl2, limits_.pl2)},
              {"pl4", setting(v.pl4, limits_.pl4)},
              {"tempLimit", setting(v.temp_limit, limits_.cpu_temperature)}};
    if (!preset.empty()) j["preset"] = preset; // No custom limits set; see cpu().
    return j;
}
Json Controller::gpu_status(bool presentation) {
    if (!features_.gpu_power()) return nullptr;
    auto& p = session_.profile;
    auto v = gpu();
    auto l = limits_.tgp;
    l.min = gpu_min();
    l.max = gpu_max();
    Json g = {{"tgp", setting(v.tgp, l)},
              {"dynamicBoost", v.dynamic_boost_enabled},
              {"dynamicBoostWatts", setting(v.dynamic_boost_watts, limits_.dynamic_boost)},
              {"unlocked", gpu_unlocked_},
              {"unlockAvailable", gpu_extend() && nvidia_available_},
              {"nvidia", nvidia_unavailable()}};
    if (presentation) {
        g["stockWatts"] = nvidia_ ? nvidia_->stock_watts() : 0;
        g["unlockMin"] = gpu_unlock_min();
        g["unlockMax"] = gpu_unlock_max();
        g["dlppcAvailable"] = dlppc_available_;
        // Withheld by this model's EC data, not by the NVIDIA driver, whose own failures arrive in "nvidia".
        if (nvidia_available_ && !gpu_extend())
            g["unlockReason"] = p.tgp_fallback ? "this model's GPU power range is unknown"
                                               : "this model's GPU power register format is unsupported";
        if (nvidia_ && nvidia_->input_unmatched) g["boardInputUnmatched"] = true;
    }
    for (const char* key :
         {"ceilingWatts", "enforcedWatts", "requestedWatts", "nvvdd", "msvdd", "boardInput", "currentCapMhz",
          "powerCapMhz", "maxClockMhz", "clockMhz", "gpuPowerMw", "corePowerMw", "boardInputLimiterActivity"})
        g[key] = nullptr;
    // These guards precede EVERY driver read, including below-EC-floor readback.
    // gpu() above uses EC plus our retained request, never ReadPowerLimits.
    if (gpu_driver_uncertain_) {
        g["nvidia"] = nvidia_error_;
        return g;
    }
    if (!nvidia_) return g;
    if (!nvidia_->awake()) {
        g["nvidia"] = "GPU asleep, not read";
        return g;
    }
    try {
        std::optional<nv::PowerLimits> power;
        std::optional<nv::Currents> currents;
        std::optional<nv::Telemetry> telemetry;
        g["nvidia"] = nullptr;
        nvidia_work([&] {
            auto attempt = [&](const char* key, const auto& work) {
                try {
                    work();
                } catch (const std::exception& e) {
                    if (presentation)
                        g[key] = e.what();
                    else
                        g["nvidia"] = std::string("NVIDIA readback failed: ") + e.what();
                }
            };
            attempt("powerError", [&] { power = nvidia_->limits(); });
            attempt("currentsError", [&] { currents = nvidia_->currents(); });
            attempt("telemetryError", [&] { telemetry = nvidia_->telemetry(); });
            // DLPPC is independent. Its failure never hides telemetry or currents.
            if (presentation) {
                attempt("dlppcError", [&] {
                    auto d = nvidia_->dlppc();
                    g["dlppc"] = d ? Json{{"active", d->active}, {"policy", d->policy}, {"controllers", d->controllers}}
                                   : Json(nullptr);
                });
            }
        });
        if (power) {
            g["ceilingWatts"] = power->max / 1000;
            g["enforcedWatts"] = power->enforced / 1000;
            if (power->requested) g["requestedWatts"] = *power->requested / 1000;
        }
        if (power && gpu_unlocked_ && v.tgp <= limits_.tgp.min) {
            int floor = limits_.tgp.min;
            int below = power->requested ? int(*power->requested / 1000) - v.boost() : floor;
            g["tgp"]["value"] = std::clamp(below, gpu_min(), floor);
        }
        // The Power tab's live table compares readings with the limits the driver reports alongside them.
        auto rail = [&](nv::Rail r, std::optional<uint32_t> measured, std::optional<uint32_t> predicted,
                        std::optional<uint32_t> live) -> Json {
            Json j = {{"limitMa", r.current},
                      {"minMa", std::max(1000, int(r.min))},
                      {"maxMa", std::min(r.allowed(gpu_unlocked_), int(r.max))},
                      {"stockMa", r.def},
                      {"measuredMa", maybe(measured)},
                      {"atClockCapMa", maybe(predicted)}};
            if (presentation) j["liveLimitMa"] = maybe(live);
            return j;
        };
        nv::Telemetry t = telemetry.value_or(nv::Telemetry{});
        if (currents) {
            auto c = *currents;
            g["nvvdd"] = rail(c.nv, telemetry ? std::optional(t.nv) : std::nullopt, t.nv_prediction, t.nv_limit);
            if (c.ms) g["msvdd"] = rail(*c.ms, t.ms, t.ms_prediction, t.ms_limit);
            if (c.input) g["boardInput"] = rail(*c.input, t.input, {}, t.input_limit);
        }
        if (presentation) g["enforcedMw"] = maybe(t.enforced_mw);
        live_fields(g, t);
    } catch (const std::exception& ex) {
        g["nvidia"] = std::string("NVIDIA readback failed: ") + ex.what();
    }
    thermal_->enforce(true);
    return g;
}
Json Controller::fans_status() {
    auto& p = session_.profile;
    auto s = session_.sample(Detail::temperatures);
    Json boost_value = nullptr;
    if (features_.fan_control()) boost_value = (read("fanControlMode") & p.bitfields.at("fanBoost").get_mask()) != 0;
    return {{"boost", boost_value},
            {"cpuRpm", s.cpu_rpm},
            {"gpuRpm", s.gpu_rpm},
            {"cpuDutyPercent", s.cpu_percent.value_or(0)},
            {"gpuDutyPercent", s.gpu_percent.value_or(0)},
            {"cpuTempC", s.cpu_temp.value_or(0)},
            {"gpuTempC", s.gpu_temp.value_or(0)}};
}
// Owned state only, no EC reads: the bypass and the live liquid cooler flag.
Json Controller::advanced_status() const {
    return {{"ecBypass",
             {{"available", bypass_supported_},
              {"reason", bypass_unavailable_},
              {"active", bypass_active_},
              {"clearing", bypass_clearing_},
              {"status", bypass_status_}}},
            {"coolerFlag", {{"available", session_.hardware.liquid_cooling()}, {"connected", cooler_flag_}}}};
}
Json Controller::status() {
    Json value = {
        {"model", session_.profile.project_id}, {"cpu", cpu_status()}, {"gpu", gpu_status()}, {"fans", fans_status()}};
    value["failsafe"] = {{"tripped", thermal_->tripped()}, {"state", thermal_->status()}};
    value.update(advanced_status());
    return value;
}
}
