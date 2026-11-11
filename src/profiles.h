#pragma once
#include "config_json.h"
#include <functional>
#include <set>
#include <array>

namespace mech::config {
struct Curve {
    std::vector<int> temp_up, temp_down, duty;
};
struct FanDefaults {
    std::optional<Curve> cpu, gpu;
};
struct Limit {
    int min = 0, max = 0;
    std::string encoding = "direct";
    std::optional<int> tj_max, offset;
};
struct Bitfield {
    std::string reg;
    std::optional<int> bit;
    std::optional<std::string> mask;
    uint8_t get_mask() const;
};
struct Redirect {
    std::string read_addr, transform;
    std::optional<int> tj_max;
};
struct BatteryMode {
    std::string name, value;
};
struct Logo {
    uint8_t r = 0, g = 0, b = 0, on_off = 0;
};
struct Mode {
    int pl1 = 0, pl2 = 0;
    std::optional<int> pl4, ctgp, dynamic_boost, tcc_offset;
    std::optional<FanDefaults> fan_curve;
};
// EC power-limit bypass, offered only on EC firmware ("0x454.0x47D" in decimal) whose image was checked.
using EcVersion = std::pair<int, int>; // Major, minor.
struct EcBypass {
    std::string value;
    std::vector<std::pair<EcVersion, EcVersion>> ec_versions;
}; // Inclusive ranges from "M.m" or "M.m-M.m".
struct FanTable {
    std::string format = "RamFan1", cpu_up, cpu_down, cpu_duty, gpu_up, gpu_down, gpu_duty;
    int entries = 16, active = 10, padding_temp = 255;
    bool has_gpu = true;
};
struct MachineProfile {
    std::string project_id, device_path = R"(\\.\ACPIDriver)";
    int project_id_value = 0;
    bool verified = false, tgp_fallback = false, cpu_fallback = false;
    std::vector<std::string> baseboard_patterns, allowlist, skip_verify;
    std::optional<std::string> cpu_vendor;
    std::map<std::string, std::string> registers;
    std::map<std::string, Limit> limits;
    std::map<std::string, Bitfield> bitfields;
    std::map<std::string, Redirect> redirects;
    std::vector<BatteryMode> battery_modes;
    FanTable fan_table;
    std::optional<FanDefaults> default_fan;
    std::optional<Logo> default_logo;
    std::optional<std::map<std::string, Mode>> modes;
    std::optional<std::vector<std::string>> mode_presets; // EC presets extraction may expose; absent keeps all three.
    std::optional<EcBypass> ec_bypass;
    int reg(const std::string& name) const;
    int tj_max() const;
    int tgp_offset() const;
    std::string cpu_encoding() const;
    bool pl4_doubled() const;
    std::set<int> expanded_allowlist() const;
    std::set<int> expanded_skip_verify() const;
    std::map<int, Redirect> expanded_redirects() const;
};
int hex(const std::string& value);
void from_json(const Json&, Curve&);
void to_json(Json&, const Curve&);
void from_json(const Json&, FanDefaults&);
void from_json(const Json&, Limit&);
void from_json(const Json&, Bitfield&);
void from_json(const Json&, Redirect&);
void from_json(const Json&, BatteryMode&);
void from_json(const Json&, Logo&);
void from_json(const Json&, Mode&);
void from_json(const Json&, EcBypass&);
void from_json(const Json&, FanTable&);
void from_json(const Json&, MachineProfile&);
MachineProfile load_machine(const std::filesystem::path& path);
void apply_universal_defaults(MachineProfile&);
std::filesystem::path profiles_directory(const std::filesystem::path& start = executable_directory());
struct Identity {
    std::string baseboard, cpu_name;
    std::optional<std::string> cpu_vendor;
    std::optional<uint8_t> project_id;
};
// The optional EC reader must belong to the serialized EC owner. No transport or writes are opened here.
Identity collect_identity(const std::function<uint8_t(int)>& read_ec = {});
struct Selection {
    std::filesystem::path path;
    std::string method;
};
std::vector<std::filesystem::path> manual_profiles(const std::filesystem::path& directory);
Selection select_profile(
    const std::filesystem::path&, const Identity&,
    const std::function<std::optional<std::filesystem::path>(const std::vector<std::filesystem::path>&)>& manual = {},
    const std::function<void(const std::string&)>& log = {});

using ReadByte = std::function<uint8_t(int)>;
struct Hardware {
    std::map<int, uint8_t> bytes;
    std::optional<std::array<uint32_t, 12>> ecspec;
    std::string ecspec_error;
    // Bytes whose startup read failed are absent and treated as unknown.
    std::optional<uint8_t> at(int address) const {
        auto i = bytes.find(address);
        if (i == bytes.end()) return {};
        return i->second;
    }
    bool decoded() const { return ecspec && ((*ecspec)[9] & 255) == 0xBB; }
    std::optional<bool> pl4_doubled() const {
        auto v = at(0x727);
        if (!v) return {};
        return (*v & 0x80) != 0;
    }
    bool liquid_cooling() const { return (at(0x7A4).value_or(0) & 0x80) != 0; }
    uint8_t gpu_module() const { return at(0x7D2).value_or(255); } // 255: not reported
    std::map<std::string, Limit> extract_limits() const;
    std::optional<std::map<std::string, Mode>> extract_modes(bool absolute) const;
};
Hardware collect_hardware(const ReadByte&, const std::function<uint32_t(int)>& read_spec);
struct Enrichment {
    std::set<std::string> limits;
    bool cpu_encoding_confirmed = false, pl4_unknown = false;
};
Enrichment enrich(MachineProfile&, const Hardware&);
struct Validation {
    std::set<std::string> enabled;
    std::map<std::string, std::string> disabled;
};
Validation validate_startup(const MachineProfile&, const ReadByte&, uint8_t gpu_module = 255,
                            const Enrichment& enrichment = {}, const std::optional<std::string>& vendor = {});
std::string gpu_recovery_unavailable(const MachineProfile&,
                                     uint8_t gpu_module); // Empty when a thermal trip can lower GPU power.

namespace encoding {
uint8_t pl1(int watts);
uint8_t pl4(int watts, bool doubled);
int decode_pl4(uint8_t, bool doubled);
uint8_t tgp(int watts, int offset = 95);
uint8_t dynamic_boost(int watts);
uint8_t cpu_temperature(int c, bool direct, int tj_max = 105);
int decode_cpu_temperature(uint8_t, bool direct, int tj_max = 105);
uint8_t fan_duty(int percent);
int decode_fan_duty(uint8_t);
uint8_t switch_speed(bool enabled, int ms);
std::pair<bool, int> decode_switch_speed(uint8_t);
uint8_t battery(int mode, uint8_t bit3);
int decode_battery(uint8_t);
}
}
