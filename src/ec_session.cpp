#include "ec_session.h"
#include "config_text.h"
#include <algorithm>

namespace mech {
int FanSnapshot::active_entries() const {
    int previous=-1,count=0;
    for(int v:cpu.temp_up) { if(v==0 || v>=254 || v<=previous) break; previous=v; ++count; }
    return count;
}
bool FanSnapshot::valid() const {
    int active=active_entries();
    return active>=2 && std::all_of(cpu.temp_up.begin(),cpu.temp_up.begin()+active,[](int v){return v<=120;});
}
std::optional<bool> FanSnapshot::gpu_present() const {
    bool ff=false;
    for(int v:gpu.temp_up) { if(v!=0 && v!=255) return true; if(v==255) ff=true; }
    if(ff) return false;
    return {};
}
FanSnapshot read_fans(EcTransport& ec,const config::FanTable& ft) {
    FanSnapshot s;
    // Count/address calculations must fit the driver's signed int32 ABI, not a generic hex ceiling.
    auto array=[&](const std::string& base) {
        int32_t a=config::hex(base);
        if(ft.entries<0 || static_cast<int64_t>(a)+ft.entries>static_cast<int64_t>(INT32_MAX)+1)
            throw std::out_of_range("Fan table does not fit EC address representation");
        std::vector<int> values;
        for(int i=0;i<ft.entries;++i) values.push_back(ec.read(a+i));
        return values;
    };
    for(int i=0;i<16;++i) try { s.probe[i]=ec.read(0xF60+i); } catch(const std::exception&) { s.probe[i]=255; }
    s.cpu={array(ft.cpu_up),array(ft.cpu_down),array(ft.cpu_duty)};
    s.gpu={array(ft.gpu_up),array(ft.gpu_down),array(ft.gpu_duty)};
    s.enable=ec.read(0x7C5); s.mode=ec.read(0x7C6);
    return s;
}
Sensors read_sensors(EcTransport& ec,const config::MachineProfile& p,Detail detail,const std::optional<Sensors>& shared) {
    auto read=[&](const char* name){return ec.read(p.reg(name));};
    Sensors s=shared.value_or(Sensors{});
    auto cpu_a=read("cpuFanRpmLo"),cpu_b=read("cpuFanRpmHi");
    auto gpu_a=read("gpuFanRpmLo"),gpu_b=read("gpuFanRpmHi");
    s.cpu_rpm=(cpu_a<<8)|cpu_b; s.gpu_rpm=(gpu_b<<8)|gpu_a;
    if(detail==Detail::rpm) return s;
    s.cpu_percent=config::encoding::decode_fan_duty(read("cpuFanDuty"));
    s.gpu_percent=config::encoding::decode_fan_duty(read("gpuFanDuty"));
    s.cpu_limit=config::encoding::decode_cpu_temperature(read("cpuTempLimitRead"),p.cpu_encoding()=="direct",p.tj_max());
    s.battery_soc=read("batterySoc");s.battery_raw=read("batteryMode");s.battery_mode=config::encoding::decode_battery(uint8_t(*s.battery_raw));
    s.boost=(read("statusByte")&4)!=0; s.curve_enabled=(read("fanCurveEnable")&128)!=0;
    if(detail==Detail::monitor) return s;
    if(p.registers.contains("cpuTemp") && !s.cpu_temp) s.cpu_temp=read("cpuTemp");
    if(p.registers.contains("gpuTemp") && !s.gpu_temp) s.gpu_temp=read("gpuTemp");
    if(detail==Detail::temperatures) return s;
    s.adapter_amps=ec.read(0x449)/10.0;
    auto lo=ec.read(0x60C),hi=ec.read(0x60D); s.system_watts=lo|(hi<<8);
    s.db_live=ec.read(0x7D5); s.tgp_live=ec.read(0x788)+p.tgp_offset();
    return s;
}
EcSession::EcSession(ChannelFactory factory,const std::function<void(const std::string&)>& diagnostic,bool read_only,
    std::function<void(unsigned)> wait,const std::filesystem::path& directory,std::optional<config::Identity> supplied,
    const std::function<std::optional<std::filesystem::path>(const std::vector<std::filesystem::path>&)>& manual):read_only_(read_only) {
    ec_=std::make_unique<EcTransport>(factory(LR"(\\.\ACPIDriver)"),diagnostic);
    auto read=[&](int a) {
        try { auto value=ec_->read(a); startup_reads_[a]=value; return value; }
        catch(const std::exception& ex) { if(diagnostic) diagnostic("EC startup read "+std::to_string(a)+": "+ex.what()); throw; }
    };
    identity=supplied?*supplied:config::collect_identity(read);
    if(supplied && !identity.project_id) identity.project_id=read(0x740);
    if(diagnostic) diagnostic("Identity: "+identity.baseboard+"; "+identity.cpu_name+"; vendor="+identity.cpu_vendor.value_or("unknown"));
    selection=config::select_profile(directory,identity,manual);
    if(selection.path.empty()) throw std::runtime_error("Model selection canceled; no matching profile and no manual selection");
    profile=config::load_machine(selection.path);
    config::apply_universal_defaults(profile);
    if(profile.device_path!=R"(\\.\ACPIDriver)") ec_=std::make_unique<EcTransport>(factory(config::wide(profile.device_path)),diagnostic);
    // Includes the original 0x727 flag before any possible activation operation.
    hardware=config::collect_hardware(read,[&](int index){return ec_->spec(index);});
    auto enrichment=config::enrich(profile,hardware);
    validation=config::validate_startup(profile,read,hardware.gpu_module(),enrichment,identity.cpu_vendor);
    try { battery_bit3=uint8_t(read(profile.reg("batteryMode"))&8); }
    catch(const std::exception& ex) { validation.enabled.erase("battery"); validation.disabled["battery"]=std::string("Battery mode variant read failed: ")+ex.what(); }
    // Log the unmapped bitfields too; startup validation intentionally only gates mapped features.
    for(const auto& [name,b]:profile.bitfields) {
        try { auto value=read(config::hex(b.reg)); if(diagnostic) diagnostic("Bitfield "+name+" raw="+std::to_string(value)+" mask="+std::to_string(b.get_mask())); }
        catch(const std::exception& ex) { if(diagnostic) diagnostic("Bitfield "+name+": "+ex.what()); }
    }
    if(read_only) snapshot_fans();
    guard=std::make_unique<WriteGuard>(*ec_,profile,read_only,std::move(wait));
    if(diagnostic) {
        diagnostic("Selected "+profile.project_id+" by "+selection.method+" from "+selection.path.string());
        if(!hardware.ecspec_error.empty()) diagnostic("ECSpec unavailable: "+hardware.ecspec_error);
        for(const auto& [feature,reason]:validation.disabled) diagnostic("Disabled "+feature+": "+reason);
        if(read_only) diagnostic("Fan snapshot: active="+std::to_string(fans.active_entries())+"; valid="+std::to_string(fans.valid())+"; latch raw="+std::to_string(fans.mode));
    }
}
void EcSession::snapshot_fans() {
    fans=read_fans(*ec_,profile.fan_table);
    if(fans.valid()) {
        profile.fan_table.active=fans.active_entries();
        if(auto present=fans.gpu_present()) profile.fan_table.has_gpu=*present;
    }
}
config::Json EcSession::diagnostic_report() {
    using config::Json;
    auto s=sample(Detail::full);
    Json limits=Json::object(),raw=Json::object();
    auto nullable=[](const auto& v)->Json {if(v)return *v;return nullptr;};
    for(const auto& [name,l]:profile.limits) limits[name]={{"min",l.min},{"max",l.max},{"encoding",l.encoding},{"offset",nullable(l.offset)},{"tjMax",nullable(l.tj_max)}};
    for(auto [a,v]:startup_reads_) raw[std::to_string(a)]=v;
    Json spec=nullptr; if(hardware.ecspec) spec=*hardware.ecspec;
    Json gpu_present=nullptr; if(auto v=fans.gpu_present()) gpu_present=*v;
    return {{"readOnly",read_only_},{"model",profile.project_id},{"baseboard",identity.baseboard},{"cpu",identity.cpu_name},
        {"cpuVendor",identity.cpu_vendor.value_or("unknown")},{"selection",selection.method},{"profile",selection.path.string()},
        {"ecspec",spec},{"ecspecError",hardware.ecspec_error},{"startupReadsDecimalAddresses",raw},
        {"limits",limits},{"enabled",validation.enabled},{"disabled",validation.disabled},
        {"pl4Doubled",profile.pl4_doubled()},{"batteryBit3",nullable(battery_bit3)},
        {"fans",{{"cpu",fans.cpu},{"gpu",fans.gpu},{"probe",fans.probe},{"enableRaw",fans.enable},{"modeRaw",fans.mode},
            {"valid",fans.valid()},{"activeEntries",fans.active_entries()},{"gpuPresent",gpu_present},
            {"latchInterpretation","Raw only. X6FR XxFR5xxx_Pro_14.810 static evidence tests bit 2 and shares bits 6/7; no universal equality or Intel inference."}}},
        {"sensors",{{"cpuRpm",s.cpu_rpm},{"gpuRpm",s.gpu_rpm},{"cpuTemp",nullable(s.cpu_temp)},
            {"gpuTemp",nullable(s.gpu_temp)},{"cpuTempLimit",nullable(s.cpu_limit)},
            {"cpuPercent",nullable(s.cpu_percent)},{"gpuPercent",nullable(s.gpu_percent)},
            {"systemWatts",nullable(s.system_watts)},{"adapterAmps",nullable(s.adapter_amps)},
            {"batterySoc",nullable(s.battery_soc)},{"batteryMode",nullable(s.battery_mode)},
            {"boost",nullable(s.boost)},{"curveEnabled",nullable(s.curve_enabled)},
            {"dbLiveWatts",nullable(s.db_live)},{"tgpLiveWatts",nullable(s.tgp_live)}}}};
}
}
