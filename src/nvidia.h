#pragma once
#include <windows.h>
#include "settings.h"
#include <array>
#include <functional>
#include <memory>
#include <span>

namespace mech::nv {
using Bytes=std::vector<uint8_t>;
uint32_t u32(std::span<const uint8_t>,size_t);
void put(std::span<uint8_t>,size_t,uint32_t);
Bytes packet(size_t,uint32_t);
struct Field {int offset;uint32_t min,def,max;};
struct RailOverride {uint32_t def,max;};
struct Override {std::optional<uint32_t> power;std::optional<RailOverride> nv,ms,input;};
struct Record {std::string name;Bytes data;};
struct Bios {
    Bytes image;std::array<uint8_t,5> version_bytes{};uint16_t device=0;
    Field power{},nv{},ms{};std::optional<Field> input;
    static Bios parse(Bytes);
    std::string version() const;
    std::vector<int> offsets() const;
    std::vector<Record> build(const Override&) const;
};
struct Identity {uint16_t device=0;uint32_t subsystem=0,domain=0,bus=0,slot=0,function=0;std::string bios,name,driver;bool operator==(const Identity&) const=default;};
struct PowerLimits {uint32_t min,def,max,enforced;std::optional<uint32_t> requested;};
struct Rail {uint32_t min,def,max,current,stock_max=0,unlock_percent=140;int allowed(bool unlock) const;bool operator==(const Rail&) const=default;};
struct Currents {Rail nv,ms;std::optional<Rail> input;};
struct Budget {uint32_t f7,platform;Budget raise(uint32_t,Budget baseline) const;bool operator==(const Budget&) const=default;};
struct Dlppc {bool active;int policy;uint32_t controllers;};
struct Telemetry {
    uint32_t nv=0,ms=0;std::optional<uint32_t> input,nv_prediction,ms_prediction,nv_limit,ms_limit,input_limit;
    std::optional<uint32_t> current_khz,power_khz,ceiling_khz,clock_mhz,gpu_mw,core_mw,enforced_mw;
    std::optional<double> ipc;
};
// Raw versioned packet seam is used by the ABI fixtures; production resolves NVAPI.
class Api {
public:
    using Call=std::function<void(uint32_t,Bytes&)>;
    Api();
    explicit Api(Call call);
    ~Api();
    Api(const Api&)=delete;Api& operator=(const Api&)=delete;
    Identity identity;
    void reinitialize();
    Bytes bios();
    PowerLimits power();void set_power(uint32_t);
    Currents currents();Currents set_currents(std::optional<int>,std::optional<int>,std::optional<int>);
    Telemetry telemetry();
    Budget budget();void set_budget(Budget);
    std::optional<Dlppc> dlppc();void set_dlppc(bool);
private:
    struct Native;std::unique_ptr<Native> native_;
    Call call_;
    struct Board {int index;uint32_t min,def,max;};
    struct Policy {int index;uint32_t type,min,def,max;};
    struct Rails {Policy nv,ms;std::optional<Policy> input;uint32_t mask;Bytes info;};
    Board board();Rails rails();Bytes control(uint32_t);Rail rail(const Policy&,const Bytes&);
    Bytes power_control(int);Bytes policy_control(int);
    std::optional<std::pair<int,uint32_t>> find_dlppc();
    std::optional<double> inline_activity();
    uint32_t inline_mask_=0,inline_khz_=0;bool inline_unsupported_=false;
    std::array<uint64_t,32> inline_counts_{};int64_t inline_time_=0;
    Bytes status_;
};
// Exact PCI-bound registry/recovery owner. The alternate key/path are fixture-only.
class Store {
public:
    explicit Store(const Identity&);
    Store(HKEY key,std::filesystem::path recovery); // Takes ownership of the test key.
    ~Store();
    static bool present();static std::filesystem::path recovery_path();static bool saved();
    bool awake() const;
    Bios open(const Bytes&,bool& staged,bool& discarded);
    void stage(const Bios&,const std::vector<Record>&);
    bool matches(const std::vector<Record>&);
    void delete_recovery();
    bool modified=false; // A registry write was attempted by the last stage operation.
    std::wstring instance;
private:
    struct Recovery {std::string version;std::map<int,uint32_t> originals;};
    HKEY key_=nullptr;std::filesystem::path path_;
    std::map<std::string,Bytes> records();std::optional<Recovery> recovery();
    static bool owned(const Bytes&,const Recovery&);
    static void require_owned(const std::map<std::string,Bytes>&,const std::optional<Recovery>&);
};
class Power {
public:
    explicit Power(int model_min,std::function<void(const std::string&)>);
    virtual ~Power()=default;
    bool enabled=false;
    bool uncertain=false; // Owned overrides may have changed without active-image verification.
    bool modified=false; // The current configure operation reached a possible hardware write.
    virtual int stock_watts() const {return static_cast<int>(original_.power.max/1000);}
    virtual int minimum_watts() const {return static_cast<int>((original_.power.min+999)/1000);}
    virtual bool awake() const {return store_->awake();}
    virtual PowerLimits limits() {return api_->power();}
    virtual Currents currents();virtual Telemetry telemetry();
    virtual Budget budget() {return api_->budget();}
    virtual std::optional<Dlppc> dlppc() {return api_->dlppc();}
    virtual void set_dlppc(bool active) {api_->set_dlppc(active);}
    virtual bool needs_restart(bool,const config::GpuSettings&);
    virtual void configure(bool,const config::GpuSettings&,bool allow_restart);
    virtual void apply(int watts);virtual bool at_most(int);virtual bool applied(int);
protected:
    Power()=default; // Injected controller fixtures provide the same narrow backend operations.
private:
    void verify(const Bytes&);void restore_budget();void restart();
    std::unique_ptr<Api> api_;std::unique_ptr<Store> store_;Bios original_;
    bool board_verified_=false;
    std::optional<Budget> budget_original_,budget_applied_;
    std::function<void(const std::string&)> log_;
};
// Value-only profile planning: never opens NVIDIA for automatic or stock-current loads.
struct ProfilePlan {config::GpuSettings settings;bool unlock=false,restart=false;std::string notice;};
ProfilePlan plan_profile(const config::GpuSettings&,bool automatic,bool extend,int normal_min,int normal_max,
    int unlock_min,int unlock_max,bool unlocked,bool available,const std::optional<Currents>&,
    const std::function<bool(bool,const config::GpuSettings&)>& needs_restart);
}
