#include "peripherals.h"
#include "ble.h"
#include "ble_wait.h"
#include "worker.h"
#include <winrt/Windows.Foundation.Collections.h>
#include <iostream>
#include <atomic>
#include <cmath>

using namespace mech;
using namespace mech::config;
using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Devices::Bluetooth;
namespace {
void check(bool ok,const char* name){if(!ok) throw std::runtime_error(name);std::cout<<"PASS "<<name<<std::endl;}
template<class F> bool until(F f,DWORD ms=3000){auto end=GetTickCount64()+ms;do{if(f())return true;Sleep(10);}while(GetTickCount64()<end);return false;}
struct FakeState {std::map<int,uint8_t> bytes;std::vector<std::pair<int,int>> writes;
    FakeState(){for(auto [a,v]:{std::pair{0x740,26},{0x741,1},{0x727,128},{0x7D2,1},{0x7AB,1},{0x7C5,0xC5}})bytes[a]=static_cast<uint8_t>(v);}
};
class Channel:public EcChannel {
    std::shared_ptr<FakeState> state;
public:
    explicit Channel(std::shared_ptr<FakeState> s):state(std::move(s)){}
    uint32_t ioctl(uint32_t code,std::span<const int32_t> w) override {
        if(code==ecSpecIoctl) {constexpr uint32_t spec[]{200,200,200,10,55,25,0,95,0,0xBB,0,0};return spec[uint32_t(w[0])>>8];}
        if(code==ecReadIoctl)return state->bytes[w[0]];
        if(code==ecWriteIoctl){state->bytes[w[0]]=uint8_t(w[1]);state->writes.emplace_back(w[0],w[1]);return 0;}
        throw std::runtime_error("Unknown IOCTL");
    }
    void reopen()override{}
};
void lighting(const std::filesystem::path& root) {
    auto state=std::make_shared<FakeState>();auto log=[](const std::string& s){std::cout<<s<<'\n';};
    EcSession session([&](const std::wstring&){return std::make_unique<Channel>(state);},log,false,[](unsigned){},profiles_directory(),Identity{"X6AR55xY","fixture","intel",uint8_t(26)});
    ProfileStore store(root,false,log);std::vector<std::pair<int,HidReport>> reports;
    Lighting owner(session,store,log,false,[&](int z,const HidReport& r){reports.emplace_back(z,r);});
    Zone green{true,4,255,61,41};owner.change(0,green);
    check(reports==std::vector<std::pair<int,HidReport>>{{0,{0,0x1A,0,1,4,0,0,0,1}},{0,{0,0x14,0,1,4,255,61,0,0}},{0,{0,8,2,1,1,41,8,0,0}}},"keyboard ON golden HID reports");
    reports.clear();state->writes.clear();owner.change(3,Zone{true,255,128,0,75});
    check(state->writes==std::vector<std::pair<int,int>>{{0x767,0},{0x7C5,0xE5},{0x769,60},{0x76A,30},{0x76B,0},{0x7AB,0x61},{0x78C,0x10}},"logo ordered guarded writes preserve shared fan-enable bits and scale RGB");
    auto logo=owner.logo();check(logo.enabled && logo.brightness==75 && logo.r==255 && logo.g==128,"logo source-compatible EC readback");
    reports.clear();state->writes.clear();owner.zone(3,Zone{});
    check(state->writes==std::vector<std::pair<int,int>>{{0x7AB,1},{0x78C,0x10}} && reports.size()==4,"logo OFF sequence");
    reports.clear();owner.zone(2,green,true);
    check(reports==std::vector<std::pair<int,HidReport>>{{2,{0,0x1A,0,1,4,0,0,0,1}},{2,{0,0x14,2,1,4,255,61,0,1}},{2,{0,8,0x22,1,1,41,1,0,1}}},"light-bar flash uses zone-zero power and save flags");
    reports.clear();owner.idle(10);owner.tick(100,10000);check(reports.size()==5 && reports.front().second==HidReport{0,9,2,0,0,0,0,0,0},"idle only turns off enabled keyboard, never logo");
    reports.clear();owner.tick(101,1000);check(reports.size()==3 && reports.front().first==0,"input wake restores keyboard intent");
    reports.clear();owner.resume(1000);owner.tick(8999,3000);check(reports.empty(),"resume respects eight-second HID settle");owner.tick(9000,3000);check(reports.size()==3,"resume restores enabled keyboard only");
    LightingState invalid;invalid.keyboard.reset();store.save_lighting(invalid);Lighting null_owner(session,store,log,false,[](int,const HidReport&){});
    bool refused=false;try{LightingSettings s;null_owner.capture(s);}catch(const std::exception&){refused=true;}check(refused,"explicit-null HID intent is not captured as all-off");
    state->bytes[0x402]=0x88;state->bytes[0x403]=0x13;state->bytes[0x404]=0x94;state->bytes[0x405]=0x11;state->bytes[0x4A2]=0xA5;state->bytes[0x4A3]=0x0B;
    auto battery=battery_telemetry(session.transport());check(battery["designCapacityMah"]==5000 && battery["fullChargeCapacityMah"]==4500 && battery["healthPercent"]==90 && std::abs(battery["temperatureCelsius"].get<double>()-24.95)<0.001,"battery LE mAh and tenth-Kelvin decoding");
}
struct Pending {
    Handle entered{CreateEventW(nullptr,TRUE,FALSE,nullptr)},release{CreateEventW(nullptr,TRUE,FALSE,nullptr)},returned{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    std::atomic<int> active=0,max_active=0;
};
IAsyncOperation<BluetoothLEDevice> delayed(std::shared_ptr<Pending> p) {
    co_await resume_background();int active=++p->active;p->max_active.store(std::max(p->max_active.load(),active));SetEvent(p->entered.get());
    // Deliberately ignores cancellation until the simulated driver returns.
    WaitForSingleObject(p->release.get(),INFINITE);--p->active;SetEvent(p->returned.get());co_return nullptr;
}
void pending(const std::filesystem::path& root) {
    auto log=[](const std::string& s){std::cout<<s<<std::endl;};ProfileStore store(root,false,log);
    for(int scenario=0;scenario<3;++scenario) {
        auto p=std::make_shared<Pending>();IAsyncOperation<BluetoothLEDevice> op{nullptr};
        auto owner=std::make_unique<Cooler>(store,log,[&](uint64_t){op=delayed(p);return op;});owner->request({CoolerAction::connect,0x123456789ABC});
        check(WaitForSingleObject(p->entered.get(),3000)==WAIT_OBJECT_0,"connection resolver genuinely pending");
        if(scenario==0) {
            check(until([&]{return owner->snapshot()["status"].get<std::string>().find("timed out")!=std::string::npos;},18000),"15-second connection timeout occurs while resolver remains outstanding");
        } else if(scenario==1) {
            owner->request({CoolerAction::disconnect});check(until([&]{return owner->snapshot()["status"]=="Disconnected";}),"manual disconnect supersedes pending connect without waiting for driver");
        }
        auto start=GetTickCount64();owner.reset();check(GetTickCount64()-start<1000,"shutdown releases BLE owner while connection completion is still pending");
        check(WaitForSingleObject(p->returned.get(),0)==WAIT_TIMEOUT,"owner destruction did not falsely claim driver cancellation completed");
        SetEvent(p->release.get());check(WaitForSingleObject(p->returned.get(),3000)==WAIT_OBJECT_0,"late driver returns after owner destruction");
        check(until([&]{return op.Status()!=AsyncStatus::Started;}),"late WinRT operation reaches actual terminal status");
        std::cout<<"Late WinRT result: "<<(op.Status()==AsyncStatus::Canceled?"Canceled":op.Status()==AsyncStatus::Completed?"Completed":"Error")<<std::endl;
        check(p->max_active==1,"single-attempt fixture owns one pending resolver");
    }
    {
        auto old=std::make_shared<Pending>(),current=std::make_shared<Pending>();std::atomic<int> calls=0;
        Cooler owner(store,log,[&](uint64_t){return delayed(++calls==1?old:current);});owner.request({CoolerAction::connect,1});
        check(WaitForSingleObject(old->entered.get(),3000)==WAIT_OBJECT_0,"first manual connection pending before replacement");
        owner.request({CoolerAction::connect,2});check(WaitForSingleObject(current->entered.get(),3000)==WAIT_OBJECT_0,"replacement logical connect starts after superseded attempt returns");
        SetEvent(old->release.get());check(WaitForSingleObject(old->returned.get(),3000)==WAIT_OBJECT_0,"old Windows completion can arrive during newer attempt");
        check(owner.snapshot()["connected"]==false && owner.snapshot()["status"]=="Connecting to cooler","late obsolete completion cannot publish or revive a connection");
        owner.request({CoolerAction::disconnect});check(until([&]{return owner.snapshot()["status"]=="Disconnected";}),"manual disconnect wins over replaced connection attempt");SetEvent(current->release.get());
        check(WaitForSingleObject(current->returned.get(),3000)==WAIT_OBJECT_0,"replaced attempt returns without resurrecting disconnected owner");
    }
    CoolerSettings s;s.pump_on=true;s.voltage=PumpVoltage::V8;s.pump_duty=63;s.rgb_on=true;s.r=1;s.g=2;s.b=3;s.rgb_mode=CoolerRgbMode::BreatheColor;
    auto packets=cooler_packets(s);check(packets[1]==std::vector<uint8_t>{0xFE,0x1C,1,63,3,0,0,0xEF} && packets[2]==std::vector<uint8_t>{0xFE,0x1E,1,1,2,3,3,0xEF} && packets.back()==std::vector<uint8_t>{0xFE,0x33,0,0,0,0,0,0xEF},"cooler wire enums and ordered commit bytes");
    for(auto section:{CoolerSection::fan,CoolerSection::pump,CoolerSection::rgb}) check(cooler_packets(s,section)==std::vector<std::vector<uint8_t>>{packets[size_t(section)-1],packets.back()},"manual section sends only its existing command and commit");
    {
        Cooler owner(store,log);owner.request({CoolerAction::apply,0,s,false,false});
        check(owner.intent().pump_duty==63 && !std::filesystem::exists(root/"liquid_cooler_state.json"),"slot cooler intent is available for reconnect without rewriting standalone state");
        bool refused=false;try {owner.request({CoolerAction::apply,0,s,false,true,CoolerSection::pump});}catch(const std::exception& e){refused=std::string(e.what()).find("not connected")!=std::string::npos;}
        check(refused && !std::filesystem::exists(root/"liquid_cooler_state.json"),"disconnected manual Apply truthfully refuses without writing standalone state");
    }
    {
        auto path=root/"invalid-saved";std::filesystem::create_directories(path);
        write_json(path/"liquid_cooler_state.json",Json{{"fanDuty",150},{"pumpDuty",-5},{"pumpVoltage",9},{"rgbMode",7}},false);
        ProfileStore saved(path,false,log);Cooler owner(saved,log);auto i=owner.intent();
        check(i.fan_duty==100 && i.pump_duty==0 && i.voltage==PumpVoltage::V8 && i.rgb_mode==CoolerRgbMode::BreatheColor,"saved cooler file gets request limits before auto-connect can send it");
    }
    {
        auto path=root/"auto";std::filesystem::create_directories(path);ProfileStore saved(path,false,log);CoolerState preference;preference.address=7;preference.auto_connect=true;saved.save_cooler(preference);
        auto p=std::make_shared<Pending>();SetEvent(p->release.get());std::atomic<int> calls=0;auto start=GetTickCount64();
        std::atomic<bool> completion=false;
        Cooler owner(saved,log,[&](uint64_t){++calls;return delayed(p);},[&](Json value){if(value["completed"]==true && value["ok"]==false) completion=true;});
        Sleep(100);check(calls==0,"constructor does not race selected-slot intent with auto-connect");
        owner.request({CoolerAction::apply,0,s,false,false});owner.start();
        check(until([&]{return calls>=1;}) && owner.intent().pump_duty==63,"explicit startup installs slot intent before saved auto-connect");
        owner.request({CoolerAction::auto_connect,0,{},false});
        check(until([&]{return calls>=2;},11000) && GetTickCount64()-start>=7900,"unreachable-cooler failure schedules event-driven eight-second reconnect backoff");
        check(completion,"real owner delivers failed completion values without UI polling");
        owner.request({CoolerAction::disconnect});check(until([&]{return owner.snapshot()["status"]=="Disconnected";}),"manual disconnect stops pending auto-reconnect");
    }
    {
        auto path=root/"manual-retry";std::filesystem::create_directories(path);ProfileStore saved(path,false,log);CoolerState preference;preference.address=7;preference.auto_connect=false;saved.save_cooler(preference);
        auto p=std::make_shared<Pending>();SetEvent(p->release.get());std::atomic<int> calls=0;
        Cooler owner(saved,log,[&](uint64_t){++calls;return delayed(p);});owner.request({CoolerAction::connect,7});
        check(until([&]{return calls>=2;},11000),"manual failed connect retries saved address even with auto-connect preference off");
        owner.request({CoolerAction::disconnect});
    }
}
void live(const std::filesystem::path& root) {
    Handle mutex(CreateMutexW(nullptr,TRUE,L"Global\\MechControl"));check(mutex && GetLastError()!=ERROR_ALREADY_EXISTS,"exclusive hardware-controller ownership");
    auto delivery=std::make_shared<Delivery>();Log log(root/"live.log");WorkerOptions options;options.state_directory=root;ControlWorker worker(delivery,log,options);
    auto read=worker.request(PeripheralRead{true}).get();check(read.code==0 && read.peripherals.has_value(),"real worker peripheral and battery read");auto before=*read.peripherals;write_json(root/"before.json",before,false);std::cout<<before.dump(2)<<std::endl;
    auto saved=read_json(root/"lighting_state.json").get<LightingState>();
    if(before.contains("lightingIntent")) {
        for(auto [id,z]:{std::pair{0,saved.keyboard},{2,saved.light_bar}}) if(z && before["zones"][id==0?0:1]==true) {
            check(worker.request(LightingApply{id,Zone{}}).get().code==0,"real HID zone OFF reports accepted");
            check(worker.request(LightingApply{id,*z}).get().code==0,"real HID saved-intent restore reports accepted");
        }
        auto logo=before["logo"].get<Zone>();check(worker.request(LightingApply{3,logo}).get().code==0,"real logo same-state HID plus guarded EC sequence");
    }
    if(before["dimmingAvailable"]==true) check(worker.request(DimmingApply{before["localDimming"].get<bool>()}).get().code==0,"real WMI same-state local dimming and EC verification");
    auto save=worker.request(SlotSave{1}).get();check(save.code==0 && save.saved,"actual controller slot capture integrates peripheral owners");
    auto after=worker.request(PeripheralRead{true}).get();write_json(root/"after.json",*after.peripherals,false);
    check((*after.peripherals)["logo"]==before["logo"],"logo readback retained after physical workflow");
    worker.request(Visibility{true}).get();worker.request(BatteryPage{true}).get();delivery->take();Sleep(3200);
    auto messages=delivery->take();check(std::any_of(messages.begin(),messages.end(),[](const auto& r){return r.kind==ResultKind::battery && r.control && r.control->peripherals->contains("battery");}),"actual visible-page battery subscription delivers physical telemetry values");
    worker.request(BatteryPage{false}).get();worker.request(Visibility{false}).get();
    if(before["coolerAvailable"]==true) {
        worker.request(CoolerRequest{CoolerAction::scan}).get();Sleep(6000);auto radio=worker.request(PeripheralRead{}).get();write_json(root/"ble-scan.json",*radio.peripherals,false);
        worker.request(CoolerRequest{CoolerAction::connect,0x123456789ABC}).get();Sleep(1000);worker.request(CoolerRequest{CoolerAction::disconnect}).get();
        check(until([&]{auto events=delivery->take();return std::any_of(events.begin(),events.end(),[](const auto& r){return r.kind==ResultKind::ble && r.control && (*r.control->peripherals)["cooler"]["completed"]==true;});}),"actual cooler owner completion reaches worker delivery without prototype discovery");
    }
    worker.stop();check(true,"real worker peripheral shutdown joined");
    // Exercise the actual HID owner with deterministic idle/resume time inputs,
    // not an asserted physical suspend or physical keyboard-input experiment.
    auto diagnostic=[&](const std::string& text){log.write(text);};
    EcSession session([](const std::wstring& path){return open_ec_channel(path,nullptr);},diagnostic,false,{},profiles_directory());
    ProfileStore store(root,true,diagnostic);Lighting lights(session,store,diagnostic,true);
    lights.idle(10);lights.tick(100,10000);lights.tick(101,1000);lights.resume(1000);lights.tick(9000,0);
    check(true,"real HID idle-off/wake and delayed reopen/restore calls accepted with injected time inputs");
}
void radio_check(const std::filesystem::path& root) {
    Handle mutex(CreateMutexW(nullptr,TRUE,L"Global\\MechControl"));check(mutex && GetLastError()!=ERROR_ALREADY_EXISTS,"exclusive cooler-check ownership");
    using namespace Windows::Devices::Radios;
    auto adapter=BluetoothAdapter::GetDefaultAsync().get();check(bool(adapter),"real Bluetooth adapter present");auto radio=adapter.GetRadioAsync().get();check(bool(radio),"real Bluetooth radio present");auto original=radio.State();
    struct Restore {Radio radio;RadioState state;~Restore(){try{auto result=radio.SetStateAsync(state).get();std::cout<<"Radio restoration: "<<(result==RadioAccessStatus::Allowed?"allowed":"denied")<<std::endl;}catch(...){std::cerr<<"Radio restoration failed\n";}}};
    {
        Restore restore{radio,original};check(Radio::RequestAccessAsync().get()==RadioAccessStatus::Allowed,"radio-control access allowed");
        check(radio.SetStateAsync(RadioState::On).get()==RadioAccessStatus::Allowed && until([&]{return radio.State()==RadioState::On;},5000),"Bluetooth temporarily enabled and verified On");
        auto log=[](const std::string& s){std::cout<<s<<std::endl;};ProfileStore store(root,false,log);Cooler owner(store,log);owner.request({CoolerAction::scan});
        check(until([&]{auto s=owner.snapshot()["status"].get<std::string>();return s=="Cooler scan complete" || s.starts_with("Cooler operation failed");},10000),"real active Bluetooth scan completes");
        auto scan=owner.snapshot();write_json(root/"radio-on-scan.json",scan,false);std::cout<<scan.dump(2)<<std::endl;
        auto address=scan["devices"].empty()?0x123456789ABCULL:scan["devices"][0]["address"].get<uint64_t>();owner.request({CoolerAction::connect,address});
        check(until([&]{auto s=owner.snapshot();return s["connected"]==true || s["status"].get<std::string>().starts_with("Cooler operation failed");},20000),"radio-on real connection attempt reaches a result");
        if(owner.snapshot()["connected"]==true) {
            Sleep(6000);write_json(root/"connected-firmware.json",owner.snapshot(),false);
            owner.request({CoolerAction::disconnect});check(until([&]{return owner.snapshot()["status"]=="Disconnected";}),"real cooler reset/disconnect completes");
            owner.request({CoolerAction::connect,address});check(until([&]{auto s=owner.snapshot();return s["connected"]==true || s["status"].get<std::string>().starts_with("Cooler operation failed");},20000),"real cooler reconnect reaches a result");
        }
        write_json(root/"radio-on-connect.json",owner.snapshot(),false);owner.request({CoolerAction::disconnect});check(until([&]{return owner.snapshot()["status"]=="Disconnected";}),"radio-on cooler owner disconnects cleanly");
    }
    check(until([&]{return radio.State()==original;},5000),"original Bluetooth radio state restored and verified");
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        init_apartment(apartment_type::multi_threaded);
        auto root=std::filesystem::path(L"C:/Temp/tandem/mechcontrol-native-phase6/check")/std::to_wstring(GetCurrentProcessId());
        for(int n=0;!std::filesystem::create_directories(root);++n) root=std::filesystem::path(L"C:/Temp/tandem/mechcontrol-native-phase6/check")/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(n));
        if(argc==3 && std::wstring_view(argv[1])==L"--live") {root=argv[2];std::filesystem::create_directories(root);live(root);}
        else if(argc==3 && std::wstring_view(argv[1])==L"--radio-check") {root=argv[2];std::filesystem::create_directories(root);radio_check(root);}
        else {lighting(root);pending(root);}
        uninit_apartment();return 0;
    }catch(const hresult_error& e){std::cerr<<to_string(e.message())<<'\n';return 1;}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
