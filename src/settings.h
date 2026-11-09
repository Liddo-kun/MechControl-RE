#pragma once
#include "profiles.h"

namespace mech::config {
struct CpuSettings { int pl1=0,pl2=0,pl4=0,temp_limit=0; };
struct GpuSettings {
    int tgp=0,dynamic_boost_watts=0; bool dynamic_boost_enabled=false;
    std::optional<bool> bios_override_enabled;
    std::optional<int> nvvdd_ma,msvdd_ma,board_input_ma;
};
struct ProtectionSettings { bool enabled=false; int threshold=50; bool reduce_cpu=false,fan_boost=false,show_window=true; };
struct FanSettings {
    bool boost=false,curve=false,linked=false,switch_enabled=false; int switch_ms=100;
    std::optional<Curve> cpu_curve=Curve{},gpu_curve=Curve{};
};
struct BatterySettings { int mode=0; };
struct Zone { bool enabled=false; uint8_t r=0,g=0,b=0; int brightness=0; };
struct LightingSettings { Zone logo,keyboard,light_bar; int idle_seconds=0; bool local_dimming=false; };
// Wire values are intentionally not ordered like the cooler's voltage UI.
enum class PumpVoltage : uint8_t { V11=0,V12=1,V7=2,V8=3 };
enum class CoolerRgbMode : uint8_t { Static=0,Breathe=1,Colorful=2,BreatheColor=3 };
struct CoolerSettings {
    bool fan_on=false,pump_on=false,rgb_on=false;
    int fan_duty=50,pump_duty=60;
    PumpVoltage voltage=PumpVoltage::V8;
    uint8_t r=0,g=0,b=0;
    CoolerRgbMode rgb_mode=CoolerRgbMode::Static;
};
struct CoolerState : CoolerSettings {
    CoolerState() { g=128; }
    uint64_t address=0; std::optional<std::string> device_name; bool auto_connect=false;
};
struct LightingState {
    std::optional<Zone> keyboard=Zone{},light_bar=Zone{},logo=Zone{}; int idle_seconds=0;
};
struct UserProfile {
    bool requires_save=false;
    std::optional<std::string> name="",project_id="";
    std::string saved_at="0001-01-01T00:00:00";
    std::optional<CpuSettings> cpu=CpuSettings{};
    std::optional<GpuSettings> gpu=GpuSettings{};
    std::optional<ProtectionSettings> protection=ProtectionSettings{};
    std::optional<FanSettings> fan=FanSettings{};
    std::optional<BatterySettings> battery=BatterySettings{};
    std::optional<LightingSettings> lighting=LightingSettings{};
    std::optional<CoolerSettings> cooler;
    bool liquid_cooler_connected=false; // EC 0x7C7 bit 1; slots written before it existed leave it off, as the EC init did.
    // Structural/model preflight only. Load owns threshold validation/migration;
    // enabled controllers must validate ranges/plans before writes.
    void preflight(const std::string& current_project_id) const;
};
enum ThermalAction : int { None=0,Gpu=1,Cpu=2,Fan=4,All=7 };
enum ThermalStage : int { Released=0,Tripped=2 };
struct Trip {
    std::optional<std::string> time;
    std::string reason,description;
    std::optional<int> temperature,threshold;
    int actions=None; bool show_window=true;
    std::map<std::string,std::optional<std::string>> results;
};
struct ThermalState {
    int stage=Released,held=None;
    std::optional<Trip> last_trip;
    std::string recovery_details;
};
ThermalState recover(const std::optional<ThermalState>& committed,const std::optional<ThermalState>& pending,bool unreadable,bool gpu_supported);
struct Settings {
    int last_slot=0;
    std::vector<std::optional<std::string>> names={"Profile 1","Profile 2","Profile 3","Profile 4","Profile 5"};
    std::string legacy_reason; int legacy_threshold=50;
    bool disable_dlppc=false;
    ThermalState thermal;
};
#define MECH_JSON(Type) void from_json(const Json&,Type&); void to_json(Json&,const Type&)
MECH_JSON(CpuSettings); MECH_JSON(GpuSettings); MECH_JSON(ProtectionSettings); MECH_JSON(FanSettings);
MECH_JSON(BatterySettings); MECH_JSON(Zone); MECH_JSON(LightingSettings); MECH_JSON(CoolerSettings);
MECH_JSON(CoolerState); MECH_JSON(LightingState); MECH_JSON(UserProfile); MECH_JSON(Trip); MECH_JSON(ThermalState);
#undef MECH_JSON
void to_json(Json&,const Settings&);

class SlotSelectionSaveError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
class ProfileStore {
public:
    using Warning = std::function<void(const std::string&)>;
    explicit ProfileStore(std::filesystem::path directory=executable_directory(),bool gpu_recovery_supported=true,Warning log={});
    const Settings& settings() const { return settings_; }
    const std::string& warning() const { return warning_; }
    void select(int slot);
    void set_disable_dlppc(bool disabled);
    void rename(int slot,const std::string& name);
    void clear(int slot);
    bool exists(int slot) const;
    std::optional<UserProfile> load(int slot);
    // The exception reports a partial save if the slot commits but selection fails.
    void save(int slot,const UserProfile& profile);
    void save_thermal(const ThermalState& state);
    LightingState load_lighting(); CoolerState load_cooler();
    void save_lighting(const LightingState&); void save_cooler(const CoolerState&);
private:
    std::filesystem::path slot_path(int slot) const;
    void warn(const std::string&);
    void load_settings(); void save_settings();
    template<class T> T load_direct(const char* filename);
    template<class T> void save_direct(const char* filename,const T&);
    std::filesystem::path directory_;
    bool gpu_supported_;
    Warning log_; std::string warning_;
    Settings settings_;
};
}
