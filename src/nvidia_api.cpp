#include "nvidia.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <windows.h>

namespace mech::nv {
struct Unsupported : std::runtime_error {using std::runtime_error::runtime_error;};
static void require(bool b,const char* name) {if(!b) throw Unsupported(std::string("Unsupported NVIDIA ")+name);}
static void check(int status,const char* name) {if(status) throw std::runtime_error(std::string("NVIDIA ")+name+" failed ("+std::to_string(status)+")");}
static void version(std::span<const uint8_t> b,uint32_t v) {require(u32(b,0)==v,"structure version");}
static bool range(uint32_t min,uint32_t def,uint32_t max) {return min<=def && def<=max && max && max<=INT_MAX;}
template<class F> static std::optional<int> unique(uint32_t mask,F match) {
    std::optional<int> found;for(int i=0;i<32;++i) if((mask&(1u<<i)) && match(i)) {if(found) return {};found=i;}return found;
}
struct Api::Native {
    using Query=void*(__cdecl*)(uint32_t);
    HMODULE nvapi=nullptr,nvml=nullptr;Query query=nullptr;void* gpu=nullptr;void* device=nullptr;
    bool initialized=false,nvml_initialized=false,platform_initialized=false;
    using Dispatch=int(__cdecl*)(uint32_t,void*,uint32_t,uint64_t,uint8_t,uint8_t,uint32_t);
    Dispatch dispatch=nullptr;uint32_t native_size=0;
    Bytes guarded;
    template<class T> T resolve(uint32_t id) {auto p=query(id);require(p!=nullptr,"interface unavailable");return reinterpret_cast<T>(p);}
    template<class T> T symbol(const char* name) {auto p=GetProcAddress(nvml,name);require(p!=nullptr,"NVML symbol");return reinterpret_cast<T>(p);}
    static HMODULE load(const wchar_t* name) {auto m=LoadLibraryExW(name,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!m) throw std::runtime_error("NVIDIA system library unavailable");return m;}
    void call_guarded(uint32_t id,Bytes& guarded,bool platform=false) {
        int s=platform?resolve<int(__cdecl*)(void*)>(id)(guarded.data()+4096):resolve<int(__cdecl*)(void*,void*)>(id)(gpu,guarded.data()+4096);
        require(std::all_of(guarded.begin(),guarded.begin()+4096,[](auto v){return v==0xcd;}) && std::all_of(guarded.end()-4096,guarded.end(),[](auto v){return v==0xcd;}),"buffer overrun");
        check(s,"packet call");
    }
    void call(uint32_t id,Bytes& b,bool platform=false) {
        guarded.assign(b.size()+8192,0xcd);std::copy(b.begin(),b.end(),guarded.begin()+4096);
        call_guarded(id,guarded,platform);std::copy(guarded.begin()+4096,guarded.end()-4096,b.begin());
    }
    uint32_t enforced() {uint32_t value=0;check(symbol<int(__cdecl*)(void*,uint32_t*)>("nvmlDeviceGetEnforcedPowerLimit")(device,&value),"NVML enforced power");return value;}
    uint32_t clock() {uint32_t v=0;check(symbol<int(__cdecl*)(void*,uint32_t,uint32_t*)>("nvmlDeviceGetClockInfo")(device,0,&v),"NVML graphics clock");return v;}
    ~Native() {
        if(platform_initialized) {auto p=query(0xd7c61344);if(p) reinterpret_cast<int(__cdecl*)(uint32_t)>(p)(1);}
        if(nvml_initialized) {auto p=GetProcAddress(nvml,"nvmlShutdown");if(p) reinterpret_cast<int(__cdecl*)()>(p)();}
        if(initialized) {auto p=query(0xd22bdd7e);if(p) reinterpret_cast<int(__cdecl*)()>(p)();}
        if(nvml) FreeLibrary(nvml);if(nvapi) FreeLibrary(nvapi);
    }
    struct Body {std::span<const uint8_t> code;DWORD64 image;};
    static Body body(void* p) {
        require(p!=nullptr,"native F7 transport");DWORD64 image=0;auto f=RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(p),&image,nullptr);
        require(f && image+f->BeginAddress==reinterpret_cast<DWORD64>(p) && f->EndAddress>f->BeginAddress && f->EndAddress-f->BeginAddress<=0x4000,"native F7 function bounds");
        return {{static_cast<const uint8_t*>(p),f->EndAddress-f->BeginAddress},image};
    }
    static size_t find(std::span<const uint8_t> code,std::initializer_list<uint8_t> pattern) {
        std::optional<size_t> found;for(size_t i=0;i+pattern.size()<=code.size();++i) if(std::equal(pattern.begin(),pattern.end(),code.begin()+i)) {require(!found,"ambiguous native F7 signature");found=i;}
        require(found.has_value(),"native F7 signature");return *found;
    }
    void init_budget() {
        if(!dispatch) {
            auto get=query(0x33ab0353),set=query(0x17695269);auto g=body(get),s=body(set);
            size_t p=find(g.code,{0xc7,0x44,0x24,0x30,0,0,0,0,0xc6,0x44,0x24,0x28,0,0xc6,0x44,0x24,0x20,1,0x45,0x33,0xc9,0x41,0xb8})+23;
            native_size=u32(g.code,p);require(native_size==0x122d70 || native_size==0x60b30,"native F7 ABI size");
            const std::initializer_list<uint8_t> tail={0x49,0x8b,0xd6,0xb9,0x46,0,0,7,0xe8};size_t q=p+4;
            require(q+tail.size()<=g.code.size() && std::equal(tail.begin(),tail.end(),g.code.begin()+q),"native F7 GET dispatch");
            find(g.code,{0x45,0x89,0x7e,0x30,0x41,0xc7,0x46,0x34,0x32,0xa6,0x80,0x20});find(s.code,{0x33,0xe6,0x80,0x20});
            auto t=find(s.code,tail);
            auto target=static_cast<uint8_t*>(get)+q+tail.size()+4+static_cast<int32_t>(u32(g.code,q+tail.size()));
            auto other=static_cast<uint8_t*>(set)+t+tail.size()+4+static_cast<int32_t>(u32(s.code,t+tail.size()));
            require(target==other && g.image==s.image && body(target).image==g.image,"paired native F7 dispatch");dispatch=reinterpret_cast<Dispatch>(target);
        }
        if(!platform_initialized) {resolve<int(__cdecl*)(uint32_t)>(0xd7c61344);check(resolve<int(__cdecl*)(uint32_t)>(0xad298d3f)(1),"InitializeEx(platform)");platform_initialized=true;}
    }
    uint32_t f7(int index,std::optional<uint32_t> value={}) {
        init_budget();auto handle=reinterpret_cast<uintptr_t>(gpu);require(index>=0 && index<32 && handle && handle<=UINT32_MAX,"F7 GPU/index");
        uint32_t method=value?0x2080e633:0x2080a632;size_t offset=0x64+index*12;Bytes guarded(native_size+8192,0xcd);auto b=std::span(guarded).subspan(4096,native_size);std::fill(b.begin(),b.end(),uint8_t(0));
        put(b,0x30,static_cast<uint32_t>(handle));put(b,0x34,method);put(b,0x3c,1u<<index);put(b,offset,value.value_or(0));put(b,offset+4,0xf7);
        int status=dispatch(0x7000046,b.data(),native_size,0,1,0,0);
        require(std::all_of(guarded.begin(),guarded.begin()+4096,[](auto v){return v==0xcd;}) && std::all_of(guarded.end()-4096,guarded.end(),[](auto v){return v==0xcd;}),"F7 buffer overrun");
        require(u32(b,0x30)==handle && u32(b,0x34)==method && u32(b,0x3c)==(1u<<index) && u32(b,offset+4)==0xf7,"F7 response identity");
        for(size_t i=0x38;i<b.size();++i) if(i!=0x38 && i!=offset-4 && i!=offset-3 && !(i>=0x3c && i<0x40) && !(i>=offset && i<offset+8)) require(b[i]==0,"F7 response bounds");
        check(status,"F7 dispatch");require(u32(b,0x2c)==1,"F7 completion");auto result=u32(b,offset);require(result && result!=UINT32_MAX,"F7 value");return result;
    }
    Bytes platform() {
        init_budget();Bytes b(0xc88);put(b,0,1);put(b,4,0x10c88);call(0x93456591,b,true);
        require(u32(b,0)==1 && u32(b,4)==0x10c88 && b[8]==1 && u32(b,0x2c) && u32(b,0x2c)!=UINT32_MAX,"PCF QBOOST controller/version/target");return b;
    }
    void platform(uint32_t target) {auto b=platform();put(b,0x2c,target);call(0xb78734ab,b,true);require(u32(platform(),0x2c)==target,"platform budget readback");}
};
Api::Api(Call call):call_(std::move(call)) {}
Api::Api() {
    native_=std::make_unique<Native>();auto& n=*native_;n.nvapi=Native::load(L"nvapi64.dll");
    n.query=reinterpret_cast<Native::Query>(GetProcAddress(n.nvapi,"nvapi_QueryInterface"));require(n.query!=nullptr,"QueryInterface");
    check(n.resolve<int(__cdecl*)()>(0x0150e828)(),"Initialize");n.initialized=true;
    std::array<void*,64> handles{};uint32_t count=0;check(n.resolve<int(__cdecl*)(void**,uint32_t*)>(0xe5ac921f)(handles.data(),&count),"EnumPhysicalGPUs");
    require(count==1,"adapter count: EC target is ambiguous unless exactly one NVIDIA GPU is present");n.gpu=handles[0];
    uint32_t pci=0,subsystem=0,revision=0,ext=0,bus=0,driver=0;char branch[64]{};
    check(n.resolve<int(__cdecl*)(void*,uint32_t*,uint32_t*,uint32_t*,uint32_t*)>(0x2ddfb66e)(n.gpu,&pci,&subsystem,&revision,&ext),"PCIIdentifiers");
    check(n.resolve<int(__cdecl*)(void*,uint32_t*)>(0x1be0b8e5)(n.gpu,&bus),"BusId");check(n.resolve<int(__cdecl*)(uint32_t*,char*)>(0x2926aaad)(&driver,branch),"DriverVersion");
    n.nvml=Native::load(L"nvml.dll");check(n.symbol<int(__cdecl*)()>("nvmlInit_v2")(),"NVML Init");n.nvml_initialized=true;
    check(n.symbol<int(__cdecl*)(uint32_t*)>("nvmlDeviceGetCount_v2")(&count),"NVML Count");
    for(uint32_t i=0;i<count;++i) {
        void* candidate=nullptr;check(n.symbol<int(__cdecl*)(uint32_t,void**)>("nvmlDeviceGetHandleByIndex_v2")(i,&candidate),"NVML Handle");Bytes data(68);
        check(n.symbol<int(__cdecl*)(void*,void*)>("nvmlDeviceGetPciInfo_v3")(candidate,data.data()),"NVML PCI info");
        if(u32(data,20)!=bus || u32(data,28)!=pci || u32(data,32)!=subsystem) continue;
        require(!n.device,"ambiguous NVAPI/NVML PCI identity");std::string bdf(reinterpret_cast<char*>(data.data()+36),strnlen(reinterpret_cast<char*>(data.data()+36),32));auto dot=bdf.rfind('.');
        require(dot!=std::string::npos && dot+2==bdf.size() && bdf.back()>='0' && bdf.back()<='7' && u32(data,24)<=31,"PCI function identity");
        n.device=candidate;identity.domain=u32(data,16);identity.slot=u32(data,24);identity.function=bdf.back()-'0';
    }
    require(n.device!=nullptr,"NVML adapter does not match NVAPI PCI identity");
    call_=[this](uint32_t id,Bytes& b){native_->call(id,b);};
    auto str=[&](uint32_t id){Bytes b(64);call_(id,b);return std::string(reinterpret_cast<char*>(b.data()),strnlen(reinterpret_cast<char*>(b.data()),b.size()));};
    identity.device=static_cast<uint16_t>(pci>>16);identity.subsystem=subsystem;identity.bus=bus;identity.bios=str(0xa561fd7d);identity.name=str(0xceee8e9f);
    char ver[32];sprintf_s(ver,"%u.%02u",driver/100,driver%100);identity.driver=ver;
}
Api::~Api()=default;
void Api::reinitialize() {
    auto before=identity;native_.reset();call_={};inline_mask_=inline_khz_=0;inline_unsupported_=false;inline_counts_={};inline_time_=0;
    Api replacement;auto& after=replacement.identity;
    require(before.device==after.device && before.subsystem==after.subsystem && before.domain==after.domain && before.bus==after.bus && before.slot==after.slot && before.function==after.function,"adapter identity changed after restart");
    native_=std::move(replacement.native_);identity=after;call_=[this](uint32_t id,Bytes& b){native_->call(id,b);};
}
Bytes Api::bios() {Bytes image(1024000);auto info=packet(16,0x20010);put(info,4,static_cast<uint32_t>(image.size()));auto pointer=reinterpret_cast<uint64_t>(image.data());memcpy(info.data()+8,&pointer,8);call_(0xfc13ee11,info);auto size=u32(info,4);require(size>=512 && size<=image.size(),"VBIOS image length");image.resize(size);return image;}
Api::Board Api::board() {
    auto b=packet(0x728,0x10728);call_(0x54924bf5,b);version(b,0x10728);std::optional<Board> found;
    for(int i=0;i<32;++i) {size_t p=0x28+i*0x38;if(!(u32(b,4)&(1u<<i)) || u32(b,p)>1) continue;require(u32(b,p+4)==0 && !found,"board-power units/identity");found=Board{i,u32(b,p+8),u32(b,p+12),u32(b,p+16)};require(range(found->min,found->def,found->max),"board-power range");}
    require(found.has_value(),"missing board-power policy");return *found;
}
Bytes Api::power_control(int i) {auto b=packet(0x528,0x10528);put(b,4,1u<<i);put(b,0x2c+i*0x28,0xf8);return b;}
PowerLimits Api::power() {
    auto p=board();auto c=power_control(p.index);call_(0x33ab0353,c);version(c,0x10528);require(u32(c,4)==(1u<<p.index) && u32(c,0x2c+p.index*0x28)==0xf8,"F8 selection readback");
    auto v=u32(c,0x28+p.index*0x28);return {p.min,p.def,p.max,native_?native_->enforced():0,v && v!=UINT32_MAX?std::optional(v):std::nullopt};
}
void Api::set_power(uint32_t mw) {auto p=board();require(mw>=p.min && mw<=p.max,"power request range");auto c=power_control(p.index);put(c,0x28+p.index*0x28,mw);call_(0x17695269,c);
    for(int i=0;;++i) {auto r=power();if(r.requested==mw && r.enforced<=mw) return;if(i==15) throw std::runtime_error("NVIDIA F8/enforced readback failed");Sleep(100);}}
