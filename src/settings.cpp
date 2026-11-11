#include "settings.h"
#include "config_text.h"
#include <algorithm>

namespace mech::config {
#define F(key, member) field(j, key, v.member)
#define PUT(key, member) j[key] = v.member
#define OPT(key, member) j[key] = nullable(v.member)
void from_json(const Json& j, CpuSettings& v) {
    object(j);
    F("pl1", pl1);
    F("pl2", pl2);
    F("pl4", pl4);
    F("tempLimit", temp_limit);
}
void to_json(Json& j, const CpuSettings& v) {
    j = {{"pl1", v.pl1}, {"pl2", v.pl2}, {"pl4", v.pl4}, {"tempLimit", v.temp_limit}};
}
void from_json(const Json& j, GpuSettings& v) {
    object(j);
    F("tgp", tgp);
    F("dynamicBoostEnabled", dynamic_boost_enabled);
    F("dynamicBoostWatts", dynamic_boost_watts);
    F("biosOverrideEnabled", bios_override_enabled);
    F("nvvddMilliamps", nvvdd_ma);
    F("msvddMilliamps", msvdd_ma);
    F("boardInputMilliamps", board_input_ma);
}
void to_json(Json& j, const GpuSettings& v) {
    j = {
        {"tgp", v.tgp}, {"dynamicBoostEnabled", v.dynamic_boost_enabled}, {"dynamicBoostWatts", v.dynamic_boost_watts}};
    OPT("biosOverrideEnabled", bios_override_enabled);
    OPT("nvvddMilliamps", nvvdd_ma);
    OPT("msvddMilliamps", msvdd_ma);
    OPT("boardInputMilliamps", board_input_ma);
}
void from_json(const Json& j, ProtectionSettings& v) {
    object(j);
    F("enabled", enabled);
    F("thresholdC", threshold);
    F("reduceCpuPower", reduce_cpu);
    F("fanBoost", fan_boost);
    F("showMainWindow", show_window);
}
void to_json(Json& j, const ProtectionSettings& v) {
    j = {{"enabled", v.enabled},
         {"thresholdC", v.threshold},
         {"reduceCpuPower", v.reduce_cpu},
         {"fanBoost", v.fan_boost},
         {"showMainWindow", v.show_window}};
}
void from_json(const Json& j, FanSettings& v) {
    object(j);
    F("boostEnabled", boost);
    F("curveEnabled", curve);
    F("linkedEnabled", linked);
    F("switchSpeedEnabled", switch_enabled);
    F("switchSpeedMs", switch_ms);
    F("cpuCurve", cpu_curve);
    F("gpuCurve", gpu_curve);
}
void to_json(Json& j, const FanSettings& v) {
    j = {{"boostEnabled", v.boost},
         {"curveEnabled", v.curve},
         {"linkedEnabled", v.linked},
         {"switchSpeedEnabled", v.switch_enabled},
         {"switchSpeedMs", v.switch_ms}};
    OPT("cpuCurve", cpu_curve);
    OPT("gpuCurve", gpu_curve);
}
void from_json(const Json& j, BatterySettings& v) {
    object(j);
    F("mode", mode);
}
void to_json(Json& j, const BatterySettings& v) { j = {{"mode", v.mode}}; }
void from_json(const Json& j, Zone& v) {
    object(j);
    F("enabled", enabled);
    F("r", r);
    F("g", g);
    F("b", b);
    F("brightness", brightness);
}
void to_json(Json& j, const Zone& v) {
    j = {{"enabled", v.enabled}, {"r", v.r}, {"g", v.g}, {"b", v.b}, {"brightness", v.brightness}};
}
static void flat_zone_read(const Json& j, const std::string& prefix, Zone& z) {
    field(j, (prefix + "Enabled").c_str(), z.enabled);
    field(j, (prefix + "R").c_str(), z.r);
    field(j, (prefix + "G").c_str(), z.g);
    field(j, (prefix + "B").c_str(), z.b);
    field(j, (prefix + "Brightness").c_str(), z.brightness);
}
static void flat_zone_write(Json& j, const std::string& prefix, const Zone& z) {
    j[prefix + "Enabled"] = z.enabled;
    j[prefix + "R"] = z.r;
    j[prefix + "G"] = z.g;
    j[prefix + "B"] = z.b;
    j[prefix + "Brightness"] = z.brightness;
}
void from_json(const Json& j, LightingSettings& v) {
    object(j);
    flat_zone_read(j, "logo", v.logo);
    flat_zone_read(j, "keyboard", v.keyboard);
    flat_zone_read(j, "lightBar", v.light_bar);
    F("idleTimerSeconds", idle_seconds);
    F("localDimmingEnabled", local_dimming);
}
void to_json(Json& j, const LightingSettings& v) {
    j = Json::object();
    flat_zone_write(j, "logo", v.logo);
    flat_zone_write(j, "keyboard", v.keyboard);
    flat_zone_write(j, "lightBar", v.light_bar);
    PUT("idleTimerSeconds", idle_seconds);
    PUT("localDimmingEnabled", local_dimming);
}
void from_json(const Json& j, CoolerSettings& v) {
    object(j);
    F("fanOn", fan_on);
    F("fanDuty", fan_duty);
    F("pumpOn", pump_on);
    F("pumpDuty", pump_duty);
    F("rgbOn", rgb_on);
    F("r", r);
    F("g", g);
    F("b", b);
    uint8_t voltage = static_cast<uint8_t>(v.voltage), rgb = static_cast<uint8_t>(v.rgb_mode);
    field(j, "pumpVoltage", voltage);
    field(j, "rgbMode", rgb);
    v.voltage = static_cast<PumpVoltage>(voltage);
    v.rgb_mode = static_cast<CoolerRgbMode>(rgb);
}
void to_json(Json& j, const CoolerSettings& v) {
    j = {{"fanOn", v.fan_on},
         {"fanDuty", v.fan_duty},
         {"pumpOn", v.pump_on},
         {"pumpDuty", v.pump_duty},
         {"rgbOn", v.rgb_on},
         {"pumpVoltage", static_cast<int>(v.voltage)},
         {"rgbMode", static_cast<int>(v.rgb_mode)},
         {"r", v.r},
         {"g", v.g},
         {"b", v.b}};
}
void from_json(const Json& j, CoolerState& v) {
    from_json(j, static_cast<CoolerSettings&>(v));
    F("savedAddress", address);
    F("savedDeviceName", device_name);
    F("autoConnect", auto_connect);
}
void to_json(Json& j, const CoolerState& v) {
    to_json(j, static_cast<const CoolerSettings&>(v));
    PUT("savedAddress", address);
    OPT("savedDeviceName", device_name);
    PUT("autoConnect", auto_connect);
}
void from_json(const Json& j, LightingState& v) {
    object(j);
    F("keyboard", keyboard);
    F("lightBar", light_bar);
    F("logo", logo);
    F("idleTimerSeconds", idle_seconds);
}
void to_json(Json& j, const LightingState& v) {
    j = Json::object();
    OPT("keyboard", keyboard);
    OPT("lightBar", light_bar);
    OPT("logo", logo);
    PUT("idleTimerSeconds", idle_seconds);
}
void from_json(const Json& j, UserProfile& v) {
    object(j);
    F("name", name);
    F("projectId", project_id);
    F("savedAt", saved_at);
    F("cpu", cpu);
    F("gpu", gpu);
    F("gpuFailsafe", protection);
    F("fan", fan);
    F("battery", battery);
    F("lighting", lighting);
    F("liquidCooler", cooler);
    F("liquidCoolerConnected", liquid_cooler_connected);
}
void to_json(Json& j, const UserProfile& v) {
    j = Json::object();
    OPT("name", name);
    OPT("projectId", project_id);
    PUT("savedAt", saved_at);
    OPT("cpu", cpu);
    OPT("gpu", gpu);
    OPT("gpuFailsafe", protection);
    OPT("fan", fan);
    OPT("battery", battery);
    OPT("lighting", lighting);
    OPT("liquidCooler", cooler);
    PUT("liquidCoolerConnected", liquid_cooler_connected);
}
void from_json(const Json& j, Trip& v) {
    object(j);
    F("Time", time);
    F("Reason", reason);
    F("TemperatureC", temperature);
    F("ThresholdC", threshold);
    F("Actions", actions);
    F("ActionDescription", description);
    F("ShowMainWindow", show_window);
    F("Results", results);
}
void to_json(Json& j, const Trip& v) {
    j = Json::object();
    OPT("Time", time);
    PUT("Reason", reason);
    OPT("TemperatureC", temperature);
    OPT("ThresholdC", threshold);
    PUT("Actions", actions);
    PUT("ActionDescription", description);
    PUT("ShowMainWindow", show_window);
    j["Results"] = Json::object();
    for (const auto& [k, x] : v.results) j["Results"][k] = nullable(x);
}
void from_json(const Json& j, ThermalState& v) {
    object(j);
    F("Stage", stage);
    F("Held", held);
    F("LastTrip", last_trip);
    F("RecoveryDetails", recovery_details);
}
void to_json(Json& j, const ThermalState& v) {
    j = {{"Stage", v.stage}, {"Held", v.held}, {"RecoveryDetails", v.recovery_details}};
    OPT("LastTrip", last_trip);
}
void to_json(Json& j, const Settings& v) {
    j = {{"lastProfile", v.last_slot}, {"disableDlppc", v.disable_dlppc}, {"thermalProtection", v.thermal}};
    j["profileNames"] = Json::array();
    for (const auto& n : v.names) j["profileNames"].push_back(nullable(n));
}
#undef F
#undef PUT
#undef OPT
void UserProfile::preflight(const std::string& current) const {
    if (project_id && !blank(*project_id) && !equal_ignore_case(*project_id, current))
        throw std::runtime_error("Profile belongs to another machine model");
    if (!protection || !lighting || (fan && (!fan->cpu_curve || !fan->gpu_curve)))
        throw std::runtime_error("Profile contains null settings/curves");
}
ThermalState recover(const std::optional<ThermalState>& committed, const std::optional<ThermalState>& pending,
                     bool unreadable, bool gpu_supported) {
    std::vector<ThermalState> records;
    for (const auto& s : {committed, pending})
        if (s) records.push_back(*s);
    ThermalState r = records.empty() ? ThermalState{} : records.front();
    bool invalid = false, held = false;
    int actions = 0, known = 0;
    std::optional<Trip> trip;
    for (const auto& s : records) {
        known |= s.held & All;
        invalid |= (s.stage != Released && s.stage != Tripped) || (s.held & ~All) != 0 ||
                   ((s.stage == Released) != (s.held == None));
        if (s.stage == Tripped) {
            held = true;
            actions |= s.held & All;
            if (!trip && s.last_trip) trip = s.last_trip;
        }
    }
    if (held) {
        r.stage = Tripped;
        r.held = actions;
        r.last_trip = trip;
    }
    if (unreadable || invalid) {
        // A damaged pending record must not drop its identifiable CPU/fan
        // obligations merely because a clean committed record was first.
        r.held = (r.held | known) & All;
        if (gpu_supported) r.held |= Gpu;
        r.stage = r.held ? Tripped : Released;
        r.recovery_details +=
            r.held
                ? "\nSaved protection state uncertain - configuration-recovery hold, not a measured thermal event. Reset clears it."
                : "\nSaved protection state uncertain - no supported GPU fallback; review configuration.";
    }
    // Each uncertain start appends to the details. Bound them without cutting a UTF-8 sequence,
    // which could not be saved.
    if (size_t cut = 8192; r.recovery_details.size() > cut) {
        while (cut && (static_cast<unsigned char>(r.recovery_details[cut]) & 0xC0) == 0x80) --cut;
        r.recovery_details.resize(cut);
    }
    return r;
}
ProfileStore::ProfileStore(std::filesystem::path directory, bool gpu_supported, Warning log)
    : directory_(std::move(directory)), gpu_supported_(gpu_supported), log_(std::move(log)) {
    load_settings();
}
void ProfileStore::warn(const std::string& s) {
    if (warning_.find(s) != std::string::npos) return;
    if (!warning_.empty()) warning_ += '\n';
    warning_ += s;
    if (log_) log_(s);
}
std::filesystem::path ProfileStore::slot_path(int slot) const {
    if (slot < 1 || slot > 5) throw std::out_of_range("Slot must be 1-5");
    return directory_ / ("userprofile_" + std::to_string(slot) + ".json");
}
void ProfileStore::load_settings() {
    bool unreadable = false;
    auto read = [&](const std::filesystem::path& path) -> std::optional<Settings> {
        try {
            if (!std::filesystem::exists(path)) return {};
            auto root = read_json(path);
            object(root);
            Settings s;
            auto read_field = [&]<class T>(const Json& parent, const char* key, T& value, bool operational = false) {
                auto i = parent.find(key);
                if (i == parent.end()) {
                    if (operational) {
                        unreadable = true;
                        warn(std::string("Missing operational ") + key);
                    }
                    return;
                }
                try {
                    T candidate{};
                    decode(*i, candidate);
                    value = std::move(candidate);
                } catch (const std::exception&) {
                    unreadable |= operational;
                    warn(std::string("Invalid ") + key + " in " + path.filename().string());
                }
            };
            read_field(root, "lastProfile", s.last_slot);
            if (s.last_slot < 0 || s.last_slot > 5) {
                s.last_slot = 0;
                warn("Invalid lastProfile; select a valid profile; protection was not armed.");
            }
            read_field(root, "profileNames", s.names);
            read_field(root, "disableDlppc", s.disable_dlppc);
            if (auto i = root.find("thermalProtection"); i != root.end()) {
                object(*i);
                auto& t = s.thermal;
                read_field(*i, "Stage", t.stage, true);
                read_field(*i, "Held", t.held, true);
                read_field(*i, "RecoveryDetails", t.recovery_details);
                auto trip = i->find("LastTrip");
                if (trip != i->end() && !trip->is_null()) {
                    if (!trip->is_object())
                        warn("Invalid optional trip history; operational holds retained.");
                    else {
                        Trip h;
                        read_field(*trip, "Time", h.time);
                        read_field(*trip, "Reason", h.reason);
                        read_field(*trip, "TemperatureC", h.temperature);
                        read_field(*trip, "ThresholdC", h.threshold);
                        read_field(*trip, "Actions", h.actions);
                        read_field(*trip, "ActionDescription", h.description);
                        read_field(*trip, "ShowMainWindow", h.show_window);
                        read_field(*trip, "Results", h.results);
                        t.last_trip = h;
                    }
                }
            }
            return s;
        } catch (const std::exception& ex) {
            unreadable = true;
            warn("Saved protection state uncertain (" + path.filename().string() + "): " + ex.what());
            return {};
        }
    };
    auto committed = read(directory_ / "settings.json"), pending = read(directory_ / "settings.json.tmp");
    settings_ = committed ? *committed : pending ? *pending : Settings{};
    settings_.thermal = recover(committed ? std::optional(committed->thermal) : std::nullopt,
                                pending ? std::optional(pending->thermal) : std::nullopt, unreadable, gpu_supported_);
    if (!settings_.thermal.recovery_details.empty()) warn(settings_.thermal.recovery_details);
    auto old = settings_.names;
    settings_.names.clear();
    bool repaired = old.size() != 5;
    for (size_t i = 0; i < 5; ++i) {
        if (i < old.size() && old[i] && !blank(*old[i]))
            settings_.names.push_back(old[i]);
        else {
            settings_.names.emplace_back("Profile " + std::to_string(i + 1));
            repaired = true;
        }
    }
    if (repaired) warn("Invalid profile names repaired; operational protection unchanged.");
}
void ProfileStore::save_settings() { write_json(directory_ / "settings.json", settings_, true); }
void ProfileStore::select(int slot) {
    if (slot < 0 || slot > 5) throw std::out_of_range("Selection must be 0-5");
    int old = settings_.last_slot;
    settings_.last_slot = slot;
    try {
        save_settings();
    } catch (...) {
        settings_.last_slot = old;
        throw;
    }
}
void ProfileStore::set_disable_dlppc(bool value) {
    bool old = settings_.disable_dlppc;
    settings_.disable_dlppc = value;
    try {
        save_settings();
    } catch (...) {
        settings_.disable_dlppc = old;
        throw;
    }
}
void ProfileStore::rename(int slot, const std::string& name) {
    slot_path(slot);
    auto old = settings_.names[slot - 1];
    settings_.names[slot - 1] = name;
    try {
        save_settings();
    } catch (...) {
        settings_.names[slot - 1] = old;
        throw;
    }
}
void ProfileStore::clear(int slot) {
    auto path = slot_path(slot);
    int old_slot = settings_.last_slot;
    auto old_name = settings_.names[slot - 1];
    if (settings_.last_slot == slot) settings_.last_slot = 0;
    settings_.names[slot - 1] = "Profile " + std::to_string(slot);
    try {
        save_settings();
    } catch (...) {
        settings_.last_slot = old_slot;
        settings_.names[slot - 1] = std::move(old_name);
        throw;
    }
    if (std::filesystem::is_directory(path)) throw std::runtime_error("Slot path is a directory, not a file");
    std::filesystem::remove(path);
}
bool ProfileStore::exists(int slot) const { return std::filesystem::is_regular_file(slot_path(slot)); }
std::optional<UserProfile> ProfileStore::load(int slot) {
    auto path = slot_path(slot);
    if (!std::filesystem::exists(path)) return {};
    auto profile = read_json(path).get<UserProfile>();
    if (!profile.protection) throw std::runtime_error("Protection settings are null");
    if (int t = profile.protection->threshold; t < 50 || t > 95)
        throw std::runtime_error("Protection threshold must be 50-95 C");
    return profile;
}
void ProfileStore::save(int slot, const UserProfile& profile) {
    write_json(slot_path(slot), profile, true);
    try {
        select(slot);
    } catch (const std::exception& ex) {
        throw SlotSelectionSaveError(
            "Profile slot " + std::to_string(slot) +
            " was saved, but selected-slot settings were not saved. Selection is unchanged: " + ex.what());
    }
}
void ProfileStore::save_thermal(const ThermalState& state) {
    auto old = settings_.thermal;
    settings_.thermal = state;
    try {
        save_settings();
    } catch (...) {
        settings_.thermal = recover(old, state, false, gpu_supported_);
        throw;
    }
}
template <class T> T ProfileStore::load_direct(const char* name) {
    try {
        auto path = directory_ / name;
        if (std::filesystem::exists(path)) {
            auto j = read_json(path);
            if (!j.is_null()) return j.get<T>();
        }
    } catch (const std::exception& ex) {
        if (log_) log_(std::string(name) + " load failed: " + ex.what());
    }
    return {};
}
template <class T> void ProfileStore::save_direct(const char* name, const T& value) {
    try {
        write_json(directory_ / name, value, false);
    } catch (const std::exception& ex) {
        if (log_) log_(std::string(name) + " save failed: " + ex.what());
    }
}
LightingState ProfileStore::load_lighting() { return load_direct<LightingState>("lighting_state.json"); }
CoolerState ProfileStore::load_cooler() { return load_direct<CoolerState>("liquid_cooler_state.json"); }
void ProfileStore::save_lighting(const LightingState& v) { save_direct("lighting_state.json", v); }
void ProfileStore::save_cooler(const CoolerState& v) { save_direct("liquid_cooler_state.json", v); }
}
