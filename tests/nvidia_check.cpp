#include "nvidia.h"
#include "win_handle.h"
#include "config_text.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace mech;
using namespace mech::nv;
namespace {
void check(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);std::cout<<"PASS "<<message<<'\n';}
template<class F> void refuses(F f,const char* message) {bool failed=false;try {f();} catch(const std::exception&) {failed=true;}check(failed,message);}
// Structural ROM from the source BiosFixture, not an OEM image.
struct Fixture {
    Bytes b;int legacy,efi,bit,token,table,profile;
    explicit Fixture(bool relocated=false) {
        legacy=relocated?0x1800:0x1000;efi=relocated?0x600:0x400;bit=relocated?0x300:0x100;token=bit+0x100;table=legacy+(relocated?0x300:0x100)+efi;profile=legacy+0x1000+efi;b.resize(legacy+efi+0x3000);
        auto text=[&](int p,std::string_view v){std::copy(v.begin(),v.end(),b.begin()+p);};
        auto image=[&](int p,int size,uint8_t type,bool last) {u16(p,0xaa55);u16(p+0x18,0x40);text(p+0x40,"PCIR");u16(p+0x44,0x10de);u16(p+0x46,relocated?0x5678:0x1234);u16(p+0x4a,0x18);u16(p+0x50,uint16_t(size/512));b[p+0x54]=type;text(p+0x60,"NPDE");u16(p+0x64,0x101);u16(p+0x66,12);u16(p+0x68,uint16_t(size/512));b[p+0x6a]=last?128:0;};
        image(0,legacy,0,false);image(legacy,efi,3,false);image(legacy+efi,0x3000,0xe0,true);
        b[bit]=255;b[bit+1]=0xb8;text(bit+2,std::string_view("BIT\0",4));u16(bit+6,0x100);b[bit+8]=12;b[bit+9]=6;b[bit+10]=2;
        unsigned sum=0;for(int i=0;i<11;++i) sum+=b[bit+i];b[bit+11]=uint8_t(-int(sum));
        auto entry=[&](int p,uint8_t id,uint16_t size,int pointer) {b[p]=id;b[p+1]=2;u16(p+2,size);u16(p+4,uint16_t(pointer));};entry(bit+12,'B',5,bit+0x80);entry(bit+18,'P',0xf8,token);
        const uint8_t ver[]{3,2,1,0x97,4};std::copy(std::begin(ver),std::end(ver),b.begin()+bit+0x80);if(relocated) b[bit+0x83]=0x96;
        put(b,token+0x2c,table-efi);put(b,token+0xf4,profile-efi);b[table]=0x40;b[table+1]=0x38;b[table+2]=0x66;b[table+3]=20;
        policy(2,1,2,0,10000,130000,220000);policy(9,3,1,6,1,11000,11000);policy(11,6,1,3,1,1000000,1000000);policy(12,6,1,4,1,1000000,1000000);policy(13,7,1,3,1000,150000,150000);policy(14,7,1,4,1000,40000,40000);
        const uint8_t header[]{0x10,0x0a,5,4,7,8,1,0x10,9,8};std::copy(std::begin(header),std::end(header),b.begin()+profile);checksum();
    }
    void u16(int p,uint16_t v) {b[p]=uint8_t(v);b[p+1]=uint8_t(v>>8);}
    int e(int i) const {return table+56+i*102;}
    void policy(int i,uint8_t type,uint32_t flags,uint8_t channel,uint32_t lo,uint32_t def,uint32_t hi) {auto p=e(i);b[p]=type;put(b,p+1,flags);b[p+5]=channel;put(b,p+13,lo);put(b,p+17,def);put(b,p+21,hi);}
    void checksum() {for(auto [p,size]:{std::pair(0,legacy),std::pair(legacy,efi)}) {b[p+size-1]=0;unsigned sum=0;for(int j=p;j<p+size;++j) sum+=b[j];b[p+size-1]=uint8_t(-int(sum));}}
};
struct Driver {
    Bytes info=packet(0xa70,0x40a70),monitor=packet(0xca8,0x30ca8),control=packet(0xa4c,0x10a4c),status=packet(0xb00b8,0xb00b8),last_get,last_set;
    Bytes therm_info=packet(0x94c,0x1094c),therm_status=packet(0xa48,0x10a48);
    Bytes d_info=packet(0x164c,0x1164c),d_control=packet(0x1548,0x11548),d_set;
    int nv,ms,input,current,power,board,input_channel,sets=0;bool ignore=false,change_unselected=false;
    explicit Driver(bool alternate=false) {
        nv=alternate?17:3;ms=alternate?2:11;input=alternate?21:6;current=alternate?4:12;power=alternate?28:0;board=alternate?9:1;input_channel=alternate?27:13;
        int nc=alternate?15:2,mc=alternate?4:19;
        put(monitor,16,(1u<<nc)|(1u<<mc)|(1u<<input_channel));policy(nv,19,nc,1000,120000,170000);policy(ms,19,mc,1000,30000,45000);policy(input,4,input_channel,1,9000,15000);
        auto channel=[&](int c,uint32_t kind,uint32_t rail){put(monitor,0x28+c*0x3c+12,kind);put(monitor,0x28+c*0x3c+16,rail);};channel(nc,6,1);channel(mc,6,16);channel(input_channel,8,0xda);
        for(size_t i=4;i<control.size();++i) control[i]=uint8_t(i*37+11);
        for(auto [i,type,value]:{std::tuple(nv,19,125000),std::tuple(ms,19,32000),std::tuple(input,4,9500)}) {put(control,0x1c+i*0x28,type);put(control,0x20+i*0x28,value);}
        policy(board,0,0,5000,95000,250000,0);policy(current,0x15,26,1,5001000,5001000);policy(power,0x15,5,1,5001000,5001000,0);
        value(nv,19,126883);value(ms,19,47772);value(input,4,8470);value(board,0,173336);
        put(status,entry(nv)+4,125000);put(status,entry(ms)+4,32000);put(status,entry(input)+4,9500);
        controller(current,2865000,126883,47772,152544,59298);controller(power,2940000,85576,47526,121715,71465);status[entry(power)+0xe1]=1;status[entry(power)+0xe0+0x48]=uint8_t(board);
        put(therm_info,4,0x3f);put(therm_info,8,108000);for(int m=0;m<6;++m) {put(therm_info,76+m*72,m==2 || m==5?5:4);put(therm_status,72+m*80,m==2 || m==5?5:4);}
        put(d_info,4,1u<<3);d_info[24]=3;put(d_info,76+3*176+8,7);put(d_info,76+3*176+48,0x52);put(d_control,72+3*168,0);d_control[72+3*168+4]=1;
    }
    static int entry(int i) {return 0x11b8+i*0x3768;}
    void policy(int i,uint32_t type,int channel,uint32_t lo,uint32_t def,uint32_t max,uint32_t unit=1) {put(info,4,u32(info,4)|(1u<<i));int p=0x3c+i*0x58;put(info,p,type);put(info,p+4,channel);put(info,p+8,unit);put(info,p+12,lo);put(info,p+16,def);put(info,p+20,max);}
    void value(int i,uint32_t type,uint32_t v) {put(status,entry(i),type);put(status,entry(i)+8,v);}
    void controller(int i,uint32_t cap,uint32_t a,uint32_t b,uint32_t pa,uint32_t pb) {int u=entry(i)+0x968;put(status,entry(i),0x15);put(status,u+4,cap);put(status,u+0x24,3090000);put(status,u+0x60,a+b);put(status,u+0x134,a);put(status,u+0xb84,b);put(status,u+0x15d0,pa+pb);put(status,u+0x1690,pa);put(status,u+0x1e70,pb);}
    void count(int m,uint64_t v) {memcpy(therm_status.data()+80+m*80,&v,8);}
    void call(uint32_t id,Bytes& b) {
        switch(id) {
        case 0x67f31384:b=info;break;case 0xc12eb19e:b=monitor;break;
        case 0x8b3e7343:{auto mask=u32(b,4);b=control;put(b,4,mask);last_get=b;break;}
        case 0xaffc2279:last_set=b;++sets;if(!ignore) for(int i=0;i<32;++i) if(u32(b,4)&(1u<<i)) put(control,0x20+i*0x28,u32(b,0x20+i*0x28));if(change_unselected) put(control,0x20+nv*0x28,126000);break;
        case 0x31b7a4cd:b=status;break;case 0xb2c9d666:b=therm_info;break;case 0x4b4bd039:b=therm_status;break;
        case 0x1cac3865:b=d_info;break;case 0x69de2c54:{auto mask=u32(b,4);b=d_control;put(b,4,mask);break;}
        case 0x4d1c7d6e:d_set=b;d_control=b;break;
        default:throw std::runtime_error("Unsupported fixture call");
        }
    }
};
void packets() {
    for(bool alternate:{false,true}) {
        Driver d(alternate);Api api([&](auto id,auto& b){d.call(id,b);});auto before=api.currents();auto after=api.set_currents({}, {},12500);
        auto expected=d.last_get;put(expected,4,1u<<d.input);put(expected,0x20+d.input*0x28,12500);
        check(after.nv==before.nv && after.ms==before.ms && after.input->current==12500 && expected==d.last_set,"current SET preserves complete GET metadata and unselected rails at relocated indices");
        auto t=api.telemetry();check(t.nv==126883 && t.ms==47772 && t.input==8470u && t.nv_prediction==152544u && t.current_khz==2865000u && t.power_khz==2940000u && t.gpu_mw==173336u && t.core_mw==133102u,"telemetry identifies controllers by validated policy/rail relationships");
        put(d.status,Driver::entry(d.current)+0x968+0x60,1);t=api.telemetry();check(!t.nv_prediction && !t.current_khz && t.power_khz==2940000u,"unknown current-controller layout omits only affected predictions/cap");
        auto old=d.d_control;api.set_dlppc(false);put(old,4,1u<<3);old[72+3*168+4]=0;check(d.d_set==old && api.dlppc()->active==false,"DLPPC SET changes only selected activation byte and retains packet metadata");
    }
    for(int mutation=0;mutation<7;++mutation) {
        Driver d;switch(mutation) {case 0:put(d.info,4,u32(d.info,4)&~(1u<<d.input));break;case 1:put(d.info,0x3c+d.input*0x58,25);break;case 2:put(d.info,0x3c+d.input*0x58+8,0);break;case 3:put(d.monitor,0x28+d.input_channel*0x3c+12,7);break;case 4:put(d.monitor,16,u32(d.monitor,16)&~(1u<<d.input_channel));break;case 5:d.policy(20,4,d.input_channel,1,9000,15000);break;case 6:put(d.info,0x3c+d.input*0x58+16,0);break;}
        Api api([&](auto id,auto& b){d.call(id,b);});check(!api.currents().input,"unsupported/ambiguous board input leaves NVVDD/MSVDD available");refuses([&]{api.set_currents({}, {},12000);},"unsupported board input refuses SET");check(api.set_currents(126000,{},{}).nv.current==126000,"other rails remain writable without board input");
    }
    for(bool corrupt:{false,true}) {Driver d;d.ignore=!corrupt;d.change_unselected=corrupt;Api a([&](auto id,auto& b){d.call(id,b);});refuses([&]{a.set_currents({}, {},12000);},"missing selected readback or changed unselected rail is a failure");}
    Driver d;Api a([&](auto id,auto& b){d.call(id,b);});check(!a.telemetry().ipc,"IPC first read is unavailable");d.count(2,54000000);d.count(3,900000000000);Sleep(600);auto t=a.telemetry();check(t.ipc && *t.ipc>0.3 && *t.ipc<0.95,"IPC uses busiest ADC_IPC monitor, not unrelated classes");d.count(2,0);check(!a.telemetry().ipc,"IPC counter reset invalidates interval");check(a.telemetry().ipc==0,"IPC resumes from reset baseline");
    const uint8_t* storage=nullptr;int statuses=0;Api reused([&](auto id,auto& b){if(id==0x31b7a4cd) {if(storage) check(storage==b.data(),"large telemetry packet storage is reused");storage=b.data();++statuses;}d.call(id,b);});
    for(int i=0;i<3;++i) reused.telemetry();check(statuses==3,"telemetry reuse still performs fresh status reads");
    put(d.info,0,0x50a70);refuses([&]{a.currents();},"unknown policy ABI stays unsupported");
}
void bios_and_recovery(const std::filesystem::path& root) {
    for(bool moved:{false,true}) {Fixture f(moved);auto b=Bios::parse(f.b);check(b.power.max==220000 && b.nv.def==150000 && b.input->def==11000,"BIOS mapping works at relocated image/table coordinates");
        auto patches=b.build({295000,RailOverride{170000,210000},RailOverride{45000,56000},RailOverride{12000,15400}});check(patches.size()==7,"BIOS builds only seven supported direct patches");
        auto changed=f.b;for(auto& p:patches) put(changed,u32(p.data,5),u32(p.data,10));check(Bios::parse(changed).nv.def==170000,"patched BIOS reparses with exact fields");}
    for(int mutation=0;mutation<8;++mutation) {Fixture f;switch(mutation) {case 0:f.b.resize(256);break;case 1:f.b[0]=0;break;case 2:f.b[f.table+2]=0x67;break;case 3:f.b[f.e(7)]=1;break;case 4:f.b[f.e(13)+5]=f.b[f.e(14)+5];break;case 5:f.b[f.profile+10]=1;break;case 6:put(f.b,f.e(9)+17,0);break;case 7:f.b[f.bit+11]^=1;f.checksum();break;}refuses([&]{Bios::parse(f.b);},"malformed/unknown BIOS refuses rather than inferring support");}
    Fixture fixture;auto original=Bios::parse(fixture.b);auto records=original.build({295000,RailOverride{160000,210000},{},{}});auto active=fixture.b;for(auto& r:records) put(active,u32(r.data,5),u32(r.data,10));
    auto key_name=L"Software\\MechControl.Native.NvidiaFixture."+std::to_wstring(GetCurrentProcessId());HKEY key=nullptr;if(RegCreateKeyExW(HKEY_CURRENT_USER,key_name.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key,nullptr)) throw std::runtime_error("Fixture registry create failed");
    auto path=root/"originals.txt";
    {
        Store store(key,path);store.stage(original,records);check(store.matches(records) && std::filesystem::exists(path),"flushed decimal text originals precede staged registry records");bool staged=false,discarded=false;auto recovered=store.open(active,staged,discarded);
        check(staged && !discarded && recovered.image==fixture.b,"owned originals restore the exact unmodified image");
        auto canonical=config::read_text(path);std::istringstream lines(canonical);std::string version,line,compatible;std::getline(lines,version);compatible=version+"\r\n";
        while(std::getline(lines,line)) {auto equal=line.find('=');compatible+=" \t+"+line.substr(0,equal)+" = +"+line.substr(equal+1)+" \t\r\n";}
        for(int encoding=0;encoding<5;++encoding) {
            auto input=compatible;if(encoding==2 || encoding==4) input.erase(std::remove(input.begin(),input.end(),'\n'),input.end());
            auto wide=config::wide(input);
            std::string bytes;
            if(encoding==0) bytes="\xef\xbb\xbf"+compatible;
            else {
                bool little=encoding%2!=0;int width=encoding<=2?2:4;
                if(width==2) bytes=little?std::string("\xff\xfe",2):std::string("\xfe\xff",2);
                else bytes=little?std::string("\xff\xfe\0\0",4):std::string("\0\0\xfe\xff",4);
                for(auto c:wide) for(int j=0;j<width;++j) bytes.push_back(static_cast<char>((uint32_t(c)>>(8*(little?j:width-1-j)))&255));
            }
            {std::ofstream out(path,std::ios::binary);out.write(bytes.data(),bytes.size());}
            check(store.open(active,staged,discarded).image==fixture.b,"BOM-detected recovery text accepts decimal whitespace/plus and CR/LF lines without changing ownership");
        }
        {std::ofstream out(path,std::ios::binary);out<<canonical;}
        // Simulate an older recovery writer lacking newly supported board fields.
        std::ifstream in(path);std::string text((std::istreambuf_iterator<char>(in)),{});in.close();auto marker=std::to_string(original.input->offset+4)+"=";auto start=text.find('\n'+marker);auto end=text.find('\n',start+1);text.erase(start,end==std::string::npos?text.size()-start:end-start);std::ofstream(path)<<text;
        store.stage(recovered,records);std::ifstream complete(path);std::string now((std::istreambuf_iterator<char>(complete)),{});check(now.find(marker+std::to_string(original.input->def))!=std::string::npos,"new recovery fields append without replacing existing originals");complete.close();
        refuses([&]{store.delete_recovery();},"cannot delete originals while any override remains staged");
        store.stage(original,{});auto pending=store.open(active,staged,discarded);check(!staged && !discarded && pending.image==fixture.b && std::filesystem::exists(path),"removed records with still-active patches retain recovery until stock is verified");
        auto stock=store.open(fixture.b,staged,discarded);check(!staged && discarded && stock.image==fixture.b && !std::filesystem::exists(path),"driver-reinstalled stock state discards only obsolete owned recovery");
        auto foreign=records.front().data;foreign[0]^=1;RegSetValueExW(key,L"romOverride00",0,REG_BINARY,foreign.data(),DWORD(foreign.size()));refuses([&]{store.open(fixture.b,staged,discarded);},"foreign ROM records are never adopted");RegDeleteValueW(key,L"romOverride00");
        RegSetValueExW(key,L"vbios",0,REG_SZ,reinterpret_cast<const BYTE*>(L"external"),18);refuses([&]{store.stage(original,records);},"external vbios refuses before staging");RegDeleteValueW(key,L"vbios");
        store.stage(original,records);auto newer=fixture;newer.b[newer.bit+0x83]=0x96;newer.checksum();store.open(newer.b,staged,discarded);check(discarded && !staged && !std::filesystem::exists(path),"changed VBIOS discards only old owned records");
    }
    RegDeleteKeyW(HKEY_CURRENT_USER,key_name.c_str());
    Budget baseline{175000,250000},raised=baseline.raise(220000,baseline);for(int i=0;i<20;++i) {raised.f7=175000;raised=raised.raise(220000,baseline);}check(raised==Budget{220000,295000},"firmware F7-only resets do not accumulate platform headroom");
    config::GpuSettings g;g.tgp=180;g.bios_override_enabled=true;g.nvvdd_ma=200000;Currents c{{1000,120000,130000,125000,170000},{1000,30000,45000,32000,45000},{}};
    auto restart=[&](bool,const config::GpuSettings& s){return s.nvvdd_ma && *s.nvvdd_ma>int(c.nv.max);};
    auto p=plan_profile(g,true,true,95,150,57,225,false,true,{},restart);check(p.settings.tgp==150 && !p.settings.nvvdd_ma && !p.restart,"automatic unlock-dependent profile clamps power without opening/restarting driver");
    g.tgp=120;g.bios_override_enabled=false;g.nvvdd_ma={};p=plan_profile(g,true,true,95,150,57,225,true,true,c,restart);check(p.settings.nvvdd_ma==120000 && p.settings.msvdd_ma==30000 && p.unlock,"legacy stock profile restores unspecified stock rails while retaining unlock");
    g.bios_override_enabled=true;g.nvvdd_ma=165000;g.board_input_ma=10000;p=plan_profile(g,true,true,95,150,57,225,true,true,c,restart);check(p.settings.nvvdd_ma==130000 && !p.settings.board_input_ma && !p.restart,"automatic active ceilings clamp currents and skip only unsupported board input");
}
int live(bool same_values) {
    Handle mutex(CreateMutexW(nullptr,TRUE,L"Global\\MechControl"));if(!mutex || GetLastError()==ERROR_ALREADY_EXISTS) throw std::runtime_error("A controller already owns Global\\MechControl");
    Api api;auto id=api.identity;std::cout<<"GPU "<<id.name<<" PCI "<<std::hex<<id.device<<'/'<<id.subsystem<<std::dec<<" BDF "<<id.domain<<':'<<id.bus<<':'<<id.slot<<'.'<<id.function<<" driver "<<id.driver<<" BIOS "<<id.bios<<'\n';
    Store store(id);std::cout<<"PnP awake "<<store.awake()<<" saved "<<Store::saved()<<'\n';auto b=Bios::parse(api.bios());check(b.device==id.device,"live BIOS matches exact NVAPI/NVML/PCI adapter");
    auto p=api.power();auto c=api.currents();std::cout<<"Power "<<p.min<<'/'<<p.def<<'/'<<p.max<<" enforced "<<p.enforced<<" requested "<<p.requested.value_or(0)<<" mW\nCurrents "<<c.nv.current<<'/'<<c.ms.current<<'/'<<(c.input?c.input->current:0)<<" mA\n";
    try {auto budget=api.budget();std::cout<<"Budget "<<budget.f7<<'/'<<budget.platform<<" mW\n";} catch(const std::exception& ex) {std::cout<<"Budget unsupported: "<<ex.what()<<'\n';}
    auto d=api.dlppc();std::cout<<"DLPPC "<<(d?d->active:-1)<<'\n';auto t=api.telemetry();std::cout<<"Measured "<<t.nv<<'/'<<t.ms<<" mA clock "<<t.clock_mhz.value_or(0)<<" MHz GPU power "<<t.gpu_mw.value_or(0)<<" mW\n";
    auto wrong=id;wrong.bus+=1;refuses([&]{Store foreign(wrong);},"live wrong PCI bus is refused, never first-adapter fallback");wrong=id;wrong.subsystem^=1;refuses([&]{Store foreign(wrong);},"live wrong PCI subsystem is refused");
    if(same_values) {auto after=api.set_currents(int(c.nv.current),int(c.ms.current),c.input?std::optional(int(c.input->current)):std::nullopt);check(after.nv==c.nv && after.ms==c.ms && after.input==c.input,"live same-value current SET/readback preserves all rails");if(d) {api.set_dlppc(d->active);check(api.dlppc()->active==d->active,"live same-value DLPPC SET/readback");}if(p.requested) {api.set_power(*p.requested);check(api.power().requested==p.requested,"live same-value F8/enforced readback");}}
    api.reinitialize();check(api.identity==id && !api.telemetry().ipc,"reinitialization reacquires exact adapter and invalidates IPC baseline");return 0;
}
}
int main(int argc,char** argv) {
    try {if(argc>1 && std::string(argv[1]).starts_with("--live")) return live(std::string(argv[1])=="--live-same-values");
        auto root=std::filesystem::path(L"C:/Temp/tandem/mechcontrol-native-phase5/check")/std::to_wstring(GetCurrentProcessId());for(int i=0;!std::filesystem::create_directories(root);++i) root=std::filesystem::path(L"C:/Temp/tandem/mechcontrol-native-phase5/check")/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(i));
        packets();bios_and_recovery(root);return 0;
    } catch(const std::exception& ex) {std::cerr<<"FAIL "<<ex.what()<<'\n';return 1;}
}