Api::Rails Api::rails() {
    auto b=packet(0xa70,0x40a70);call_(0x67f31384,b);version(b,0x40a70);auto m=packet(0xca8,0x30ca8);call_(0xc12eb19e,m);version(m,0x30ca8);
    std::optional<Policy> nv,ms,input;int inputs=0;auto mask=u32(b,4),channels=u32(m,16);
    for(int i=0;i<32;++i) {
        if(!(mask&(1u<<i))) continue;
        size_t p=0x3c+i*0x58;auto type=u32(b,p),channel=u32(b,p+4);
        if((type!=19 && type!=4) || u32(b,p+8)!=1 || channel>=32 || !(channels&(1u<<channel))) continue;
        size_t q=0x28+channel*0x3c;Policy rail{i,type,u32(b,p+12),u32(b,p+16),u32(b,p+20)};
        if(type==4) {if(u32(m,q+12)!=8 || u32(m,q+16)==UINT32_MAX) continue;input=rail;++inputs;continue;}
        if(u32(m,q+12)!=6) continue;auto id=u32(m,q+16)&255;if(id!=1 && id!=16) continue;require(range(rail.min,rail.def,rail.max),"current range");
        if(id==1) {require(!nv,"ambiguous NVVDD policies");nv=rail;}else {require(!ms,"ambiguous MSVDD policies");ms=rail;}
    }
    require(nv && ms,"NVVDD/MSVDD sensor policies");if(inputs>1 || (input && !range(input->min,input->def,input->max))) input={};
    return {*nv,*ms,input,(1u<<nv->index)|(1u<<ms->index)|(input?1u<<input->index:0),std::move(b)};
}
Bytes Api::control(uint32_t mask) {auto b=packet(0xa4c,0x10a4c);put(b,4,mask);call_(0x8b3e7343,b);version(b,0x10a4c);require(u32(b,4)==mask,"current selection mask");return b;}
Rail Api::rail(const Policy& r,const Bytes& b) {size_t p=0x1c+r.index*0x28;auto v=u32(b,p+4);require(u32(b,p)==r.type && v>=r.min && v<=r.max,"current-policy identity/range");return {r.min,r.def,r.max,v,r.max};}
Currents Api::currents() {auto r=rails();auto b=control(r.mask);return {rail(r.nv,b),rail(r.ms,b),r.input?std::optional(rail(*r.input,b)):std::nullopt};}
Currents Api::set_currents(std::optional<int> nv,std::optional<int> ms,std::optional<int> input) {
    auto r=rails();auto b=control(r.mask);Currents before{rail(r.nv,b),rail(r.ms,b),r.input?std::optional(rail(*r.input,b)):std::nullopt};uint32_t selected=0;
    auto change=[&](std::optional<Policy> p,std::optional<int> value) {if(!value) return;require(p.has_value(),"board input policy");require(*value>=0 && uint32_t(*value)>=p->min && uint32_t(*value)<=p->max,"current request range");put(b,0x20+p->index*0x28,*value);selected|=1u<<p->index;};
    change(r.nv,nv);change(r.ms,ms);change(r.input,input);if(!selected) return currents();put(b,4,selected);call_(0xaffc2279,b);auto result=currents();
    require((!nv || result.nv.current==uint32_t(*nv)) && (!ms || result.ms.current==uint32_t(*ms)) && (!input || (result.input && result.input->current==uint32_t(*input))),"current readback");
    require((nv || result.nv==before.nv) && (ms || result.ms==before.ms) && (input || result.input==before.input),"unselected current changed");return result;
}
Budget Api::budget() {require(native_!=nullptr,"native budget transport unavailable");return {native_->f7(board().index),u32(native_->platform(),0x2c)};}
void Api::set_budget(Budget target) {
    auto current=budget();auto b=board();require(target.f7>=b.min && target.f7<=b.max && target.platform && target.platform!=UINT32_MAX,"budget target range");
    if(target.platform>current.platform) native_->platform(target.platform);
    if(target.f7!=current.f7) {native_->f7(b.index,target.f7);require(native_->f7(b.index)==target.f7,"F7 exact readback");}
    if(target.platform<current.platform) native_->platform(target.platform);
}
std::optional<std::pair<int,uint32_t>> Api::find_dlppc() {
    auto b=packet(0x164c,0x1164c);call_(0x1cac3865,b);version(b,0x1164c);int p=b[24]|(b[25]<<8);
    if(p>=32 || !(u32(b,4)&(1u<<p))) return {};size_t e=76+p*176;require(u32(b,e+8)==7 && u32(b,e)==0,"PERF_CF policy identity");return std::pair(p,u32(b,e+48));
}
Bytes Api::policy_control(int p) {auto b=packet(0x1548,0x11548);put(b,4,1u<<p);call_(0x69de2c54,b);version(b,0x11548);require(u32(b,4)==(1u<<p) && u32(b,72+p*168)==0,"PERF_CF policy control");return b;}
std::optional<Dlppc> Api::dlppc() {auto p=find_dlppc();if(!p) return {};return Dlppc{policy_control(p->first)[72+p->first*168+4]!=0,p->first,p->second};}
void Api::set_dlppc(bool active) {auto p=find_dlppc();require(p.has_value(),"DLPPC policy");auto b=policy_control(p->first);b[72+p->first*168+4]=active?1:0;call_(0x4d1c7d6e,b);require((policy_control(p->first)[72+p->first*168+4]!=0)==active,"DLPPC readback");}
std::optional<double> Api::inline_activity() {
    if(inline_unsupported_) return {};
    try {
        if(!inline_mask_) {auto b=packet(0x94c,0x1094c);call_(0xb2c9d666,b);version(b,0x1094c);uint32_t all=u32(b,4),ipc=0;
            for(int m=0;m<32;++m) if((all&(1u<<m)) && u32(b,76+m*72)==5) ipc|=1u<<m;
            if(!ipc || !u32(b,8)) {inline_unsupported_=true;return {};}inline_mask_=all;inline_khz_=u32(b,8);}
        auto b=packet(0xa48,0x10a48);put(b,4,inline_mask_);call_(0x4b4bd039,b);version(b,0x10a48);
        LARGE_INTEGER now{},freq{};QueryPerformanceCounter(&now);QueryPerformanceFrequency(&freq);double seconds=double(now.QuadPart-inline_time_)/freq.QuadPart;
        bool valid=inline_time_ && seconds>0 && seconds<=5;uint64_t busiest=0;
        for(int m=0;m<32;++m) {if(!(inline_mask_&(1u<<m)) || u32(b,72+m*80)!=5) continue;uint64_t count;memcpy(&count,b.data()+80+m*80,8);
            if(count<inline_counts_[m]) valid=false;else busiest=std::max(busiest,count-inline_counts_[m]);inline_counts_[m]=count;}
        inline_time_=now.QuadPart;return valid?std::optional(busiest/(seconds*inline_khz_*1000)):std::nullopt;
    } catch(const Unsupported&) {inline_unsupported_=true;return {};} catch(const std::exception&) {return {};}
}
Telemetry Api::telemetry() {
    auto r=rails();auto mask=u32(r.info,4);constexpr size_t size=0xb00b8;
    status_.resize(size+(native_?8192:0));auto b=std::span(status_).subspan(native_?4096:0,size);std::fill(b.begin(),b.end(),uint8_t(0));put(b,0,size);put(b,4,mask);
    if(native_) {std::fill_n(status_.begin(),4096,uint8_t(0xcd));std::fill_n(status_.end()-4096,4096,uint8_t(0xcd));native_->call_guarded(0x31b7a4cd,status_);}
    else {call_(0x31b7a4cd,status_);b=std::span(status_);}
    version(b,0xb00b8);
    auto entry=[](int i){return size_t(0x11b8+i*0x3768);};auto at=[&](int i,int o){return u32(b,entry(i)+0x968+o);};
    auto measured=[&](Policy p){require(u32(b,entry(p.index))==p.type,"current status identity");return u32(b,entry(p.index)+8);};
    Telemetry t;t.nv=measured(r.nv);t.ms=measured(r.ms);if(r.input && u32(b,entry(r.input->index))==r.input->type) t.input=u32(b,entry(r.input->index)+8);
    auto controller=[&](int i) {return u32(b,entry(i))==0x15 && at(i,4) && at(i,4)<=at(i,0x24) && uint64_t(at(i,0x60))==uint64_t(at(i,0x134))+at(i,0xb84) && uint64_t(at(i,0x15d0))==uint64_t(at(i,0x1690))+at(i,0x1e70);};
    auto current=unique(mask,[&](int i){return controller(i) && at(i,0x134)==t.nv && at(i,0xb84)==t.ms;});
    auto board_power=unique(mask,[&](int i){auto p=0x3c+i*0x58;return u32(r.info,p)==0 && u32(r.info,p+8)==0 && u32(r.info,p+20)!=0 && u32(b,entry(i))==0;});
    std::optional<int> power;
    if(board_power) power=unique(mask,[&](int i){if(!controller(i)) return false;auto a=entry(i)+0xe0;int count=b[a+1];if(count>8) return false;for(int k=0;k<count;++k) if(b[a+0x48+k*8]==*board_power) return true;return false;});
    auto value=[&](std::optional<int> i,int o)->std::optional<uint32_t>{return i?std::optional(at(*i,o)):std::nullopt;};
    auto limit=[&](Policy p)->std::optional<uint32_t> {auto v=u32(b,entry(p.index)+4);return u32(b,entry(p.index))==p.type && v>=p.min && v<=p.max?std::optional(v):std::nullopt;};
    t.nv_prediction=value(current,0x1690);t.ms_prediction=value(current,0x1e70);t.current_khz=value(current,4);t.power_khz=value(power,4);t.ceiling_khz=value(current,0x24);if(!t.ceiling_khz) t.ceiling_khz=value(power,0x24);
    t.nv_limit=limit(r.nv);t.ms_limit=limit(r.ms);if(r.input) t.input_limit=limit(*r.input);if(board_power && u32(b,entry(*board_power)+8)) t.gpu_mw=u32(b,entry(*board_power)+8);t.core_mw=value(power,0x60);
    if(native_) {try {t.clock_mhz=native_->clock();} catch(...) {}try {t.enforced_mw=native_->enforced();} catch(...) {}}
    t.ipc=inline_activity();return t;
}
}
