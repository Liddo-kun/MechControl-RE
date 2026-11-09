#include "ec_session.h"
#include "config_text.h"
#include <algorithm>
#include <bitset>
#include <cmath>
#include <tuple>

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
std::string describe(const FanSnapshot& s,const std::string& title) {
    auto row=[](const char* label,const auto& values) {
        std::string r="\n  "+std::string(label)+": ";char b[4];
        for(size_t i=0;i<values.size();++i) {snprintf(b,sizeof b,"%02X",unsigned(values[i])&255);r+=(i?" ":"")+std::string(b);}
        return r;
    };
    return "--- "+title+" ---"+row("CPU tempUp  ",s.cpu.temp_up)+row("CPU tempDown",s.cpu.temp_down)+row("CPU duty    ",s.cpu.duty)
        +row("GPU tempUp  ",s.gpu.temp_up)+row("GPU tempDown",s.gpu.temp_down)+row("GPU duty    ",s.gpu.duty)+row("RamFan2 probe",s.probe)
        +"\n  0x7C5 = "+config::hex_text(s.enable)+"  0x7C6 = "+config::hex_text(s.mode);
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
    auto lo=ec.read(0x60C),hi=ec.read(0x60D); s.adapter_raw=lo|(hi<<8);
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
    if(diagnostic) {
        auto id=identity.project_id;
        diagnostic("--- Identity ---\n  Baseboard:  "+(identity.baseboard.empty()?"(not available)":identity.baseboard)
            +"\n  CPU:        "+(identity.cpu_name.empty()?"(not available)":identity.cpu_name)+"\n  CPU vendor: "+identity.cpu_vendor.value_or("unknown")
            +"\n  EC 0x740:   "+(id?std::to_string(*id)+" ("+config::hex_text(*id)+"), shifted "+std::to_string(*id>>1):"read failed"));
    }
    selection=config::select_profile(directory,identity,manual,diagnostic);
    if(selection.path.empty()) throw std::runtime_error("Model selection canceled; no matching profile and no manual selection");
    profile=config::load_machine(selection.path);
    if(diagnostic) diagnostic("[Profile] Selected "+profile.project_id+" by "+selection.method+" from "+selection.path.string()
        +" (projectIdValue "+std::to_string(profile.project_id_value)+", verified "+(profile.verified?"yes":"no")+")");
    config::apply_universal_defaults(profile);
    if(profile.device_path!=R"(\\.\ACPIDriver)") ec_=std::make_unique<EcTransport>(factory(config::wide(profile.device_path)),diagnostic);
    // Includes the original 0x727 flag before any possible activation operation.
    hardware=config::collect_hardware(read,[&](int index){return ec_->spec(index);});
    bool profile_modes=profile.modes.has_value();
    auto enrichment=config::enrich(profile,hardware);
    validation=config::validate_startup(profile,read,hardware.gpu_module(),enrichment,identity.cpu_vendor);
    try { battery_bit3=uint8_t(read(profile.reg("batteryMode"))&8); }
    catch(const std::exception& ex) { validation.enabled.erase("battery"); validation.disabled["battery"]=std::string("Battery mode variant read failed: ")+ex.what(); }
    if(read_only) snapshot_fans();
    guard=std::make_unique<WriteGuard>(*ec_,profile,read_only,std::move(wait));
    if(diagnostic) {
        describe(diagnostic,enrichment,profile_modes);
        if(read_only) diagnostic(mech::describe(fans,"Fan Table Dump"));
    }
}
// The startup sections remote users send back to add support for untested models.
void EcSession::describe(const std::function<void(const std::string&)>& log,const config::Enrichment& e,bool profile_modes) {
    using config::hex_text;using std::to_string;const auto& h=hardware;const auto& p=profile;
    auto join=[](const auto& items){std::string s;for(const auto& i:items) s+=(s.empty()?"":", ")+std::string(i);return s;};
    auto byte=[&](int a){auto v=h.at(a);return v?hex_text(*v):std::string("read failed");};
    auto dec=[&](int a){auto v=h.at(a);return v?to_string(*v):std::string("?");};
    auto pl4=h.pl4_doubled();bool absolute=p.cpu_encoding()=="direct";
    std::string s="--- Hardware Detection ---\n  Support1 (0x765): "+byte(0x765)+"\n  Support2 (0x766): "+byte(0x766)+"\n  Support5 (0x742): "+byte(0x742);
    if(h.ecspec) for(int i=0;i<12;++i) s+="\n  ECSpec["+hex_text(i)+"]: "+hex_text((*h.ecspec)[i],8);
    else s+="\n  ECSpec config: not available ("+h.ecspec_error+")";
    log(s);
    s="--- ECSpec Decoded Limits ---";
    if(!h.decoded()) s+=h.ecspec?"\n  ECSpec buffer format unexpected (index 9 did not echo 0xBB)":"\n  ECSpec not available";
    else {
        std::array<int,9> b{};for(int i=0;i<9;++i) b[i]=(*h.ecspec)[i]&255;
        auto value=[&](int i,const char* unit){return (b[i]==255?std::string("N/A"):to_string(b[i])+unit)+" (raw="+hex_text(b[i])+")";};
        s+="\n  PL1 max:      "+value(0,"W")+"\n  PL2 max:      "+value(1,"W")
            +"\n  PL4 max:      "+(b[2]==255?std::string("N/A"):to_string(pl4.value_or(false)?b[2]*2:b[2])+"W")+" (raw="+hex_text(b[2])+", "+(pl4?*pl4?"x2":"direct":"0x727 unread")+")"
            +"\n  CPU temp max: "+(b[3]<=40?to_string(105-b[3])+"C (TccOffset="+to_string(b[3])+", TjMax=105, encoding=inverted_offset)"
                :b[3]>=80 && b[3]<=120?to_string(b[3])+"C (absolute, encoding=direct)":"unknown (raw="+hex_text(b[3])+")")
            +"\n  Default TGP:  "+value(7,"W")
            +(b[7]>=50 && b[7]<=200 && b[4]!=255?"\n  TGP range:    "+to_string(b[7])+"-"+to_string(b[4]+b[7])+"W (configurable="+to_string(b[4])+")":"\n  TGP config:   "+value(4,""))
            +"\n  DB max:       "+value(5,"W")+"\n  GPU target:   "+value(6,"C")+"\n  DB CPU TDP:   "+value(8,"W");
        std::vector<std::string> keys;for(const auto& [k,l]:h.extract_limits()) keys.push_back(k);
        s+="\n  Extractable limits: "+(keys.empty()?"none passed sanity validation":join(keys));
    }
    log(s);
    log("--- Extended Identity ---\n  ModuleID:      "+byte(0x7D3)+"\n  ModuleID_GPU:  "+byte(0x7D2)+"\n  ROMID:         "+byte(0x771)+" / "+byte(0x772)
        +"\n  EC Version:    "+dec(0x454)+"."+dec(0x47D)+"\n  SystemID:      "+byte(0x456)+"\n  Support6:      "+byte(0x78E)
        +"\n  ApBiosByte:    "+byte(0x7A4)+" (LiquidCooling="+(h.liquid_cooling()?"true":"false")+")\n  VRM Current:   "+dec(0x753)+"A (max "+dec(0x754)+"A)"
        +"\n  0x727:         "+byte(0x727)+" (PL4 encoding="+(pl4?*pl4?"double":"direct":"unknown")+")");
    auto watts=[&](int a){auto v=h.at(a);return !v?std::string("?"):*v==255?std::string("N/A"):to_string(*v)+"W";};
    auto raw_pl4=[&](int a){auto v=h.at(a);return !v?std::string("?"):*v==255?std::string("N/A"):to_string(pl4.value_or(false)?*v*2:*v)+"W (raw="+to_string(*v)+")";};
    auto tcc=[&](int a){auto v=h.at(a);return !v?std::string("?"):*v==255?std::string("N/A"):absolute?to_string(*v)+"C (absolute)"
        :*v>40?"raw="+to_string(*v)+" (unknown)":to_string(105-*v)+"C (offset="+to_string(*v)+")";};
    s="--- Per-Mode PL Defaults (EC Registers) ---";
    for(auto [name,base,t]:{std::tuple{"Gaming: ",0x730,0x7D8},{"Office: ",0x734,0x7D9},{"BatSave:",0x7A7,0}})
        s+="\n  "+std::string(name)+" PL1="+watts(base)+" PL2="+watts(base+1)+" PL4="+raw_pl4(base+2)+" D="+byte(base+3)+(t?" TCC="+tcc(t):"");
    s+="\n  Turbo TCC: "+tcc(0x7DA);
    s+="\n--- Effective Mode Defaults ("+std::string(profile_modes?"profile":"EC registers")+") ---";
    if(!p.modes) s+="\n  (none)";
    else for(const auto& [k,m]:*p.modes) s+="\n  "+k+": PL1="+to_string(m.pl1)+"W PL2="+to_string(m.pl2)+"W "+(m.pl4?"PL4="+to_string(*m.pl4)+"W":"no PL4")
        +" "+(m.ctgp?"CTGP="+to_string(*m.ctgp)+"W DB="+to_string(m.dynamic_boost.value_or(0))+"W":"no GPU")+" "+(m.tcc_offset?"TCC="+to_string(*m.tcc_offset)+"C":"no TCC")
        +(m.fan_curve?" [has fan curves]":" [no fan curves]");
    if(!profile_modes && p.mode_presets) s+="\n  Presets limited by profile to: "+join(*p.mode_presets);
    log(s);
    s="--- Effective Limits ---";
    if(!e.limits.empty()) s+="\n  ECSpec-derived: "+join(e.limits);
    if(p.cpu_fallback || p.tgp_fallback) s+=std::string("\n  Universal fallback limits: CPU ")+(p.cpu_fallback?"yes":"no")+", TGP "+(p.tgp_fallback?"yes":"no");
    s+="\n  CPU temp encoding: "+p.cpu_encoding()+(e.cpu_encoding_confirmed?" (confirmed by ECSpec)":" (not confirmed by ECSpec)");
    for(const auto& [k,l]:p.limits) s+="\n  "+k+": "+to_string(l.min)+"-"+to_string(l.max)+" (encoding="+l.encoding
        +(l.offset?", offset="+to_string(*l.offset):"")+(l.tj_max?", tjMax="+to_string(*l.tj_max):"")+")";
    log(s);
    s="--- Feature Register Values ---";
    auto reg=[&](const char* name,const std::function<std::string(uint8_t)>& decode) {
        if(!p.registers.contains(name)) {s+="\n  "+std::string(name)+": (not in profile)";return;}
        try {int a=p.reg(name);auto v=ec_->read(a);s+="\n  "+std::string(name)+" ("+hex_text(a,3)+"): "+hex_text(v)+" -> "+decode(v);}
        catch(const std::exception& ex) {s+="\n  "+std::string(name)+" ("+p.registers.at(name)+"): read failed: "+ex.what();}
    };
    auto bits=[](uint8_t v){return "bits: "+std::bitset<8>(v).to_string();};
    auto logo=[](uint8_t v){return "EC="+to_string(v)+", HID~"+to_string(int(std::nearbyint(v*255.0/60)));};
    reg("PL1",[](uint8_t v){return to_string(v)+"W";});reg("PL2",[](uint8_t v){return to_string(v)+"W";});
    reg("PL4",[&](uint8_t v){return to_string(config::encoding::decode_pl4(v,p.pl4_doubled()))+"W";});
    reg("cpuTempLimitRead",[&](uint8_t v){return to_string(config::encoding::decode_cpu_temperature(v,absolute,p.tj_max()))+"C"+(absolute?" (absolute)":" (tjMax="+to_string(p.tj_max())+")");});
    reg("tgpDbCtrl",bits);reg("tgp",[&](uint8_t v){return to_string(v+p.tgp_offset())+"W (offset="+to_string(p.tgp_offset())+")";});
    reg("dbTotalPower",[](uint8_t v){return "raw="+to_string(v);});reg("dbMaxTgp",[](uint8_t v){return to_string(v)+"W";});
    reg("fanControlMode",[&](uint8_t v){bool on=false;try {on=(v&p.bitfields.at("fanBoost").get_mask())!=0;} catch(const std::exception&) {}return std::string("boost ")+(on?"ON":"OFF");});
    reg("fanCurveEnable",bits);reg("batteryMode",[](uint8_t v){return "mode="+to_string(config::encoding::decode_battery(v));});
    reg("batterySoc",[](uint8_t v){return to_string(v)+"%";});
    reg("cpuFanDuty",[](uint8_t v){return to_string(config::encoding::decode_fan_duty(v))+"%";});reg("gpuFanDuty",[](uint8_t v){return to_string(config::encoding::decode_fan_duty(v))+"%";});
    reg("logoOnOff",[](uint8_t v){return "raw="+hex_text(v);});reg("logoR",logo);reg("logoG",logo);reg("logoB",logo);
    log(s);
    s="--- Startup Validation ---\n  Profile "+std::string(p.verified?"verified: features are not gated by register checks":"unverified: register layout validated");
    for(const auto& [name,b]:p.bitfields) try {auto v=ec_->read(config::hex(b.reg));s+="\n  Bitfield "+name+" ("+b.reg+"): raw="+hex_text(v)+", masked="+hex_text(v&b.get_mask());}
        catch(const std::exception& ex) {s+="\n  Bitfield "+name+": read failed: "+ex.what();}
    for(const auto& f:validation.enabled) s+="\n  Enabled:  "+f;
    for(const auto& [f,why]:validation.disabled) s+="\n  Disabled: "+f+" - "+why;
    if(battery_bit3) {
        using config::encoding::battery;
        s+="\n  Battery mode variant: bit3="+std::string(*battery_bit3?"set":"clear")+" (values="+hex_text(battery(0,*battery_bit3))+"/"+hex_text(battery(1,*battery_bit3))+"/"+hex_text(battery(2,*battery_bit3))+")";
    }
    log(s);
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
            {"adapterCurrentRaw",nullable(s.adapter_raw)},{"adapterAmps",nullable(s.adapter_amps)},
            {"batterySoc",nullable(s.battery_soc)},{"batteryMode",nullable(s.battery_mode)},
            {"boost",nullable(s.boost)},{"curveEnabled",nullable(s.curve_enabled)},
            {"dbLiveWatts",nullable(s.db_live)},{"tgpLiveWatts",nullable(s.tgp_live)}}}};
}
}
