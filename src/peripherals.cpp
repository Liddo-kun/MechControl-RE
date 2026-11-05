#include "peripherals.h"
#include "config_text.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <wbemidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <winrt/base.h>

namespace mech {
using namespace config;
namespace {
int rounded(double value) {int i=static_cast<int>(value);double f=value-i;return i+(f>0.5 || (f==0.5 && (i&1)));}
struct Com {HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);~Com(){if(SUCCEEDED(hr)) CoUninitialize();}};
struct Bstr {BSTR value;Bstr(const wchar_t* s):value(SysAllocString(s)){}~Bstr(){SysFreeString(value);}operator BSTR()const{return value;}};
using Microsoft::WRL::ComPtr;
ComPtr<IWbemServices> wmi() {
    ComPtr<IWbemLocator> locator;winrt::check_hresult(CoCreateInstance(CLSID_WbemLocator,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&locator)));
    ComPtr<IWbemServices> service;winrt::check_hresult(locator->ConnectServer(Bstr(L"ROOT\\WMI"),nullptr,nullptr,nullptr,0,nullptr,nullptr,&service));
    winrt::check_hresult(CoSetProxyBlanket(service.Get(),RPC_C_AUTHN_WINNT,RPC_C_AUTHZ_NONE,nullptr,RPC_C_AUTHN_LEVEL_CALL,RPC_C_IMP_LEVEL_IMPERSONATE,nullptr,EOAC_NONE));return service;
}
uint8_t dimming_call(IWbemServices* service,uint64_t data) {
    ComPtr<IWbemClassObject> cls,signature,input,output;
    winrt::check_hresult(service->GetObject(Bstr(L"AcpiTest_MULong"),0,nullptr,&cls,nullptr));
    winrt::check_hresult(cls->GetMethod(L"GetSetULong",0,&signature,nullptr));winrt::check_hresult(signature->SpawnInstance(0,&input));
    VARIANT v;VariantInit(&v);v.vt=VT_BSTR;v.bstrVal=SysAllocString(std::to_wstring(data).c_str());
    auto hr=input->Put(L"Data",0,&v,CIM_UINT64);VariantClear(&v);winrt::check_hresult(hr);
    winrt::check_hresult(service->ExecMethod(Bstr(L"AcpiTest_MULong.InstanceName='ACPI\\PNP0C14\\1_1'"),Bstr(L"GetSetULong"),0,nullptr,input.Get(),&output,nullptr));
    VariantInit(&v);winrt::check_hresult(output->Get(L"Return",0,&v,nullptr,nullptr));
    VARIANT n;VariantInit(&n);hr=VariantChangeType(&n,&v,0,VT_UI4);VariantClear(&v);winrt::check_hresult(hr);auto result=static_cast<uint8_t>(n.ulVal);VariantClear(&n);return result;
}
}
Lighting::Lighting(EcSession& s,ProfileStore& store,std::function<void(const std::string&)> log,bool hardware,Send send,
    std::function<void(std::function<void()>)> run_io,std::function<void(const std::vector<EcWrite>&)> batch):
    session_(s),store_(store),log_(std::move(log)),hardware_(hardware),output_(std::move(send)),run_io_(std::move(run_io)),batch_(std::move(batch)),state_(store.load_lighting()) {
    if(!batch_) batch_=[this](const auto& writes){session_.guard->apply(writes);}; // Standalone packet fixture/live probe only.
    if(hardware_) open();
    for(auto* zone:{&state_.logo,&state_.keyboard,&state_.light_bar}) if(!*zone) log_("Lighting intent contains an explicit-null zone; it is unavailable, not an all-off default");
}
bool Lighting::open() {
    bool found=false;io([&]{found=open_device();});return found;
}
void Lighting::io(const std::function<void()>& work) {
    io_busy_=true;
    try {if(run_io_) run_io_(work);else work();} catch(...) {io_busy_=false;throw;}
    io_busy_=false;
}
bool Lighting::open_device() {
    for(auto& h:handles_) h.reset();
    GUID guid;HidD_GetHidGuid(&guid);auto devices=SetupDiGetClassDevsW(&guid,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(devices==INVALID_HANDLE_VALUE) return false;
    struct List {HDEVINFO h;~List(){SetupDiDestroyDeviceInfoList(h);}} cleanup{devices};
    std::vector<std::pair<std::wstring,USHORT>> paths;std::string probes;int found=0;
    SP_DEVICE_INTERFACE_DATA iface{sizeof(iface)};
    for(DWORD i=0;SetupDiEnumDeviceInterfaces(devices,nullptr,&guid,i,&iface);++i) {
        DWORD bytes=0;SetupDiGetDeviceInterfaceDetailW(devices,&iface,nullptr,0,&bytes,nullptr);std::vector<uint8_t> storage(bytes);
        if(bytes<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        auto detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());detail->cbSize=sizeof(*detail);
        if(!SetupDiGetDeviceInterfaceDetailW(devices,&iface,detail,bytes,nullptr,nullptr)) continue;
        std::wstring path=detail->DevicePath;if(lower_invariant(path).find(L"vid_048d")==std::wstring::npos) continue;
        ++found;probes+="\n  "+winrt::to_string(path)+": ";
        Handle probe;
        for(DWORD access:{DWORD(0),DWORD(GENERIC_READ|GENERIC_WRITE),DWORD(GENERIC_WRITE)}) {probe=Handle(CreateFileW(path.c_str(),access,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr));if(probe) break;}
        PHIDP_PREPARSED_DATA data=nullptr;HIDP_CAPS caps{};
        if(!probe) probes+="cannot open (error "+std::to_string(GetLastError())+")";
        else if(!HidD_GetPreparsedData(probe.get(),&data)) probes+="GetPreparsedData failed";
        else {
            auto status=HidP_GetCaps(data,&caps);HidD_FreePreparsedData(data);
            probes+="caps status="+hex_text(unsigned(status),8)+" feature="+std::to_string(caps.FeatureReportByteLength)+" input="+std::to_string(caps.InputReportByteLength)
                +" output="+std::to_string(caps.OutputReportByteLength)+" usage="+hex_text(caps.Usage,4)+" usagePage="+hex_text(caps.UsagePage,4);
            if(status==HIDP_STATUS_SUCCESS && caps.FeatureReportByteLength==9) {paths.emplace_back(path,caps.Usage);probes+=" -> lighting controller";}
        }
    }
    log_("[HID] Found "+std::to_string(found)+" ITE (VID 048D) HID device paths"+probes);
    std::sort(paths.begin(),paths.end(),[](const auto& a,const auto& b){return CompareStringOrdinal(a.first.c_str(),-1,b.first.c_str(),-1,TRUE)==CSTR_LESS_THAN;});
    auto write=[&](int z,const std::wstring& path) {handles_[z]=Handle(CreateFileW(path.c_str(),GENERIC_WRITE,3,nullptr,OPEN_EXISTING,0,nullptr));if(!handles_[z]) handles_[z]=Handle(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,3,nullptr,OPEN_EXISTING,0,nullptr));};
    auto kb=std::find_if(paths.begin(),paths.end(),[](auto& p){return p.second==1;});auto bar=std::find_if(paths.begin(),paths.end(),[](auto& p){return p.second==2;});
    if(kb!=paths.end() || bar!=paths.end()) {if(kb!=paths.end()) write(0,kb->first);if(bar!=paths.end()) {write(2,bar->first);write(3,bar->first);}}
    else for(size_t i=0;i<std::min<size_t>(paths.size(),4);++i) write(static_cast<int>(i),paths[i].first);
    log_("HID lighting zones: keyboard="+std::to_string(available(0))+" bar="+std::to_string(available(2))+" logo="+std::to_string(available(3)));
    return std::any_of(handles_.begin(),handles_.end(),[](auto& h){return bool(h);});
}
bool Lighting::available(int z) const {return z>=0 && z<4 && (output_ || bool(handles_[z]));}
void Lighting::send(int z,const HidReport& report) {
    if(output_) {output_(z,report);return;}
    if(!available(z)) throw std::runtime_error("HID lighting zone unavailable");
    auto packet=report;if(HidD_SetFeature(handles_[z].get(),packet.data(),9)) return;
    auto error=GetLastError();
    if(error==6 || error==31 || error==55 || error==433 || error==1167 || error==1617) {
        open_device();if(available(z) && HidD_SetFeature(handles_[z].get(),packet.data(),9)) return;
    }
    throw std::system_error(GetLastError(),std::system_category(),"HidD_SetFeature");
}
void Lighting::zone(int z,const Zone& value,bool flash,bool color_only) {
    if(z!=0 && z!=2 && z!=3) throw std::invalid_argument("Unknown lighting zone");
    auto v=value;v.brightness=std::clamp(v.brightness,0,100);uint8_t zone=static_cast<uint8_t>(z),mask=z==0?2:z==2?0x22:0x23,speed=z==0?8:1,b=static_cast<uint8_t>(v.brightness);
    io([&] {if(flash) {
        if(z==3) throw std::invalid_argument("Logo persists through EC, not HID flash");
        if(z==0) send(z,{0,0x14,zone,9,v.r,v.g,v.b,0,0});
        send(z,{0,0x1A,static_cast<uint8_t>(z==2?0:z),1,4,0,0,0,1});
        send(z,{0,0x14,zone,1,v.r,v.g,v.b,0,1});send(z,{0,8,mask,1,1,b,speed,0,1});
        if(!v.enabled) send(z,{0,0x1A,zone,0,0,0,0,0,1});
    } else if(!v.enabled || !b) {
        if(z==0) send(z,{0,9,2,0,0,0,0,0,0});
        send(z,{0,0x12,0,3,0,0,0,0,0});send(z,{0,8,5,0,0,0,0,0,0});send(z,{0,8,1,zone,0,0,0,0,0});send(z,{0,0x1A,zone,0,0,0,0,0,1});
    } else {
        if(!color_only) send(z,{0,0x1A,zone,1,4,0,0,0,1});
        send(z,{0,0x14,zone,1,v.r,v.g,v.b,0,0});send(z,{0,8,mask,1,1,b,speed,0,0});
    }});
    if(z==3 && !flash) {
        auto& p=session_.profile;std::vector<EcWrite> writes;
        if(v.enabled && b) {
            if(!color_only) writes={{p.reg("logoTrigger"),0},{p.reg("fanCurveEnable"),0x20,0x20}};
            for(auto [name,c]:{std::pair{"logoR",v.r},{"logoG",v.g},{"logoB",v.b}}) writes.push_back({p.reg(name),static_cast<uint8_t>(rounded(c*60.0/255))});
            constexpr uint8_t levels[]{1,0x21,0x41,0x61,0x81};writes.push_back({p.reg("logoOnOff"),levels[(b-1)/25+1]});
        } else writes.push_back({p.reg("logoOnOff"),1});
        writes.push_back({p.reg("logoCommit"),0x10});batch_(writes);
    }
}
Zone Lighting::logo() {
    auto& p=session_.profile;auto& ec=session_.transport();auto on=ec.read(p.reg("logoOnOff"));Zone z;
    z.brightness=on&0x80?100:on&0x40?75:on&0x20?50:on!=1?25:0;z.enabled=z.brightness>0;
    z.r=static_cast<uint8_t>(std::clamp(rounded(ec.read(p.reg("logoR"))*255.0/60),0,255));
    z.g=static_cast<uint8_t>(std::clamp(rounded(ec.read(p.reg("logoG"))*255.0/60),0,255));
    z.b=static_cast<uint8_t>(std::clamp(rounded(ec.read(p.reg("logoB"))*255.0/60),0,255));return z;
}
void Lighting::change(int z,const Zone& v,bool flash,bool color_only) {
    zone(z,v,flash,color_only);
    if(z==0) state_.keyboard=v;else if(z==2) state_.light_bar=v;else state_.logo=v;
    store_.save_lighting(state_);
}
void Lighting::apply(const LightingSettings& s) {
    for(auto [z,v]:{std::pair{3,s.logo},{0,s.keyboard},{2,s.light_bar}}) if(available(z)) try {zone(z,v);} catch(const std::exception& e){log_(std::string("Lighting apply failed: ")+e.what());}
    state_.logo=s.logo;state_.keyboard=s.keyboard;state_.light_bar=s.light_bar;idle(s.idle_seconds);store_.save_lighting(state_);
}
void Lighting::capture(LightingSettings& s) {
    if(!hardware_ && !output_) return;
    try {s.logo=logo();} catch(const std::exception& e){log_(std::string("Logo capture failed; target intent retained: ")+e.what());}
    // Null intent is invalid, not an implicit off zone. Refuse capture rather than erase it.
    if(available(0)) {if(!state_.keyboard) throw std::runtime_error("Cannot capture explicit-null keyboard intent");s.keyboard=*state_.keyboard;}
    if(available(2)) {if(!state_.light_bar) throw std::runtime_error("Cannot capture explicit-null light-bar intent");s.light_bar=*state_.light_bar;}
    s.idle_seconds=state_.idle_seconds;
}
void Lighting::idle(int seconds) {state_.idle_seconds=seconds;if(seconds<=0 && sleeping_) {sleeping_=false;restore(false);}}
void Lighting::resume(ULONGLONG now) {resume_at_=now+8000;}
void Lighting::restore(bool reopen) {
    if(reopen && hardware_) open();
    auto saved=reopen?store_.load_lighting():state_;
    for(auto [z,v]:{std::pair{0,saved.keyboard},{2,saved.light_bar}}) if(v && v->enabled && available(z)) try {zone(z,*v);} catch(const std::exception& e){log_(std::string("Lighting restore failed: ")+e.what());}
}
void Lighting::tick(ULONGLONG now,std::optional<DWORD> idle_ms) {
    if(io_busy_) return; // The shared timer can sample safety while a HID operation is joined.
    if(resume_at_ && now>=resume_at_) {resume_at_=0;restore(true);}
    if(state_.idle_seconds<=0) return;
    if(!idle_ms) {LASTINPUTINFO input{sizeof(input)};idle_ms=GetLastInputInfo(&input)?GetTickCount()-input.dwTime:0;}
    if(!sleeping_ && *idle_ms>=static_cast<uint32_t>(state_.idle_seconds)*1000u) {
        sleeping_=true;
        for(auto [z,v]:{std::pair{0,state_.keyboard},{2,state_.light_bar}}) if(v && v->enabled && available(z)) try {zone(z,Zone{});} catch(const std::exception& e){log_(e.what());}
    } else if(sleeping_ && *idle_ms<2000) {sleeping_=false;restore(false);}
}
bool local_dimming_available() {
    try {Com com;auto service=wmi();ComPtr<IWbemClassObject> object;return SUCCEEDED(service->GetObject(Bstr(L"AcpiTest_MULong.InstanceName='ACPI\\PNP0C14\\1_1'"),0,nullptr,&object,nullptr));} catch(...) {return false;}
}
void set_local_dimming(bool enabled) {
    try {
    Com com;auto service=wmi();
    if(dimming_call(service.Get(),0x0000050000000000ULL+(enabled?2:1))!=(enabled?0xBB:0xAA)) throw std::runtime_error("Local dimming EC WMI result mismatch");
    if(dimming_call(service.Get(),0x0000050000000000ULL+(enabled?0xE:0xD))!=(enabled?0xE:0xD)) throw std::runtime_error("Local dimming GPIO WMI result mismatch");
    } catch(const winrt::hresult_error& e) {throw std::runtime_error(winrt::to_string(e.message()));}
}
void ensure_uefi_flags(const std::function<void(const std::string&)>& log) {
    Handle token;HANDLE raw=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&raw)) {log("UEFI privilege unavailable");return;}token=Handle(raw);
    TOKEN_PRIVILEGES privilege{};privilege.PrivilegeCount=1;privilege.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;
    if(!LookupPrivilegeValueW(nullptr,SE_SYSTEM_ENVIRONMENT_NAME,&privilege.Privileges[0].Luid)) return;
    SetLastError(0);AdjustTokenPrivileges(token.get(),FALSE,&privilege,0,nullptr,nullptr);if(GetLastError()!=0) {log("UEFI privilege unavailable");return;}
    constexpr auto name=L"OemMagicVariable",guid=L"{9f33f85c-13ca-4fd1-9c4a-96217722c593}";std::array<uint8_t,4096> data{};
    auto size=GetFirmwareEnvironmentVariableW(name,guid,data.data(),static_cast<DWORD>(data.size()));
    if(!size) {log("UEFI OemMagicVariable unavailable; skipping");return;}
    if(size!=180 || data[0x2F]>3 || data[0x33]>1 || data[0x5D]>1 || data[0x61]>1) {log("UEFI layout unrecognized; skipping");return;}
    if(data[0x2F]==3 && data[0x33]==1 && data[0x5D]==1 && data[0x61]==1) {log("UEFI flags already set");return;}
    data[0x2F]=3;data[0x33]=data[0x5D]=data[0x61]=1;
    if(!SetFirmwareEnvironmentVariableW(name,guid,data.data(),size)) {log("UEFI flag write failed: "+std::to_string(GetLastError()));return;}
    auto count=GetFirmwareEnvironmentVariableW(name,guid,data.data(),static_cast<DWORD>(data.size()));
    log(count==180 && data[0x2F]==3 && data[0x33]==1 && data[0x5D]==1 && data[0x61]==1?"UEFI flags updated and verified":"UEFI flags written; readback not verified");
}
Json battery_telemetry(EcTransport& ec) {
    auto word=[&](int a){return ec.read(a)|(ec.read(a+1)<<8);};int design=word(0x402),full=word(0x404);
    return {{"onAC",(ec.read(0x490)&1)!=0},{"chargePercent",ec.read(0x4AB)},{"temperatureCelsius",word(0x4A2)/10.0-273.15},
        {"cycleCount",word(0x4A6)},{"designCapacityMah",design},{"fullChargeCapacityMah",full},{"remainingCapacityMah",word(0x436)},
        {"designVoltageMv",word(0x408)},{"presentVoltageMv",word(0x438)},{"chargeRateMa",word(0x434)},{"healthPercent",design>0?rounded(100.0*full/design):0}};
}
}
