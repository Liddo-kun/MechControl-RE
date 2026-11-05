#include "settings.h"
#include "config_text.h"
#include <windows.h>
#include <iostream>
#include <fstream>

using namespace mech::config;
static void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
template<class F> static std::string fails(F f) { try { f(); } catch(const std::exception& e) { return e.what(); } throw std::runtime_error("Expected failure"); }
static void raw(const std::filesystem::path& p,const std::string& s) { std::ofstream f(p,std::ios::binary); f<<s; if(!f) throw std::runtime_error("Fixture write failed"); }
static ThermalState hold(int actions) { ThermalState s; s.stage=Tripped; s.held=actions; return s; }
struct Lock {
    HANDLE h;
    explicit Lock(const std::filesystem::path& path,bool allow_write=true):h(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|(allow_write?FILE_SHARE_WRITE:0),nullptr,OPEN_EXISTING,0,nullptr)) { require(h!=INVALID_HANDLE_VALUE,"Lock fixture failed"); }
    ~Lock() { CloseHandle(h); }
};
int main(int argc,char** argv) try {
    require(argc==3,"Usage: configuration-check profiles scratch");
    if(std::string_view(argv[1])=="--read-reference") {
        ProfileStore store(argv[2]); auto slot=store.load(1); require(slot.has_value(),"C# slot missing"); slot->preflight("IDY_7Y");
        require(slot->cpu->pl1==80 && !slot->gpu->nvvdd_ma && slot->gpu->msvdd_ma==120000 && !slot->gpu->board_input_ma,"C# CPU/GPU round trip");
        require(slot->cooler->voltage==PumpVoltage::V7 && slot->cooler->rgb_mode==CoolerRgbMode::BreatheColor,"C# cooler slot enum");
        require(store.settings().thermal.held==(Cpu|Fan) && store.settings().last_slot==4 && store.settings().disable_dlppc,"C# settings round trip");
        require(store.load_lighting().keyboard->g==128 && store.load_cooler().address==0xFEDCBA9876543210ULL,"C# peripheral state round trip");
        auto cases=read_json(std::filesystem::path(argv[2])/"compatibility.json");
        for(const auto& c:cases["casing"]) require(lower_invariant(wide(c["input"].get<std::string>()))==wide(c["lower"].get<std::string>()),"Original invariant Unicode casing");
        for(const auto& c:cases["ordinal"]) require(equal_ignore_case(c["left"].get<std::string>(),c["right"].get<std::string>())==c["equal"].get<bool>(),"Original ordinal case comparison");
        for(const auto& c:cases["characters"]) {
            wchar_t unit=static_cast<wchar_t>(c["unit"].get<int>());
            require(letter_or_digit(unit)==c["letterOrDigit"].get<bool>() && blank(std::wstring(1,unit))==c["whiteSpace"].get<bool>(),"Original UTF-16 character categories");
        }
        for(const auto& c:cases["hex"]) {
            if(c["value"].is_null()) fails([&]{hex(c["input"].get<std::string>());});
            else require(hex(c["input"].get<std::string>())==c["value"].get<int>(),"Original 32-bit hex conversion");
        }
        for(const auto& c:cases["bits"]) { Bitfield bit; bit.bit=c["bit"].get<int>(); require(bit.get_mask()==c["mask"].get<int>(),"Original defined byte/shift conversion"); }
        for(const auto& c:cases["dates"]) {
            auto input=c["input"].get<std::string>();
            require(valid_date(input)==c["date"].get<bool>(),("Original DateTime validation: "+input+"; native="+std::to_string(valid_date(input))+"; source="+c["date"].dump()).c_str());
            require(valid_date(input,true)==c["offset"].get<bool>(),("Original DateTimeOffset validation: "+input).c_str());
        }
        for(const auto& c:cases["depth"]) {
            bool accepted=true;try {parse(c["json"].get<std::string>());}catch(...){accepted=false;}
            require(accepted==c["accepted"].get<bool>(),"Original source parser depth rule");
        }
        for(const auto& f:std::filesystem::directory_iterator(argv[2])) {
            if(f.path().filename().string().starts_with("bom-")) {
                auto p=read_json(f.path()).get<UserProfile>();require(p.cpu->pl1==80 && p.name=="snow ☃","Original BOM-decoded file fixture");
            }
            if(f.path().filename().string().starts_with("null-")) fails([&]{auto p=read_json(f.path()).get<UserProfile>();p.preflight("IDY_7Y");});
        }
        std::cout<<"PASS: native reads original C# serialized round-trip fixtures\n"; return 0;
    }
    std::filesystem::path profiles=argv[1],scratch=argv[2];
    // Unique child prevents deletion or overwriting of prior evidence/user files.
    std::filesystem::create_directories(scratch);auto base=scratch;auto stem=std::to_string(GetCurrentProcessId());scratch=base/stem;
    for(unsigned suffix=1;!std::filesystem::create_directory(scratch);++suffix) scratch=base/(stem+"-"+std::to_string(suffix));
    auto area=[&](const char* name) { auto p=scratch/name; std::filesystem::create_directories(p); return p; };
    auto encoding=area("encoding");
    for(auto [width,little]:{std::pair{2,true},std::pair{2,false},std::pair{4,true},std::pair{4,false}}) {
        std::string bytes=width==2?(little?std::string("\xFF\xFE",2):std::string("\xFE\xFF",2)):
            (little?std::string("\xFF\xFE\0\0",4):std::string("\0\0\xFE\xFF",4));
        for(char c:std::string("{\"cpu\":{\"pl1\":80},}")) for(int i=0;i<width;++i) bytes+=i==(little?0:width-1)?c:'\0';
        auto file=encoding/(std::to_string(width)+(little?"le.json":"be.json"));raw(file,bytes);
        require(read_json(file)["cpu"]["pl1"]==80,"BOM-aware UTF-16/32 source input");
    }
    raw(encoding/"utf8.json",std::string("\xEF\xBB\xBF")+"{\"pl1\":80}");require(read_json(encoding/"utf8.json")["pl1"]==80,"UTF-8 BOM input");
    require(parse(std::string(64,'[')+"0"+std::string(64,']')).is_array(),"Source depth 64 accepted");
    fails([&]{parse(std::string(65,'[')+"0"+std::string(65,']'));});
    auto files=manual_profiles(profiles); require(files.size()==46,"Runtime profile count");
    for(const auto& f:files) {
        if(f.stem()=="IDY_7Y_defaults") { fails([&]{load_machine(f);}); continue; }
        auto p=load_machine(f); apply_universal_defaults(p);
        require(!p.expanded_allowlist().empty(),"Allowlist"); p.expanded_skip_verify(); p.expanded_redirects();
        for(const auto& [_,b]:p.bitfields) { hex(b.reg); b.get_mask(); }
    }
    auto select=[&](const std::string& board,std::optional<std::string> vendor,std::optional<int> raw_id) {
        Identity i; i.baseboard=board; i.cpu_vendor=vendor; if(raw_id) i.project_id=static_cast<uint8_t>(*raw_id); return select_profile(profiles,i);
    };
    require(select("YAOSHI Series-X6AR55xY","intel",26).path.stem()=="IDY_7Y","Intel pattern/collision");
    require(select("X6FR5xxY","amd",26).path.stem()=="X6FR5xxY","AMD pattern/collision");
    require(select("","intel",26).path.stem()=="IDY_7Y","Intel shifted ID");
    require(select("","amd",26).path.stem()=="X6FR5xxY","AMD shifted ID");
    require(select("","intel",13).method=="EC direct","Direct before shifted");
    require(select("IDY_7Y","amd",{}).path.empty(),"Filename vendor mismatch");
    require(select("xx-IDY_7Y-xx",{},{}).method=="baseboard filename","Unknown vendor permissive");
    require(select("xxIDY_7Yxx",{},{}).path.empty(),"Filename bounded token");
    Identity unknown; unknown.cpu_vendor="amd";
    auto manual=select_profile(profiles,unknown,[&](const auto& list)->std::optional<std::filesystem::path> { require(list.size()==46,"Unfiltered manual list"); return profiles/"IDY_7Y.json"; });
    require(manual.method=="manual selection","Manual selection");
    require(select_profile(profiles,unknown).path.empty(),"Cancel manual selection");
    auto matching=area("matching");
    auto machine=read_json(profiles/"IDY_7Y.json");
    machine["cpuVendor"]="intel"; machine["baseboardPatterns"]={"short"}; machine["projectIdValue"]=41;
    write_json(matching/"A.json",machine,false); machine["baseboardPatterns"]={"short-long"}; machine["projectIdValue"]=42;
    write_json(matching/"B.json",machine,false); write_json(matching/"C.json",machine,false); raw(matching/"broken.json","{");
    Identity match; match.baseboard="short-long"; match.cpu_vendor="intel"; match.project_id=uint8_t{41};
    require(select_profile(matching,match).path.stem()=="B","Longest pattern before ID; ordinal tie");
    auto unicode_dir=area("unicode-matching");
    auto unicode_profile=machine; unicode_profile["baseboardPatterns"]={"ÄΣᲐK𐐀"};
    write_json(unicode_dir/L"Ä.JSON",unicode_profile,false);
    Identity unicode_id; unicode_id.cpu_vendor="INTEL"; unicode_id.baseboard="äσაk𐐨";
    require(select_profile(unicode_dir,unicode_id).path.filename()==L"Ä.JSON","Invariant Unicode pattern and upper-case extension");
    unicode_id.baseboard="-ä-"; require(select_profile(unicode_dir,unicode_id).method=="baseboard filename","Unicode filename casing");
    unicode_id.baseboard="界ä"; require(select_profile(unicode_dir,unicode_id).path.empty(),"Unicode letter is not a filename-token boundary");
    unicode_id.baseboard="٥ä"; require(select_profile(unicode_dir,unicode_id).path.empty(),"Unicode decimal digit is not a token boundary");
    unicode_id.baseboard="Ⅷä"; require(!select_profile(unicode_dir,unicode_id).path.empty(),"Letter-number category is a token boundary in char.IsLetterOrDigit");
    auto length_dir=area("utf16-pattern-length");
    unicode_profile["baseboardPatterns"]={"é"}; write_json(length_dir/"A.json",unicode_profile,false);
    unicode_profile["baseboardPatterns"]={"ab"}; write_json(length_dir/"B.json",unicode_profile,false);
    unicode_id.baseboard="éab"; require(select_profile(length_dir,unicode_id).path.stem()=="B","Longest match counts UTF-16 units, not UTF-8 bytes");
    require(hex("0x10000")==65536 && hex("FFFFFFFF")==-1 && hex("0x0xFF")==255 && hex("+0XFF")==255,"Source hex rules without arbitrary address ceiling");
    fails([&]{hex("100000000");}); fails([&]{hex(" FF ");}); fails([&]{hex("-1");});
    Bitfield mask; mask.mask="0x1FF"; mask.bit=0; require(mask.get_mask()==255,"Explicit mask takes priority and casts to byte");
    auto invalid=area("invalid");
    raw(invalid/"oem.json",R"({"projectId":"archive","registers":{"x":"0x740"}})");
    fails([&]{load_machine(invalid/"oem.json");});
    auto minimal=machine; minimal["limits"]=Json::object(); minimal["bitfields"]=Json::object(); minimal["verifyRedirect"]=Json::object(); minimal["batteryModes"]=Json::array();
    auto p=minimal.get<MachineProfile>(); apply_universal_defaults(p);
    require(p.tgp_fallback && p.cpu_fallback && p.limits.size()==9,"Whole-map conservative defaults");
    auto partial=machine; partial["limits"]={{"PL1",{{"max",100}}}}; partial["verifyRedirect"]={{"0x123",{{"readAddr","0x124"}}}};
    auto pp=partial.get<MachineProfile>(); apply_universal_defaults(pp);
    require(pp.limits.size()==1 && pp.redirects.size()==1 && !pp.tgp_fallback,"No individual map-entry merging");
    fails([&]{ auto bad=machine; bad["limits"]=nullptr; bad.get<MachineProfile>(); });
    auto tolerant=parse("{/*c*/\"cpu\":{\"pl1\":123,},\"CPU\":{\"pl1\":44},\"unknown\":3,}").get<UserProfile>();
    require(tolerant.cpu->pl1==123 && !Json(tolerant).contains("unknown"),"JSON comments/trailing/case/unknown");
    for(const auto& text:{R"({"cpu":{"pl1":null}})",R"({"cpu":{"pl1":true}})",R"({"cpu":{"pl1":1.0}})",R"({"cpu":{"pl1":2147483648}})",R"({"lighting":{"logoR":256}})",R"({"liquidCooler":{"pumpVoltage":"V8"}})"})
        fails([&]{ parse(text).get<UserProfile>(); });
    for(const auto& text:{"[,]","{,}","[1,,]","{\"a\":,}","/*unterminated"}) fails([&]{parse(text);});
    require(parse(R"({"s":"comma,} /*literal*/ quote\"", "a":[1,/*comment*/],})")["a"].size()==1,"Trailing-comma scanner respects strings/comments");
    for(const auto& date:{"2026-11-03","2026-11-03T12:00","2026-11-03T12:00:00","2026-11-03T12:00:00.1234567890123456Z","2026-11-03T12:00:00+01"}) require(valid_date(date),"Reference ISO date form accepted");
    require(!valid_date("2026-11-03t12:00:00z") && !valid_date("2026-11-03T12:00:00+14:01") && !valid_date("2026-02-29"),"Invalid dates refused");
    auto defaults=parse("{}").get<UserProfile>(); defaults.preflight("IDY_7Y");
    require(defaults.fan->switch_ms==100 && defaults.protection->threshold==50 && !defaults.cooler,"Omitted defaults");
    auto null_cpu=parse(R"({"cpu":null})").get<UserProfile>(); require(!null_cpu.cpu,"Explicit null preserved"); fails([&]{null_cpu.preflight("IDY_7Y");});
    auto foreign=defaults; foreign.project_id="X6FR5xxY"; fails([&]{foreign.preflight("IDY_7Y");});
    foreign.project_id="idy_7y"; foreign.preflight("IDY_7Y"); foreign.project_id=""; foreign.preflight("IDY_7Y");
    foreign.project_id="Ä_MODEL"; foreign.preflight("ä_model");
    foreign.project_id="K"; fails([&]{foreign.preflight("k");});
    foreign.project_id="\xC2\xA0"; foreign.preflight("IDY_7Y");
    require(!defaults.gpu->bios_override_enabled && !defaults.gpu->nvvdd_ma && Json(defaults)["gpu"]["boardInputMilliamps"].is_null(),"Nullable GPU rails preserved");
    Hardware h=collect_hardware([](int a)->uint8_t { if(a==0x727)return 128; if(a==0x7A4)return 128; if(a==0x730)return 80; if(a==0x731)return 100; if(a==0x732)return 100; if(a==0x7D8)return 95; return 0; },
        [](int i)->uint32_t { const uint32_t b[]{150,160,180,95,50,25,87,90,255,187,0,0}; return b[i]; });
    require(h.decoded() && h.liquid_cooling() && h.pl4_doubled()==true,"Hardware capture and capabilities");
    p=load_machine(profiles/"IDY_7Y.json"); apply_universal_defaults(p); auto enriched=enrich(p,h);
    require(p.cpu_encoding()=="direct" && p.tgp_offset()==90 && p.limits.at("PL4").max==360 && p.redirects.at("0x786").transform=="direct" && enriched.cpu_encoding_confirmed,"ECSpec enrichment");
    require(p.modes && p.modes->at("gaming").tcc_offset==95 && p.modes->at("gaming").pl4==200,"EC mode defaults decode");
    p=load_machine(profiles/"IDY_7Y.json"); p.verified=true; p.modes=std::map<std::string,Mode>{}; apply_universal_defaults(p); auto verified_max=p.limits.at("PL1").max; enrich(p,h);
    require(p.limits.at("PL1").max==verified_max && p.cpu_encoding()=="direct" && p.modes->empty(),"Verified limits; hardware encoding; empty modes not replaced");
    h.ecspec->at(9)=0; require(h.extract_limits().empty(),"ECSpec marker validated");
    auto read=[](int a)->uint8_t { if(a==0x463)return 10; if(a==0x7A6)return 8; if(a==0x743)return 7; if(a==0x744)return 0; if(a==0x746)return 25; if(a>=0x769 && a<=0x76B)return 0; return 100; };
    p=load_machine(profiles/"IDY_7Y.json"); p.verified=true; apply_universal_defaults(p);
    require(!validate_startup(p,read,1,{},"amd").enabled.contains("cpuPower"),"Verified vendor contradiction gated");
    require(validate_startup(p,read,1,{},"intel").enabled.contains("cpuPower"),"Matching vendor enabled");
    p.verified=false;
    require(!validate_startup(p,read,0,{},"intel").enabled.contains("gpuPower"),"Absent GPU gated");
    auto sentinel=[](int)->uint8_t{return 0;};
    require(!validate_startup(p,sentinel,1,{},"intel").enabled.contains("cpuPower"),"Unverified sanity gate");
    Enrichment ec; ec.limits={"PL1"};
    require(validate_startup(p,sentinel,1,ec,"intel").enabled.contains("cpuPower"),"ECSpec stronger than sentinel");
    require(encoding::cpu_temperature(95,false)==0x8A && encoding::cpu_temperature(95,true)==0xDF,"CPU encode");
    require(encoding::decode_cpu_temperature(0x8A,false)==95 && encoding::decode_cpu_temperature(0xDF,true)==95,"CPU read high bit mask");
    require(encoding::pl4(200,true)==100 && encoding::pl4(200,false)==200 && encoding::battery(2,0)==32 && encoding::battery(2,8)==40,"Runtime encodings");
    require(encoding::tgp(150,90)==60 && encoding::switch_speed(true,300)==0x83 && encoding::fan_duty(100)==200,"Other encodings");

    auto persistence=area("persistence"); ProfileStore store(persistence);
    require(store.settings().thermal.held==0 && store.settings().last_slot==0,"Missing settings clean");
    auto slot=defaults; slot.name="Native fixture \xE2\x98\x83"; slot.project_id="IDY_7Y"; slot.saved_at="2026-11-03T12:00:00+00:00";
    slot.cpu=CpuSettings{80,100,200,95}; slot.cooler=CoolerSettings{}; slot.cooler->voltage=PumpVoltage::V7; slot.cooler->rgb_mode=CoolerRgbMode::BreatheColor;
    slot.gpu->msvdd_ma=120000; slot.gpu->bios_override_enabled=false;
    for(int i=1;i<=5;++i) { store.save(i,slot); require(store.exists(i) && store.load(i)->cpu->pl1==80,"Five slots real round trip"); }
    auto slot_before=read_json(persistence/"userprofile_4.json");
    { Lock lock(persistence/"userprofile_4.json"); auto changed=slot; changed.cpu->pl1=123; fails([&]{store.save(4,changed);}); }
    require(read_json(persistence/"userprofile_4.json")==slot_before && store.settings().last_slot==5,"Failed slot replacement preserves committed slot and selection");
    store.rename(3,"Renamed"); require(store.load(3)->name==slot.name,"Rename independent of slot contents");
    store.set_disable_dlppc(true); require(ProfileStore(persistence).settings().disable_dlppc,"Global DLPPC persistence");
    store.rename(5,"Keep name");
    auto before=read_json(persistence/"settings.json");
    {
        Lock lock(persistence/"settings.json");
        require(fails([&]{store.save(2,slot);}).find("was saved, but selected-slot")!=std::string::npos,"Partial slot/selection error");
        require(store.settings().last_slot==5 && store.load(2).has_value(),"Selection unchanged after slot committed");
        fails([&]{store.rename(1,"Failed");}); require(store.settings().names[0]=="Profile 1","Rename rollback");
        fails([&]{store.set_disable_dlppc(false);}); require(store.settings().disable_dlppc,"DLPPC rollback");
        require(read_json(persistence/"settings.json")==before,"Committed settings retained on replace failure");
        fails([&]{store.clear(5);});
        require(store.exists(5) && store.settings().last_slot==5 && store.settings().names[4]=="Keep name","Failed clear restores only changed selection/name and retains file");
    }
    // A slot temporary file is never a load candidate.
    write_json(persistence/"userprofile_1.json.tmp",slot,false); store.clear(1); require(!store.load(1),"Slot pending not auto-loaded");
    store.select(3); store.rename(3,"Delete failure");
    { Lock lock(persistence/"userprofile_3.json"); fails([&]{store.clear(3);}); }
    require(store.exists(3) && store.settings().last_slot==0 && store.settings().names[2]=="Profile 3","Clear deletion failure is not a cross-file transaction");
    auto legacy=Json(slot); legacy["gpuFailsafe"]={{"enabled",false},{"thresholdC",35},{"fanBoost",true},{"reduceCpuPower",true},{"showMainWindow",false}};
    write_json(persistence/"userprofile_1.json",legacy,false);
    auto migrated=store.load(1);
    require(migrated->requires_save && migrated->protection->threshold==50 && !migrated->protection->enabled && migrated->protection->fan_boost && migrated->protection->reduce_cpu && !migrated->protection->show_window,"Legacy threshold retains options");
    require(read_json(persistence/"userprofile_1.json")["gpuFailsafe"]["thresholdC"]==35,"Migration does not save");
    auto invalid_threshold=legacy; invalid_threshold["gpuFailsafe"]["thresholdC"]=96;
    write_json(persistence/"userprofile_1.json",invalid_threshold,false); fails([&]{store.load(1);});
    write_json(persistence/"userprofile_1.json",legacy,false);
    auto operational=area("operational"); ProfileStore thermal(operational);
    auto trip=hold(Cpu|Fan); Trip history; history.reason="native fixture"; history.actions=Cpu|Fan; history.time="2026-11-03T12:00:00+00:00"; trip.last_trip=history;
    thermal.save_thermal(trip);
    {
        Lock lock(operational/"settings.json");
        fails([&]{thermal.save_thermal({});}); require(thermal.settings().thermal.held==(Cpu|Fan),"Failed release retains write authority");
        require(ProfileStore(operational).settings().thermal.held==(Cpu|Fan),"Pending release cannot erase committed hold");
    }
    thermal.save_thermal({}); require(ProfileStore(operational).settings().thermal.held==0,"Durable release");
    {
        Lock lock(operational/"settings.json");
        fails([&]{thermal.save_thermal(hold(Gpu));}); require(thermal.settings().thermal.held==Gpu,"Failed trip retains in-memory hold");
        require(ProfileStore(operational).settings().thermal.held==Gpu,"Pending trip recovered");
    }
    thermal.save_thermal(trip); write_json(operational/"settings.json.tmp",Json{{"thermalProtection",hold(Gpu)}},false);
    require(ProfileStore(operational).settings().thermal.held==All,"Union committed and pending obligations");
    auto uncertain_pending=area("uncertain-pending");
    write_json(uncertain_pending/"settings.json",Json{{"thermalProtection",ThermalState{}}},false);
    ThermalState invalid_pending; invalid_pending.stage=Released; invalid_pending.held=Cpu|Fan;
    write_json(uncertain_pending/"settings.json.tmp",Json{{"thermalProtection",invalid_pending}},false);
    require(ProfileStore(uncertain_pending,false).settings().thermal.held==(Cpu|Fan),"Invalid pending record cannot discard known non-GPU obligations");
    auto failed_create=area("failed-create"); ProfileStore create_store(failed_create); create_store.save_thermal(hold(Cpu));
    std::filesystem::create_directory(failed_create/"settings.json.tmp");
    fails([&]{create_store.save_thermal({});}); require(create_store.settings().thermal.held==Cpu,"Failed temp creation retains release authority");
    require(ProfileStore(failed_create,false).settings().thermal.held==Cpu,"Unreadable pending state preserves known hold without GPU fallback");
    auto inspect=[&](const std::string& text,bool gpu=true) { auto d=area(("case-"+std::to_string(GetTickCount64())+"-"+std::to_string(std::hash<std::string>{}(text))).c_str()); raw(d/"settings.json",text); return ProfileStore(d,gpu).settings(); };
    require(inspect(R"({"thermalProtection":{"Stage":1,"Held":7}})").thermal.held==0,"Retired stage released");
    require(inspect(R"({"lastProfile":"bad","profileNames":[null," ","Custom"],"disableDlppc":null})").thermal.held==0,"Cosmetic corruption not operational");
    require(inspect(R"({"profileNames":["\u00a0","Custom","\u2007","\u3000","\u202f"]})").names[0]=="Profile 1","Unicode whitespace-only names repaired");
    require(inspect(R"({"thermalProtection":{"Stage":2,"Held":2,"LastTrip":{"Time":"bad","ThresholdC":"bad","Results":null}}})").thermal.held==Cpu,"Optional-history corruption retains exact hold");
    require(inspect(R"({"thermalProtection":{"Held":2}})").thermal.held==(Gpu|Cpu),"Missing stage uncertain retains known held");
    require(inspect(R"({"thermalProtection":null})").thermal.held==Gpu,"Null operational group uncertain");
    require(inspect(R"({"thermalProtection":{"Stage":3,"Held":4}})").thermal.held==(Gpu|Fan),"Illegal operational stage preserves known hold");
    require(inspect(R"({"thermalProtection":{"Stage":2,"Held":8}})").thermal.held==Gpu,"Invalid action mask becomes recovery hold");
    require(inspect("{").thermal.held==Gpu && inspect("{",false).thermal.held==0,"Corrupt settings model-derived fallback");
    require(inspect(R"({"thermalProtection":{"Stage":2,"Held":4},"gpuFailsafeTripReason":"old"})",false).thermal.held==Fan,"New state precedence; known held without GPU fallback");
    require(inspect(R"({"gpuFailsafeTripReason":"legacy"})").thermal.held==Gpu,"Legacy GPU hold migration");
    auto bounded=hold(Fan); bounded.recovery_details=std::string(9000,'x'); require(recover(bounded,{},false,true).recovery_details.size()==8192,"Bound recovery details");
    std::vector<std::string> diagnostics;
    ProfileStore state_store(persistence,true,[&](const std::string& message){diagnostics.push_back(message);});
    auto warning_before=state_store.warning(); auto diagnostic_count=diagnostics.size();
    LightingState lights; lights.keyboard->enabled=true; lights.keyboard->g=128; lights.idle_seconds=30;
    state_store.save_lighting(lights); require(state_store.load_lighting().keyboard->enabled,"Lighting direct round trip");
    raw(persistence/"lighting_state.json",R"({"keyboard":null})"); require(!state_store.load_lighting().keyboard,"Explicit-null lighting defect remains visible");
    raw(persistence/"lighting_state.json","null"); require(state_store.load_lighting().keyboard.has_value(),"Whole-null lighting defaults");
    raw(persistence/"liquid_cooler_state.json","{"); require(state_store.load_cooler().g==128,"Corrupt cooler default");
    require(diagnostics.size()==diagnostic_count+1 && state_store.warning()==warning_before,"Peripheral load failure logs only, without global configuration warning");
    CoolerState cooler; cooler.address=0xFEDCBA9876543210ULL; cooler.device_name="Native fixture"; cooler.voltage=PumpVoltage::V12;
    state_store.save_cooler(cooler); require(state_store.load_cooler().address==cooler.address,"Cooler uint64 round trip");
    require(Json(cooler)["pumpVoltage"]==1 && Json(*slot.cooler)["pumpVoltage"]==2 && Json(*slot.cooler)["rgbMode"]==3,"Cooler numeric enums wire order");
    { Lock lock(persistence/"liquid_cooler_state.json",false); state_store.save_cooler(cooler); }
    require(diagnostics.size()==diagnostic_count+2 && diagnostics.back().find("save failed")!=std::string::npos && state_store.warning()==warning_before,"Direct save failure logs only, with no throw or global warning");
    { Lock lock(persistence/"lighting_state.json",false); state_store.save_lighting(lights); }
    require(diagnostics.size()==diagnostic_count+3 && state_store.warning()==warning_before,"Lighting failure uses the same log-only contract");
    // Stable, native-written interoperability inputs, consumed by the isolated C# harness.
    auto output=area("interop");
    write_json(output/"userprofile.json",slot,true); write_json(output/"lighting_state.json",lights,false); write_json(output/"liquid_cooler_state.json",cooler,false);
    Settings exported; exported.thermal=trip; exported.last_slot=4; exported.disable_dlppc=true; write_json(output/"settings.json",exported,true);
    auto identity=collect_identity();
    auto detected=select_profile(profiles,identity);
    Json identity_json={{"baseboard",identity.baseboard},{"cpuName",identity.cpu_name},{"cpuVendor",nullable(identity.cpu_vendor)},
        {"selectedProfile",detected.path.filename().string()},{"method",detected.method}};
    write_json(scratch/"identity.json",identity_json,false);
    std::cout<<"PASS: 45 machine profiles + rejected reference-only file; detection, encodings, model preflight, migrations, actual failed/interrupted saves, thermal recovery, peripheral state\n"
        <<"Evidence: "<<scratch.string()<<"\nIdentity: "<<identity_json.dump()<<"\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
