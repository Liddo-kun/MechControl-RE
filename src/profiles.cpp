#include "profiles.h"
#include "config_text.h"
#include <windows.h>
#include <algorithm>
#include <charconv>
#include <tuple>
#include <bit>

namespace mech::config {
#define F(key,member) field(j,key,v.member)
void from_json(const Json& j,Curve& v) { object(j); F("tempUp",temp_up); F("tempDown",temp_down); F("duty",duty); }
void to_json(Json& j,const Curve& v) { j={{"tempUp",v.temp_up},{"tempDown",v.temp_down},{"duty",v.duty}}; }
void from_json(const Json& j,FanDefaults& v) { object(j); F("cpu",cpu); F("gpu",gpu); }
void from_json(const Json& j,Limit& v) { object(j); F("min",min); F("max",max); F("encoding",encoding); F("tjMax",tj_max); F("offset",offset); }
void from_json(const Json& j,Bitfield& v) { object(j); F("register",reg); F("bit",bit); F("mask",mask); }
void from_json(const Json& j,Redirect& v) { object(j); F("readAddr",read_addr); F("transform",transform); F("tjMax",tj_max); }
void from_json(const Json& j,BatteryMode& v) { object(j); F("name",name); F("value",value); }
void from_json(const Json& j,Logo& v) { object(j); F("r",r); F("g",g); F("b",b); F("onOff",on_off); }
void from_json(const Json& j,Mode& v) {
    object(j); F("PL1",pl1); F("PL2",pl2); F("PL4",pl4); F("CTGP",ctgp); F("dynamicBoost",dynamic_boost); F("tccOffset",tcc_offset); F("fanCurve",fan_curve);
}
void from_json(const Json& j,EcBypass& v) {
    object(j); F("value",value); std::vector<std::string> versions; field(j,"ecVersions",versions);
    auto number=[](std::string_view s,std::string_view entry) {
        int n=0;auto [end,error]=std::from_chars(s.data(),s.data()+s.size(),n);
        if(s.empty() || error!=std::errc{} || end!=s.data()+s.size()) throw std::runtime_error("Invalid ecBypass EC version: "+std::string(entry));return n;
    };
    for(std::string_view entry:versions) {
        auto version=[&](std::string_view s) {auto dot=s.find('.');if(dot==s.npos) number({},entry);return EcVersion{number(s.substr(0,dot),entry),number(s.substr(dot+1),entry)};};
        auto dash=entry.find('-');auto low=version(entry.substr(0,dash));v.ec_versions.push_back({low,dash==entry.npos?low:version(entry.substr(dash+1))});
    }
}
void from_json(const Json& j,FanTable& v) {
    object(j); F("format",format); F("cpuTempUpBase",cpu_up); F("cpuTempDownBase",cpu_down); F("cpuDutyBase",cpu_duty);
    F("gpuTempUpBase",gpu_up); F("gpuTempDownBase",gpu_down); F("gpuDutyBase",gpu_duty);
    F("entriesPerArray",entries); F("activeEntries",active); F("paddingTemp",padding_temp);
}
void from_json(const Json& j,MachineProfile& v) {
    object(j); F("projectId",project_id); F("projectIdValue",project_id_value); F("verified",verified);
    if(auto i=j.find("driver");i!=j.end()) { object(*i); field(*i,"devicePath",v.device_path); }
    F("registers",registers); F("fanTable",fan_table); F("limits",limits); F("bitfields",bitfields);
    F("batteryModes",battery_modes); F("baseboardPatterns",baseboard_patterns); F("cpuVendor",cpu_vendor);
    F("skipVerify",skip_verify); F("verifyRedirect",redirects); F("defaultFanCurve",default_fan);
    F("defaultLogo",default_logo); F("modeDefaults",modes); F("modePresets",mode_presets); F("writeAllowlist",allowlist); F("ecBypass",ec_bypass);
}
#undef F
int hex(const std::string& text) {
    // Match Replace("0x", "") then Convert.ToInt32(..., 16), including the
    // signed interpretation of all 32 bits. Address policy belongs to the driver/guard.
    std::string clean=text;
    for(size_t i=0;(i=clean.find("0x",i))!=std::string::npos;) clean.erase(i,2);
    std::string_view s=clean;
    if(s.starts_with('+')) s.remove_prefix(1);
    if(s.starts_with("0x") || s.starts_with("0X")) s.remove_prefix(2);
    uint32_t v=0; auto [p,e]=std::from_chars(s.data(),s.data()+s.size(),v,16);
    if(e!=std::errc{} || p!=s.data()+s.size()) throw std::runtime_error("Invalid hex integer: "+text);
    return std::bit_cast<int>(v);
}
uint8_t Bitfield::get_mask() const {
    if(mask) return static_cast<uint8_t>(hex(*mask));
    if(bit) return static_cast<uint8_t>(uint32_t{1}<<(static_cast<uint32_t>(*bit)&31));
    throw std::runtime_error("Invalid bitfield mask/bit");
}
int MachineProfile::reg(const std::string& name) const { return hex(registers.at(name)); }
int MachineProfile::tj_max() const { auto i=limits.find("cpuTempLimit"); return i==limits.end()?105:i->second.tj_max.value_or(105); }
int MachineProfile::tgp_offset() const { auto i=limits.find("TGP"); return i==limits.end()?95:i->second.offset.value_or(95); }
std::string MachineProfile::cpu_encoding() const { auto i=limits.find("cpuTempLimit"); return i==limits.end()?"inverted_offset":i->second.encoding; }
bool MachineProfile::pl4_doubled() const { auto i=limits.find("PL4"); return i!=limits.end() && i->second.encoding=="double"; }
static const std::vector<std::string> init_allow={"0x708","0x709","0x726","0x727","0x741","0x748","0x7C5","0x7C7","0x7C6","0x706","0xF00-0xF5F"};
static const std::vector<std::string> feature_allow={"0x783","0x784","0x785","0x786","0x743","0x744","0x745","0x746","0x751","0x7A6","0x767","0x769","0x76A","0x76B","0x7AB","0x78C","0x787"};
static const std::vector<std::string> universal_skip={"0x767","0x78C","0x726","0x727","0x741","0x7C6","0x708","0x709","0x748","0x706","0x7C7"};
static void expand(std::set<int>& out,const std::vector<std::string>& entries) {
    for(const auto& e:entries) {
        auto dash=e.find('-');
        if(dash==std::string::npos) out.insert(hex(e));
        else { int a=hex(e.substr(0,dash)),b=hex(e.substr(dash+1)); for(int64_t n=a;n<=b;++n) out.insert(static_cast<int>(n)); }
    }
}
std::set<int> MachineProfile::expanded_allowlist() const { std::set<int> r; expand(r,init_allow); expand(r,allowlist); return r; }
std::set<int> MachineProfile::expanded_skip_verify() const { std::set<int> r; expand(r,universal_skip); expand(r,skip_verify); return r; }
std::map<int,Redirect> MachineProfile::expanded_redirects() const { std::map<int,Redirect> r; for(const auto& [k,v]:redirects) r[hex(k)]=v; return r; }
MachineProfile load_machine(const std::filesystem::path& path) {
    auto p=read_json(path).get<MachineProfile>();
    if(p.project_id.empty() || p.registers.empty() || p.allowlist.empty()) throw std::runtime_error("Profile requires projectId, registers and writeAllowlist: "+path.string());
    return p;
}
void apply_universal_defaults(MachineProfile& p) {
    p.bitfields.try_emplace("fanCurveEnable",Bitfield{"0x7C5",7,{}});
    p.bitfields.try_emplace("batteryMode",Bitfield{"0x7A6",{},"0x38"});
    p.bitfields.try_emplace("dynamicBoostEn",Bitfield{"0x743",{},"0x07"});
    p.bitfields.try_emplace("logoOnOff",Bitfield{"0x7AB",7,{}});
    p.bitfields.try_emplace("fanBoost",Bitfield{"0x751",6,{}});
    if(p.limits.empty()) {
        p.tgp_fallback=p.cpu_fallback=true;
        p.limits={{"PL1",{15,150,"direct",{},{}}},{"PL2",{15,150,"direct",{},{}}},
            {"PL4",{30,300,"double",{},{}}},{"cpuTempLimit",{80,100,"inverted_offset",105,{}}},
            {"TGP",{95,150,"offset",{},95}},{"dynamicBoost",{5,25,"direct",{},{}}},
            {"fanDuty",{0,200,"raw_div_2_percent",{},{}}},{"fanTemp",{0,105,"direct",{},{}}},
            {"fanSwitchSpeed",{100,12700,"fan_switch_speed",{},{}}}};
    }
    if(!p.verified) {
        // Unverified profiles use the universal lists. A declared EC bypass keeps only its own explicit entry.
        std::set<int> listed; expand(listed,p.allowlist);
        bool bypass=p.ec_bypass && p.registers.contains("ecBypass") && listed.contains(p.reg("ecBypass"));
        p.allowlist=init_allow; p.allowlist.insert(p.allowlist.end(),feature_allow.begin(),feature_allow.end()); p.skip_verify=universal_skip;
        if(bypass) p.allowlist.push_back(p.registers.at("ecBypass"));
    }
    if(p.redirects.empty()) p.redirects["0x786"]={"0x463","inverted_offset",105};
    if(p.battery_modes.empty()) p.battery_modes={{"High Capacity","0x08"},{"Balanced","0x18"},{"Stationary","0x28"}};
}
std::filesystem::path profiles_directory(const std::filesystem::path& start) {
    auto dir=start;
    for(int i=0;i<6 && !dir.empty();++i) { auto candidate=dir/"Profiles"/"MECHREVO"; if(std::filesystem::is_directory(candidate)) return candidate; auto parent=dir.parent_path(); if(parent==dir) break; dir=parent; }
    throw std::runtime_error("Profiles/MECHREVO not found");
}
std::vector<std::filesystem::path> manual_profiles(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> r;
    for(const auto& e:std::filesystem::directory_iterator(dir)) if(e.is_regular_file() &&
        CompareStringOrdinal(e.path().extension().c_str(),-1,L".json",-1,TRUE)==CSTR_EQUAL) r.push_back(e.path());
    std::sort(r.begin(),r.end()); return r;
}
Selection select_profile(const std::filesystem::path& dir,const Identity& id,
    const std::function<std::optional<std::filesystem::path>(const std::vector<std::filesystem::path>&)>& manual,
    const std::function<void(const std::string&)>& log) {
    struct Candidate { std::filesystem::path path; Json json; };
    std::vector<Candidate> candidates;
    auto note=[&](const std::string& s){if(log) log("[Profile] "+s);};
    auto names=[](const std::vector<std::filesystem::path>& v){std::string s;for(const auto& p:v) s+=(s.empty()?"":", ")+p.filename().string();return s;};
    auto files=manual_profiles(dir);
    for(const auto& f:files) try {
        auto j=read_json(f); object(j);
        if(id.cpu_vendor && j.contains("cpuVendor") && j["cpuVendor"].is_string() && !equal_ignore_case(j["cpuVendor"].get<std::string>(),*id.cpu_vendor)) {
            note("Skipping "+f.filename().string()+": declares cpuVendor '"+j["cpuVendor"].get<std::string>()+"', CPU is "+*id.cpu_vendor);continue;
        }
        candidates.push_back({f,std::move(j)});
    } catch(const std::exception& ex) { note("Skipping malformed "+f.filename().string()+": "+ex.what()); } // Matching continues.
    auto board=lower_invariant(wide(id.baseboard)); Selection best; size_t length=0;
    for(const auto& c:candidates) try {
        if(!c.json.contains("baseboardPatterns") || !c.json["baseboardPatterns"].is_array()) continue;
        for(const auto& p:c.json["baseboardPatterns"]) {
            auto s=lower_invariant(wide(p.get<std::string>()));
            if(!blank(s) && s.size()>length && board.find(s)!=std::string::npos) { best={c.path,"baseboard pattern"}; length=s.size(); }
        }
    } catch(const std::exception&) {}
    if(!best.path.empty()) return best;
    if(id.project_id) for(int shift=0;shift<=1;++shift) {
        std::vector<std::filesystem::path> matches;int value=*id.project_id>>shift;
        for(const auto& c:candidates) try {
            if(!c.json.contains("projectIdValue")) continue;
            int n=0; decode(c.json["projectIdValue"],n);
            if(n==value) matches.push_back(c.path);
        } catch(const std::exception&) {}
        if(matches.empty()) continue;
        if(matches.size()>1) note("Project ID "+std::to_string(value)+" matched multiple profiles: "+names(matches)+" -> chose "+matches[0].filename().string());
        return {matches[0],shift?"EC shifted":"EC direct"};
    }
    length=0; std::wstring best_token; std::vector<std::filesystem::path> matches;
    for(const auto& c:candidates) {
        auto token=lower_invariant(c.path.stem().wstring()); size_t pos=0;
        while(!token.empty() && (pos=board.find(token,pos))!=std::string::npos) {
            auto end=pos+token.size();
            if((pos==0 || !letter_or_digit(board[pos-1])) && (end==board.size() || !letter_or_digit(board[end]))) {
                matches.push_back(c.path);
                if(token.size()>length || (token.size()==length && token<best_token)) { best={c.path,"baseboard filename"}; length=token.size(); best_token=token; } break;
            } ++pos;
        }
    }
    if(matches.size()>1) note("Baseboard matched multiple profile names: "+names(matches)+" -> chose "+best.path.filename().string()+" (longest match)");
    if(!best.path.empty()) return best;
    note("No automatic match; asking for manual selection");
    if(manual) if(auto path=manual(files)) return {*path,"manual selection"};
    return {{},"manual selection canceled"};
}
Hardware collect_hardware(const ReadByte& read,const std::function<uint32_t(int)>& spec) {
    Hardware h;
    for(int a:{0x765,0x766,0x742,0x78E,0x7A4,0x7D3,0x7D2,0x771,0x772,0x454,0x47D,0x456,0x727,
        0x730,0x731,0x732,0x733,0x7D8,0x734,0x735,0x736,0x737,0x7D9,0x7A7,0x7A8,0x7A9,0x7AA,0x7DA,0x753,0x754})
        try { h.bytes[a]=read(a); } catch(const std::exception&) { /* the reader logs it; dependent features treat the byte as unknown */ }
    try { std::array<uint32_t,12> s{}; for(int i=0;i<12;++i) s[i]=spec(i); h.ecspec=s; } catch(const std::exception& ex) { h.ecspec_error=ex.what(); }
    return h;
}
std::map<std::string,Limit> Hardware::extract_limits() const {
    std::map<std::string,Limit> r; if(!decoded()) return r;
    std::array<int,9> b{}; for(int i=0;i<9;++i) b[i]=(*ecspec)[i]&255;
    if(b[0]>=15 && b[0]<=253) r["PL1"]={10,b[0],"direct",{},{}};
    if(b[1]>=15 && b[1]<=253) r["PL2"]={10,b[1],"direct",{},{}};
    if(auto d=pl4_doubled(); d && b[2]>=15 && b[2]<=250) r["PL4"]={20,*d?b[2]*2:b[2],*d?"double":"direct",{},{}};
    if(b[3]<=40) r["cpuTempLimit"]={80,105-b[3],"inverted_offset",105,{}};
    else if(b[3]>=80 && b[3]<=120) r["cpuTempLimit"]={80,b[3],"direct",{},{}};
    if(b[7]>=50 && b[7]<=200 && b[4]!=255 && b[4]+b[7]<=300) r["TGP"]={b[7],b[4]+b[7],"offset",{},b[7]};
    if(b[5]<=50) r["dynamicBoost"]={5,b[5],"direct",{},{}};
    return r;
}
std::optional<std::map<std::string,Mode>> Hardware::extract_modes(bool absolute) const {
    std::map<std::string,Mode> r;
    for(auto [name,base,tcc]:{std::tuple{"gaming",0x730,0x7D8},std::tuple{"office",0x734,0x7D9},std::tuple{"batterySaver",0x7A7,0}}) {
        // Unread bytes become 255, which every range check below rejects.
        int a=at(base).value_or(255),b=at(base+1).value_or(255),c=at(base+2).value_or(255),t=tcc?at(tcc).value_or(255):255;
        if(a<5 || a>200) continue;
        Mode m; m.pl1=a; m.pl2=(b>=5 && b<=200)?b:a;
        if(auto d=pl4_doubled(); d && c>=5 && c<=250) m.pl4=*d?c*2:c;
        if(absolute && t>=60 && t<=120) m.tcc_offset=t;
        else if(!absolute && t<=40) m.tcc_offset=105-t;
        r[name]=m;
    }
    if(r.empty()) return {}; return r;
}
Enrichment enrich(MachineProfile& p,const Hardware& h) {
    Enrichment e; auto limits=h.extract_limits();
    if(!p.verified) {
        for(const auto& [k,v]:limits) { p.limits[k]=v; e.limits.insert(k); }
        if(limits.contains("TGP")) p.tgp_fallback=false;
        if(limits.contains("PL1") && limits.contains("PL2") && limits.contains("PL4")) p.cpu_fallback=false;
    }
    auto temp=limits.find("cpuTempLimit");
    if(temp!=limits.end() && temp->second.encoding=="direct") { p.limits["cpuTempLimit"]=temp->second; p.redirects["0x786"]={"0x463","direct",{}}; }
    e.cpu_encoding_confirmed=temp!=limits.end() && temp->second.encoding==p.cpu_encoding();
    if(auto d=h.pl4_doubled()) { if(p.limits.contains("PL4")) p.limits["PL4"].encoding=*d?"double":"direct"; } else e.pl4_unknown=true;
    if(!p.modes) {
        p.modes=h.extract_modes(p.cpu_encoding()=="direct");
        if(p.modes && p.mode_presets) std::erase_if(*p.modes,[&](const auto& m){return std::ranges::find(*p.mode_presets,m.first)==p.mode_presets->end();});
    }
    return e;
}
std::string gpu_recovery_unavailable(const MachineProfile& p,uint8_t module) {
    if(module==0 || module==255) return "the EC reports no GPU module";
    if(p.tgp_fallback) return "this model's GPU power range is unknown";
    return p.registers.contains("gpuTemp") && p.registers.contains("tgp")?"":"the profile lacks a GPU temperature or power register";
}
Validation validate_startup(const MachineProfile& p,const ReadByte& read,uint8_t gpu_module,const Enrichment& e,const std::optional<std::string>& vendor) {
    Validation r;
    auto enriched=[&](std::string_view f) {
        if(f=="cpuPower") return e.limits.contains("PL1")||e.limits.contains("PL2")||e.limits.contains("PL4")||e.limits.contains("cpuTempLimit");
        if(f=="gpuPower") return e.limits.contains("TGP")||e.limits.contains("dynamicBoost");
        return false;
    };
    auto disable=[&](const std::string& feature,const std::string& why) { if(!p.verified) r.disabled.try_emplace(feature,why); };
    const std::map<std::string,std::string> features={{"fanCurveEnable","fanControl"},{"batteryMode","battery"},{"dynamicBoostEn","gpuPower"},{"logoOnOff","lighting"}};
    const std::map<std::string,std::set<int>> valid={{"dynamicBoostEn",{0,2,5,7}},{"batteryMode",{0,8,16,24,32,40}},{"fanCurveEnable",{0,128}},{"logoOnOff",{0,128}}};
    for(const auto& [k,b]:p.bitfields) {
        if(!features.contains(k)) continue;
        const auto& f=features.at(k);
        try { int v=read(hex(b.reg))&b.get_mask(); if(valid.contains(k) && !valid.at(k).contains(v) && !enriched(f)) disable(f,"Invalid bitfield "+k); }
        catch(const std::exception& ex) { disable(f,"Bitfield "+k+" read failed: "+ex.what()); }
    }
    auto check=[&](const char* reg,const char* feature,int min,int max,const std::function<int(uint8_t)>& decode_value) {
        if(!p.registers.contains(reg)) return;
        try { int v=decode_value(read(p.reg(reg))); if((v<min || v>max) && !enriched(feature)) disable(feature,std::string("Register ")+reg+" outside sanity range"); }
        catch(const std::exception&) { /* reference read errors do not establish a layout mismatch */ }
    };
    check("PL1","cpuPower",5,300,[](uint8_t v){return v;});
    check("PL2","cpuPower",5,300,[](uint8_t v){return v;});
    check("PL4","cpuPower",10,600,[&](uint8_t v){return encoding::decode_pl4(v,p.pl4_doubled());});
    check("cpuTempLimitRead","cpuPower",60,105,[&](uint8_t v){return encoding::decode_cpu_temperature(v,p.cpu_encoding()=="direct",p.tj_max());});
    check("tgp","gpuPower",50,250,[&](uint8_t v){return v+p.tgp_offset();});
    check("dbMaxTgp","gpuPower",0,50,[](uint8_t v){return v;});
    if(gpu_module==0) disable("gpuPower","GPU module not detected");
    for(const auto* reg:{"logoR","logoG","logoB"}) check(reg,"lighting",0,60,[](uint8_t v){return v;});
    for(const auto* f:{"cpuPower","gpuPower","fanControl","battery","lighting"}) if(!r.disabled.contains(f)) r.enabled.insert(f);
    if(!e.cpu_encoding_confirmed && vendor && p.cpu_encoding()!=(*vendor=="amd"?"direct":"inverted_offset")) {
        r.enabled.erase("cpuPower"); r.disabled["cpuPower"]="CPU temperature encoding contradicts CPU vendor without ECSpec confirmation";
    }
    if(e.pl4_unknown) { r.enabled.erase("cpuPower"); r.disabled["cpuPower"]="PL4 encoding unknown (0x727 read failed)"; }
    return r;
}
namespace encoding {
uint8_t pl1(int w) { return static_cast<uint8_t>(std::clamp(w,10,210)); }
uint8_t pl4(int w,bool d) { return static_cast<uint8_t>(std::clamp(d?w/2:w,5,210)); }
int decode_pl4(uint8_t v,bool d) { return d?v*2:v; }
uint8_t tgp(int w,int offset) { return static_cast<uint8_t>(std::clamp(w-offset,0,254)); }
uint8_t dynamic_boost(int w) { return static_cast<uint8_t>(std::clamp(w,5,25)); }
uint8_t cpu_temperature(int c,bool direct,int tj) { return static_cast<uint8_t>(0x80|(direct?std::clamp(c,80,120):tj-std::clamp(c,tj-40,tj))); }
int decode_cpu_temperature(uint8_t v,bool direct,int tj) { return direct?v&127:tj-(v&127); }
uint8_t fan_duty(int p) { return static_cast<uint8_t>(std::clamp(p,0,100)*2); }
int decode_fan_duty(uint8_t v) { return v/2; }
uint8_t switch_speed(bool enabled,int ms) { return enabled?static_cast<uint8_t>(128|std::clamp(ms/100,1,127)):0; }
std::pair<bool,int> decode_switch_speed(uint8_t v) { return {(v&128)!=0,(v&127)*100}; }
uint8_t battery(int mode,uint8_t bit3) { return static_cast<uint8_t>((std::clamp(mode,0,2)<<4)|bit3); }
int decode_battery(uint8_t v) { return (v>>4)&3; }
}
}
