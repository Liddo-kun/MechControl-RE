#include "controller.h"
#include <algorithm>
#include <charconv>
#include <iomanip>
#include <sstream>

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
        j["enabled"] = session_.validation.enabled;
        j["disabled"] = session_.validation.disabled;
        j["curvesSupported"] = curves_supported();
        j["fanFormat"] = p.fan_table.format;
        j["hasGpuFan"] = p.fan_table.has_gpu;
        j["linkAvailable"] = curves_supported() && p.fan_table.has_gpu && p.fan_table.entries >= 10;
        j["thermalActions"] = thermal_->supported_actions();
        j["thermalReason"] = gpu_unprotected_;
        j["actionLimits"] = {{"gpu", enabled("gpuPower") ? control_limit(p, "TGP").min : 0},
                             {"pl1", cpu_minimum_ ? control_limit(p, "PL1").min : 0},
                             {"pl2", cpu_minimum_ ? control_limit(p, "PL2").min : 0},
                             {"pl4", cpu_minimum_ ? control_limit(p, "PL4").min : 0}};
        j["dutyBaseCpu"] = hex(p.fan_table.cpu_duty);
        j["dutyBaseGpu"] = hex(p.fan_table.gpu_duty);
        if (enabled("battery")) j["batteryRegister"] = p.reg("batteryMode");
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
    if ((read && (page == 1 || page == -2)) || fan_revision_ != presented_fan_) {
        j["fan"] = fan_intent_;
        presented_fan_ = fan_revision_;
    }
    if ((read && (page == 0 || page == -2)) || advanced_revision_ != presented_advanced_) {
        j["advanced"] = advanced_status();
        presented_advanced_ = advanced_revision_;
    }
    if (read && (page == 2 || page == -2)) j["thermal"] = thermal_->state();
    if (page == 1 && linked_ && (read || written_revision_ != presented_written_)) {
        j["writtenCpu"] = nullable(written_cpu_);
        j["writtenGpu"] = nullable(written_gpu_);
        presented_written_ = written_revision_;
    }
    if (page == 1 && linked_) j["linkedStatus"] = linked_status_;
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
    if (read && page == 1 && enabled("fanControl")) try {
            j["boost"] = (this->read("fanControlMode") & p.bitfields.at("fanBoost").get_mask()) != 0;
            auto speed = encoding::decode_switch_speed(this->read("fanSwitchSpeed"));
            j["switchEnabled"] = speed.first;
            j["switchMs"] = speed.second;
        } catch (const std::exception& e) {
            j["fanError"] = e.what();
        }
    if (read && page == 3 && enabled("battery")) try {
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
    if (!enabled("gpuPower")) return nullptr;
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
    if (!enabled("gpuPower")) return nullptr;
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
    if (!enabled("cpuPower")) return nullptr;
    auto& p = session_.profile;
    std::string preset;
    auto v = cpu(&preset);
    Json j = {{"pl1", setting(v.pl1, control_limit(p, "PL1"))},
              {"pl2", setting(v.pl2, control_limit(p, "PL2"))},
              {"pl4", setting(v.pl4, control_limit(p, "PL4"))},
              {"tempLimit", setting(v.temp_limit, control_limit(p, "cpuTempLimit"))}};
    if (!preset.empty()) j["preset"] = preset; // No custom limits set; see cpu().
    return j;
}
Json Controller::gpu_status(bool presentation) {
    if (!enabled("gpuPower")) return nullptr;
    auto& p = session_.profile;
    auto v = gpu();
    auto l = control_limit(p, "TGP");
    l.min = gpu_min();
    l.max = gpu_max();
    Json g = {{"tgp", setting(v.tgp, l)},
              {"dynamicBoost", v.dynamic_boost_enabled},
              {"dynamicBoostWatts", setting(v.dynamic_boost_watts, control_limit(p, "dynamicBoost"))},
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
    // This guard precedes EVERY driver read, including below-EC-floor readback.
    // gpu() above uses EC plus our retained request, never ReadPowerLimits.
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
        if (power && gpu_unlocked_ && v.tgp <= control_limit(p, "TGP").min) {
            int floor = control_limit(p, "TGP").min;
            int below = power->requested
                            ? int(*power->requested / 1000) - (v.dynamic_boost_enabled ? v.dynamic_boost_watts : 0)
                            : floor;
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
    if (enabled("fanControl")) boost_value = (read("fanControlMode") & p.bitfields.at("fanBoost").get_mask()) != 0;
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
static int whole(const std::string& text) {
    int v = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (error != std::errc{} || end != text.data() + text.size() || text.empty() || text[0] == '-' || v < 0)
        throw std::invalid_argument("Expected a nonnegative whole number");
    return v;
}
static int milliamps(std::string text) {
    int scale = 1000;
    auto lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](char c) { return static_cast<char>(tolower(static_cast<unsigned char>(c))); });
    if (lower.ends_with("ma")) {
        text.resize(text.size() - 2);
        scale = 1;
    } else if (lower.ends_with('a'))
        text.pop_back();
    // Decimal arithmetic: accepting a binary float and rounding would lose exact mA.
    auto dot = text.find('.');
    auto integral = dot == 0 ? 0 : whole(text.substr(0, dot));
    int64_t value = static_cast<int64_t>(integral) * scale;
    if (dot != std::string::npos) {
        auto fraction = text.substr(dot + 1);
        int places = scale == 1000 ? 3 : 0;
        for (char c : fraction) {
            if (c < '0' || c > '9') throw std::invalid_argument("Invalid current");
            if (places > 0) {
                scale /= 10;
                value += (c - '0') * scale;
                --places;
            } else if (c != '0')
                throw std::invalid_argument("Current must be exactly representable to 1 mA");
        }
    }
    if (value <= 0 || value > INT_MAX) throw std::invalid_argument("Current must be positive and fit mA");
    return static_cast<int>(value);
}
static std::string text_status(const Json& j) {
    std::ostringstream out;
    auto decimal = [](int64_t value, int precision, bool trim) {
        std::ostringstream s;
        s.imbue(std::locale::classic());
        s << std::fixed << std::setprecision(precision) << (value / 1000.0);
        auto text = s.str();
        if (trim) {
            while (text.ends_with('0')) text.pop_back();
            if (text.ends_with('.')) text.pop_back();
        }
        return text;
    };
    auto amps = [&](const Json& value) { return value.is_null() ? "--" : decimal(value.get<int>(), 3, true) + " A"; };
    auto watts = [&](const Json& value) {
        return value.is_null() ? "--" : decimal(value.get<int64_t>(), 1, false) + " W";
    };
    auto mhz = [](const Json& value) { return value.is_null() ? "--" : std::to_string(value.get<int>()) + " MHz"; };
    auto setting_text = [](const Json& v, const char* unit) {
        return std::to_string(v.at("value").get<int>()) + " " + unit + " (" + std::to_string(v.at("min").get<int>()) +
               "-" + std::to_string(v.at("max").get<int>()) + " " + unit + ")";
    };
    auto advanced = [&] {
        auto& b = j.at("ecBypass");
        auto& f = j.at("coolerFlag");
        auto on = [](bool v) { return v ? "on" : "off"; };
        auto status = b.at("status").get<std::string>();
        out << "EC    power-limit bypass "
            << (!b.at("available").get<bool>() ? "unavailable (" + b.at("reason").get<std::string>() + ")"
                : !status.empty()              ? status
                                               : "Off")
            << "; liquid cooler flag ";
        if (!f.at("available").get<bool>())
            out << "unavailable";
        else
            out << on(f.at("connected").get<bool>());
    };
    if (j.contains("model")) {
        out << "Model " << j.at("model").get<std::string>()
            << "; thermal failsafe: " << j.at("failsafe").at("state").get<std::string>() << "\r\n";
        for (const char* group : {"gpu", "cpu", "fans"})
            out << (j.at(group).is_null()
                        ? std::string(group == std::string("gpu") ? "GPU" : "CPU") + "   power control unavailable"
                        : text_status(j.at(group)))
                << "\r\n";
        advanced();
    } else if (j.contains("ecBypass")) {
        advanced();
    } else if (j.contains("pl1")) {
        out << "CPU   PL1 " << setting_text(j.at("pl1"), "W") << "; PL2 " << setting_text(j.at("pl2"), "W") << "; PL4 "
            << setting_text(j.at("pl4"), "W") << "; temp limit " << setting_text(j.at("tempLimit"), "C");
    } else if (j.contains("tgp")) {
        out << "GPU   TGP " << setting_text(j.at("tgp"), "W") << "; Dynamic Boost "
            << (j.at("dynamicBoost").get<bool>() ? "on" : "off") << ", " << setting_text(j.at("dynamicBoostWatts"), "W")
            << "; unlock "
            << (j.at("unlocked").get<bool>()          ? "on"
                : j.at("unlockAvailable").get<bool>() ? "off"
                                                      : "unavailable");
        if (!j.at("nvidia").is_null())
            out << "\r\n      NVIDIA: " << j.at("nvidia").get<std::string>();
        else {
            out << "\r\n      NVIDIA ceiling " << j.at("ceilingWatts") << " W; enforced " << j.at("enforcedWatts")
                << " W; requested "
                << (j.at("requestedWatts").is_null() ? "none"
                                                     : std::to_string(j.at("requestedWatts").get<int>()) + " W");
            for (auto [key, name] :
                 {std::pair("nvvdd", "NVVDD"), std::pair("msvdd", "MSVDD"), std::pair("boardInput", "Board input")}) {
                const auto& r = j.at(key);
                out << "\r\n      " << name;
                if (r.is_null()) {
                    out << " current limit: not available on this GPU";
                    continue;
                }
                out << " limit " << amps(r.at("limitMa")) << " (" << decimal(r.at("minMa").get<int>(), 3, true) << '-'
                    << amps(r.at("maxMa")) << ", stock " << amps(r.at("stockMa")) << "); measured "
                    << amps(r.at("measuredMa"));
                if (std::string_view(key) != "boardInput") out << "; at clock cap " << amps(r.at("atClockCapMa"));
            }
            out << "\r\n      Clock caps: current controller " << mhz(j.at("currentCapMhz")) << "; power controller "
                << mhz(j.at("powerCapMhz")) << "; max " << mhz(j.at("maxClockMhz"));
            Json other = nullptr;
            if (!j.at("gpuPowerMw").is_null() && !j.at("corePowerMw").is_null())
                other = j.at("gpuPowerMw").get<int64_t>() - j.at("corePowerMw").get<int64_t>();
            out << "\r\n      Clock " << mhz(j.at("clockMhz")) << "; power " << watts(j.at("gpuPowerMw"))
                << " (core rails " << watts(j.at("corePowerMw")) << ", memory and other " << watts(other)
                << "); board input limiter (IPC) active ";
            if (j.at("boardInputLimiterActivity").is_null())
                out << "--";
            else
                out << std::fixed << std::setprecision(1) << j.at("boardInputLimiterActivity").get<double>() * 100
                    << '%';
        }
    } else {
        out << "Fans  boost " << (j.at("boost").is_null() ? "unavailable" : j.at("boost").get<bool>() ? "on" : "off");
        for (const char* prefix : {"cpu", "gpu"}) {
            std::string p = prefix;
            out << "; " << (p == "cpu" ? "CPU" : "GPU") << ' ' << j.at(p + "Rpm").get<int>() << " RPM, "
                << j.at(p + "DutyPercent").get<int>() << "%, " << j.at(p + "TempC").get<int>() << " C";
        }
    }
    return out.str();
}
cli::Reply Controller::cli(const std::vector<std::string>& arguments) {
    std::vector<std::string> args;
    bool json = false;
    for (const auto& a : arguments)
        if (a == "--json")
            json = true;
        else
            args.push_back(a);
    if (args.size() == 1 && args[0] == "status") {
        auto j = status();
        return {0, json ? j.dump() : text_status(j)};
    }
    std::optional<ControlCommand> command;
    std::string group;
    try {
        if (args.empty()) throw std::invalid_argument("Expected status, cpu, gpu, fan boost, ec-bypass or cooler-flag");
        if (args[0] == "fan") {
            if (args.size() != 3 || args[1] != "boost" || (args[2] != "on" && args[2] != "off"))
                throw std::invalid_argument("Usage: fan boost on|off");
            command = FanBoost{args[2] == "on"};
            group = "fans";
        } else if (args[0] == "ec-bypass" || args[0] == "cooler-flag") {
            if (args.size() != 2 || (args[1] != "on" && args[1] != "off"))
                throw std::invalid_argument("Usage: " + args[0] + " on|off");
            if (args[0] == "ec-bypass")
                command = EcBypassApply{args[1] == "on"};
            else
                command = CoolerFlag{args[1] == "on"};
            group = "ec";
        } else if (args[0] == "cpu" || args[0] == "gpu") {
            bool is_cpu = args[0] == "cpu";
            if (args.size() < 3 || args.size() % 2 == 0)
                throw std::invalid_argument("Expected one or more option/value pairs");
            std::map<std::string, std::string> options;
            const std::set<std::string> cpu_options = {"--pl1", "--pl2", "--pl4", "--temp-limit"},
                                        gpu_options = {"--tgp", "--db", "--nvvdd", "--msvdd", "--board-input"};
            const auto& allowed = is_cpu ? cpu_options : gpu_options;
            for (size_t i = 1; i < args.size(); i += 2) {
                if (!allowed.contains(args[i]) || !options.emplace(args[i], args[i + 1]).second)
                    throw std::invalid_argument("Unknown or duplicate option: " + args[i]);
            }
            // Match source availability/read/usage ordering, but preflight all values before writes.
            if (is_cpu) {
                if (!enabled("cpuPower")) return {1, "CPU control unavailable"};
                auto c = cpu();
                for (const auto& [key, value] : options) {
                    int n = whole(value);
                    if (key == "--pl1")
                        c.pl1 = n;
                    else if (key == "--pl2")
                        c.pl2 = n;
                    else if (key == "--pl4")
                        c.pl4 = n;
                    else
                        c.temp_limit = n;
                }
                command = CpuApply{c};
                group = "cpu";
            } else {
                if (!enabled("gpuPower")) return {1, "GPU control unavailable"};
                auto g = gpu(true); // Intentional Apply reads the current F8 request; passive status does not.
                if (auto it = options.find("--tgp"); it != options.end()) g.tgp = whole(it->second);
                if (auto it = options.find("--db"); it != options.end()) {
                    auto& value = it->second;
                    g.dynamic_boost_enabled = value != "off";
                    if (value != "on" && value != "off") g.dynamic_boost_watts = whole(value);
                }
                bool power = options.contains("--tgp") || options.contains("--db");
                if (power) gpu_plan(g);
                if (options.contains("--nvvdd") || options.contains("--msvdd") || options.contains("--board-input")) {
                    if (!nvidia_ || nvidia_busy_) return {1, nvidia_unavailable()};
                    // Read availability first, as the source does; parse each current once.
                    nvidia_work([&] { nvidia_->currents(); });
                    if (auto it = options.find("--nvvdd"); it != options.end()) g.nvvdd_ma = milliamps(it->second);
                    if (auto it = options.find("--msvdd"); it != options.end()) g.msvdd_ma = milliamps(it->second);
                    if (auto it = options.find("--board-input"); it != options.end())
                        g.board_input_ma = milliamps(it->second);
                }
                command = GpuApply{g, power};
                group = "gpu";
            }
        } else
            throw std::invalid_argument("Unknown command: " + args[0]);
    } catch (const std::invalid_argument& ex) {
        return {2, ex.what()};
    } catch (const std::exception& ex) {
        diagnostic_("[CLI] Not applied: " + args[0] + "; " + ex.what());
        return {1, ex.what()};
    }
    auto result = execute(*command);
    diagnostic_(std::string(result.code ? "[CLI] Not applied: " : "[CLI] Applied: ") + args[0] +
                (result.code ? "; " + result.text : ""));
    if (result.code) return {result.code, result.text};
    auto j = group == "cpu"   ? cpu_status()
             : group == "gpu" ? gpu_status()
             : group == "ec"  ? advanced_status()
                              : fans_status();
    return {0, json ? j.dump() : text_status(j)};
}
std::string Controller::defaults(const std::string& key) {
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
        if (enabled("cpuPower")) {
            auto c = cpu();
            auto cw = cpu_writes(p, m.pl1, m.pl2, m.pl4.value_or(c.pl4), m.tcc_offset.value_or(c.temp_limit));
            log_cpu(cw);
            append(cw);
        }
        if (enabled("gpuPower") && m.ctgp) {
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
        if (enabled("cpuPower")) {
            int temp = control_limit(p, "cpuTempLimit").max;
            w = {{p.reg("PL1"), 0},
                 {p.reg("PL2"), 0},
                 {p.reg("PL4"), 0},
                 {p.reg("cpuTempLimitWrite"),
                  encoding::cpu_temperature(temp, p.cpu_encoding() == "direct", p.tj_max())}};
        }
        auto mask = p.bitfields.at("dynamicBoostEn").get_mask();
        append({{p.reg("tgpDbCtrl"), 0, mask}, {p.reg("tgp"), 0}, {p.reg("dbTotalPower"), 0}, {p.reg("dbMaxTgp"), 0}});
        append({fan_boost_write(p, false), {p.reg("fanCurveEnable"), 0, 128}});
        fan = fan_intent_;
        fan->linked = false;
        fan->curve = false;
        if (curves_supported() && p.default_fan && p.default_fan->cpu && p.default_fan->gpu) {
            fan->cpu_curve = active_curve(*p.default_fan->cpu, p.fan_table.entries);
            auto g = active_curve(*p.default_fan->gpu, p.fan_table.entries);
            if (p.fan_table.has_gpu) fan->gpu_curve = g;
            append(fan_curve_writes(p, *fan->cpu_curve, p.fan_table.has_gpu ? std::optional(g) : std::nullopt, false,
                                    latch_mask_));
            full_curves = true;
        }
        if (session_.battery_bit3)
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
            ++fan_revision_;
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
    if (key.empty() && peripheral_hardware_ && p.default_logo) {
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
    text += key.empty() && peripheral_hardware_ && p.default_logo
                ? "model logo defaults restored; other peripheral intent unchanged"
                : "peripheral intent unchanged";
    if (thermal_->tripped()) text += "; thermal holds supersede normal defaults";
    if (!curves_supported())
        text += "; unsupported fan curves skipped";
    else if (fan && fan->curve && !full_curves)
        text += "; preset has no GPU curve, GPU table unchanged and full fan intent not applied";
    if (fan_review_required_) text += "; editable fan seed still requires review";
    diagnostic_((key.empty() ? std::string("Firmware") : key) + " defaults: " + text);
    return text;
}
}
