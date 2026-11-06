#include "controller.h"
#include "worker.h"
#ifdef MECHCONTROL_WITH_CLI
#include "cli_server.h"
#endif
#include <iostream>
#include <fstream>
#include <atomic>
#include <psapi.h>

using namespace mech;
using namespace mech::config;
namespace {
void check(bool ok,const char* text) {if(!ok) throw std::runtime_error(text);std::cout<<"PASS "<<text<<'\n';}
struct State {
    std::mutex mutex;
    std::map<int,uint8_t> bytes;
    std::vector<std::pair<int,int>> writes;
    std::map<int,int> reads;
    std::optional<int> fail_write,fail_read,ignore;
    bool recover_on_activation=false;
    bool activated=false,repair_fans_on_activation=false;
    int fan_reads_before_activation=0;
    bool amd=false;
    State() {
        for(auto [a,v]:std::initializer_list<std::pair<int,int>>{{0x740,26},{0x741,1},{0x727,128},{0x7D2,1},{0x783,100},{0x784,150},{0x785,100},{0x463,10},
            {0x743,7},{0x744,45},{0x746,15},{0x745,255},{0x7A6,0xA0},{0x751,0x90},{0x7C5,0xE0},{0x7C6,4},
            {0x43E,40},{0x44F,40},{0x464,4},{0x465,42},{0x46B,20},{0x46C,4}}) bytes[a]=static_cast<uint8_t>(v);
        for(int i=0;i<16;++i) for(int base:{0xF00,0xF30}) {
            bytes[base+i]=static_cast<uint8_t>(i<10?19+i*8:255);
            bytes[base+16+i]=static_cast<uint8_t>(i==0?0:i<10?11+i*8:255);
            bytes[base+32+i]=static_cast<uint8_t>(i<10?i*20:180);
        }
    }
};
class Fake : public EcChannel {
public:
    explicit Fake(std::shared_ptr<State> s):s_(std::move(s)) {}
    uint32_t ioctl(uint32_t code,std::span<const int32_t> words) override {
        if(GetCurrentThreadId()!=owner_) throw std::runtime_error("EC transport escaped its serialized owner");
        std::lock_guard lock(s_->mutex);int a=words[0];
        if(code==ecSpecIoctl) {static constexpr uint32_t spec[]{200,200,200,10,55,25,0,95,0,0xBB,0,0};return (a>>8)==3 && s_->amd?95:spec[a>>8];}
        if(code==ecReadIoctl) {
            ++s_->reads[a];if(a>=0xF00 && a<0xF70 && !s_->activated) ++s_->fan_reads_before_activation;
            if(s_->fail_read==a) throw std::system_error(ERROR_GEN_FAILURE,std::system_category());return s_->bytes[a];
        }
        if(code!=ecWriteIoctl) throw std::runtime_error("Wrong IOCTL");
        if(s_->fail_write==a) throw std::system_error(ERROR_GEN_FAILURE,std::system_category());
        s_->writes.emplace_back(a,words[1]);
        if(s_->ignore!=a) s_->bytes[a]=static_cast<uint8_t>(words[1]);
        if(a==0x741 && words[1]==5) {
            s_->activated=true;s_->bytes[a]=1;if(s_->recover_on_activation) s_->ignore.reset();
            if(s_->repair_fans_on_activation) for(int i=0;i<16;++i) for(int base:{0xF00,0xF30}) s_->bytes[base+i]=static_cast<uint8_t>(i<10?19+i*8:255);
        }
        if(a==0x786) s_->bytes[0x463]=static_cast<uint8_t>(words[1]&127);
        return 0;
    }
    void reopen() override {}
private:std::shared_ptr<State> s_;DWORD owner_=GetCurrentThreadId();
};
struct Rig {
    std::shared_ptr<State> state=std::make_shared<State>();
    std::unique_ptr<EcSession> session;
    std::unique_ptr<Controller> controller;
    std::vector<std::string> diagnostics;
    Rig(const std::filesystem::path& path,bool start=true,std::unique_ptr<nv::Power> gpu={},std::function<void(std::function<void()>)> restart={},Lighting::Send send={}) {
        std::filesystem::create_directories(path);
        auto log=[this](const std::string& s){diagnostics.push_back(s);};
        session=std::make_unique<EcSession>([this](const std::wstring&){return std::make_unique<Fake>(state);},log,false,[](unsigned){},profiles_directory(),Identity{"X6AR55xY","fixture","intel",uint8_t{26}});
        controller=std::make_unique<Controller>(*session,path,log,[](unsigned){},std::move(restart),false,std::move(gpu),false,std::function<void(Json)>{},std::move(send));
        if(start) controller->start();state->writes.clear();state->reads.clear();
    }
};
UserProfile profile() {
    UserProfile p;p.project_id="IDY_7Y";p.cpu=CpuSettings{100,150,200,95};p.gpu->tgp=120;p.gpu->dynamic_boost_enabled=true;p.gpu->dynamic_boost_watts=15;
    Curve c;for(int i=0;i<10;++i) {c.temp_up.push_back(19+i*8);c.duty.push_back(i*20);}
    p.fan->cpu_curve=c;p.fan->gpu_curve=c;p.fan->curve=true;return p;
}
void peripheral_intent(const std::filesystem::path& root) {
    auto path=root/"peripheral-intent";Rig r(path);auto p=profile();p.lighting->keyboard={true,12,34,56,45};p.lighting->idle_seconds=30;
    write_json(path/"userprofile_1.json",p,true);
    auto result=r.controller->execute(SlotLoad{1});auto saved=read_json(path/"lighting_state.json").get<LightingState>();
    check(result.code==0 && saved.keyboard->r==12 && saved.idle_seconds==30,"real slot orchestration installs and persists lighting intent even with unavailable HID");
    Handle locked(CreateFileW((path/"lighting_state.json").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
    p.lighting->keyboard.r=90;write_json(path/"userprofile_2.json",p,true);auto count=r.diagnostics.size();
    result=r.controller->execute(SlotLoad{2});auto live=r.controller->execute(PeripheralRead{});
    check(result.code==0 && std::any_of(r.diagnostics.begin()+count,r.diagnostics.end(),[](auto& s){return s.find("lighting_state.json save failed")!=std::string::npos;}) && (*live.peripherals)["lightingIntent"]["keyboard"]["r"]==90 && read_json(path/"lighting_state.json").get<LightingState>().keyboard->r==12,
        "failed peripheral direct save logs without rolling back intent or failing core slot selection");
    result=r.controller->execute(LightingIdle{60});check(result.code==0 && !result.saved && result.text.find("persistence failures are logged")!=std::string::npos,"log-only lighting save cannot claim verified persistence");
}
struct GpuFixture : nv::Power {
    bool sleeping=false,dlppc_active=true,fail_configure=false,fail_currents=false,fail_budget=false;int reads=0,sets=0,restarts=0;DWORD restart_delay=0;std::atomic<DWORD> read_delay=0;std::optional<uint32_t> request;
    nv::Currents rails{{1,120000,168000,140000,120000},nv::Rail{1,30000,42000,40000,30000},{}};
    GpuFixture() {enabled=true;}
    int stock_watts() const override {return 175;}
    int minimum_watts() const override {return 5;}
    bool awake() const override {return !sleeping;}
    void read() {++reads;if(sleeping) throw std::runtime_error("Fixture asleep GPU was contacted");}
    nv::PowerLimits limits() override {read();return {5000,95000,enabled?250000u:175000u,request.value_or(95000),request};}
    nv::Currents currents() override {read();if(auto delay=read_delay.exchange(0)) Sleep(delay);if(fail_currents) throw std::runtime_error("Fixture current read failure");return rails;}
    nv::Telemetry telemetry() override {read();nv::Telemetry t;t.nv=123456;t.ms=34567;return t;}
    nv::Budget budget() override {read();if(fail_budget) throw std::runtime_error("Unsupported fixture F7 value");return {250000,350000};}
    std::optional<nv::Dlppc> dlppc() override {read();return nv::Dlppc{dlppc_active,7,2048};}
    void set_dlppc(bool active) override {++sets;dlppc_active=active;}
    bool needs_restart(bool unlock,const GpuSettings& g) override {return unlock!=enabled || (g.nvvdd_ma && *g.nvvdd_ma>int(rails.nv.max)) || (g.msvdd_ma && *g.msvdd_ma>int(rails.ms->max));}
    void configure(bool unlock,const GpuSettings& g,bool allow) override {
        modified=false;
        if(needs_restart(unlock,g)) {if(!allow) throw std::runtime_error("Fixture restart required");modified=uncertain=true;if(fail_configure) throw std::runtime_error("Fixture unverified staging/restart failure");if(restart_delay) Sleep(restart_delay);++restarts;request={};dlppc_active=true;rails.nv.max=unlock?168000:120000;rails.ms->max=unlock?42000:30000;uncertain=false;}
        if(g.nvvdd_ma || g.msvdd_ma || g.board_input_ma) modified=true;
        enabled=unlock;if(g.nvvdd_ma) rails.nv.current=*g.nvvdd_ma;if(g.msvdd_ma) rails.ms->current=*g.msvdd_ma;++sets;
    }
    void apply(int watts) override {read();request=watts*1000;++sets;}
    bool at_most(int watts) override {read();return request && *request<=uint32_t(watts*1000);}
    bool applied(int watts) override {read();return request==uint32_t(watts*1000);}
};
void peripheral_regressions(const std::filesystem::path& root) {
    auto path=root/"peripheral-regressions";int reports=0;bool simulate_gpu=false;Controller* pending=nullptr;
    Rig r(path,true,std::make_unique<GpuFixture>(),[&](auto work){if(simulate_gpu) {simulate_gpu=false;pending->tick(GetTickCount64()+9000);}work();},[&](int,const HidReport&){++reports;});pending=r.controller.get();
    check(pending->execute(LightingApply{0,{true,12,34,56,45}}).code==0,"controller owns real lighting command orchestration");
    auto p=profile();p.lighting.reset();write_json(path/"userprofile_1.json",p,true);
    auto saved=pending->execute(SlotSave{1});auto captured=read_json(path/"userprofile_1.json");
    check(saved.code==0 && captured["lighting"]["keyboardR"]==12,"available lighting capture replaces a null target group");
    r.state->fail_read=0x769;auto read=pending->execute(PeripheralRead{true});
    check(read.code==0 && read.peripherals->contains("logoError") && read.peripherals->contains("battery") && read.peripherals->contains("lightingIntent"),"failed logo query does not suppress independent lighting and battery snapshots");
    saved=pending->execute(SlotSave{1});check(saved.code==0 && read_json(path/"userprofile_1.json")["lighting"]["keyboardR"]==12,"failed logo capture retains target logo without suppressing HID intent");r.state->fail_read.reset();
    r.state->ignore=0x769;r.state->bytes[0x741]=0;r.state->recover_on_activation=true;
    auto result=pending->execute(LightingApply{3,{true,255,128,0,75}});
    check(result.code==1 && result.partial && result.text.find("reactivated")!=std::string::npos,"logo reactivation participates in controller epoch and partial-application accounting");
    pending->execute(Resume{});reports=0;simulate_gpu=true;
    result=pending->execute(CliCommand{{"status","--json"}});
    check(result.code==0 && reports>0,"HID resume runs during NVIDIA work instead of pausing behind its busy flag");
    pending->execute(BatteryPage{true});auto now=GetTickCount64()+2001;
    pending->tick(now);check(!pending->take_battery(),"hidden active battery page has no telemetry subscription work");
    pending->execute(Visibility{true});now=GetTickCount64()+2100;pending->tick(now);check(pending->take_battery().has_value(),"visible active battery page produces bindable telemetry on shared timer");
    pending->tick(now+1000);check(!pending->take_battery(),"battery telemetry does not run at shared one-second cadence");
    pending->tick(now+2000);check(pending->take_battery().has_value(),"battery telemetry uses existing two-second page cadence");
    pending->execute(BatteryPage{false});pending->tick(now+4000);check(!pending->take_battery(),"leaving battery page stops telemetry without a second polling timer");
    Mode mode;mode.pl1=80;mode.pl2=120;mode.pl4=200;mode.tcc_offset=95;r.session->profile.modes=std::map<std::string,Mode>{{"gaming",mode}};
    result=pending->execute(DefaultsApply{"gaming"});check(result.code==0 && result.text.find("logo defaults restored")==std::string::npos,"mode defaults do not claim logo restoration");
}
void startup_degradation(const std::filesystem::path& root) {
    auto start=[&](const char* name,const std::function<void(State&)>& setup,bool& cpu,std::vector<std::string>& errors) {
        auto path=root/name;std::filesystem::create_directories(path);auto s=std::make_shared<State>();setup(*s);
        EcSession session([&](const std::wstring&){return std::make_unique<Fake>(s);},[](const std::string&){},false,[](unsigned){},profiles_directory(),Identity{"X6AR55xY","fixture","intel",uint8_t{26}});
        cpu=session.validation.enabled.contains("cpuPower");
        Controller c(session,path,[](const std::string&){},[](unsigned){},{},false,{},false);errors=c.start();
    };
    bool cpu=false;std::vector<std::string> errors;
    start("diagnostic-read-failure",[](State& s){s.fail_read=0x753;},cpu,errors);
    check(cpu && errors.empty(),"failed diagnostic-only startup read leaves control and EC init intact");
    start("pl4-flag-read-failure",[](State& s){s.fail_read=0x727;},cpu,errors);
    check(!cpu && errors.size()==1 && errors[0].starts_with("0x726, 0x727: "),"unknown PL4 encoding disables CPU power and failed init step is reported");
    start("activation-not-acknowledged",[](State& s){s.bytes[0x741]=0;s.fail_write=0x741;},cpu,errors);
    check(errors.size()==2 && errors[0].starts_with("0x741: ") && errors[1].find("not acknowledged")!=std::string::npos,"unacknowledged activation is a startup warning, not a controller failure");
}
void gpu_integration(const std::filesystem::path& root) {
    auto backend=std::make_unique<GpuFixture>();auto driver=backend.get();Rig r(root/"gpu",true,std::move(backend));auto& c=*r.controller;
    GpuSettings combined;combined.tgp=70;auto sets=driver->sets;
    check(c.execute(GpuApply{combined,true,false,CpuSettings{0,0,0,0}}).code==1 && r.state->writes.empty() && driver->sets==sets,"combined UI power validates CPU before NVIDIA or EC writes");
    auto applied=c.execute(GpuApply{combined,true,false,CpuSettings{100,150,200,95}});
    auto cpu=std::find_if(r.state->writes.begin(),r.state->writes.end(),[](auto w){return w.first==0x783;}),gpu=std::find_if(r.state->writes.begin(),r.state->writes.end(),[](auto w){return w.first==0x744;});
    check(applied.code==0 && applied.applied && cpu<gpu && gpu!=r.state->writes.end(),"combined UI power preserves CPU-before-GPU EC order in one explicit application");
    r.state->writes.clear();r.state->fail_write=0x783;combined.tgp=90;
    auto partial=c.execute(GpuApply{combined,true,false,CpuSettings{90,150,200,95}});
    check(partial.code==1 && partial.partial && driver->request==70000u && std::none_of(r.state->writes.begin(),r.state->writes.end(),[](auto w){return w.first==0x744;}),"failed combined CPU batch prevents subsequent GPU power application");r.state->fail_write.reset();r.state->writes.clear();
    GpuSettings g;g.tgp=70;check(c.execute(GpuApply{g}).code==0 && r.state->bytes[0x744]==0 && driver->request==70000u,"below-EC-floor request uses EC minimum plus real driver path");
    driver->sleeping=true;int before=driver->reads;auto status=c.execute(CliCommand{{"status","--json"}});auto j=parse(status.text);
    check(status.code==0 && j["gpu"]["tgp"]["value"]==70 && j["gpu"]["requestedWatts"].is_null() && driver->reads==before,"entire passive status path preserves below-floor intent without touching sleeping driver");
    c.execute(AcEvent{true});c.tick(GetTickCount64()+9000);c.execute(Resume{});c.tick(GetTickCount64()+9000);
    check(driver->reads==before,"AC/resume delayed checks do not contact sleeping GPU");driver->sleeping=false;
    auto response=c.execute(CliCommand{{"gpu","--nvvdd","125.001A","--json"}});j=parse(response.text);
    check(response.code==0 && driver->rails.nv.current==125001 && j["nvvdd"]["limitMa"]==125001 && driver->request==70000u,"current-only CLI parses exact milliamps once and does not rewrite GPU power");
    auto plain=c.execute(CliCommand{{"status"}});check(plain.code==0 && plain.text.find("NVVDD limit 125.001 A")!=std::string::npos && plain.text.find("Clock caps: current controller")!=std::string::npos && plain.text.find("board input limiter (IPC)")!=std::string::npos,"plain GPU status retains source current/telemetry fields and units");
    c.execute(Visibility{true});r.state->reads.clear();before=driver->reads;auto live=c.presentation(false,true,0);
    check(r.state->reads.empty() && driver->reads==before+1 && live["gpuLive"]["nvvdd"]["measuredMa"]==123456,"Power tab live refresh reads only driver telemetry, no EC bytes");
    c.execute(UiPage{6});before=driver->reads;auto info=c.presentation(false,false,6);auto again=c.presentation(false,false,6);
    check(info["gpuBudget"]["laptopWatts"]==250 && !again.contains("gpuBudget") && driver->reads==before+1,"System Info reads driver budgets at most every five seconds");c.execute(UiPage{0});c.execute(Visibility{false});
    before=driver->sets;check(c.execute(CliCommand{{"gpu","--nvvdd","125.0001A"}}).code==2 && driver->sets==before,"fractional milliamp request fails without writes");
    check(c.execute(CliCommand{{"gpu","--nvvdd","200A"}}).code==1 && driver->sets==before,"CLI above active ceiling refuses without restart or current writes");
    driver->fail_budget=true;r.state->writes.clear();GpuSettings above;above.tgp=200;
    auto refused_budget=c.execute(GpuApply{above});check(refused_budget.code==1 && !refused_budget.partial && r.state->writes.empty() && driver->request==70000u && driver->enabled,"unsupported budget refuses above-stock GPU EC/F8 requests without discarding the working owner");driver->fail_budget=false;
    driver->request={};status=c.execute(CliCommand{{"status","--json"}});j=parse(status.text);
    check(j["gpu"]["tgp"]["value"]==95 && j["gpu"]["requestedWatts"].is_null(),"awake status drops cached below-floor setpoint when driver reset cleared the actual request");
    auto p=profile();p.gpu->bios_override_enabled=false;write_json(root/"gpu/userprofile_1.json",p,true);
    auto loaded=c.execute(SlotLoad{1,false});check(loaded.code==0 && driver->enabled && driver->rails.nv.current==120000 && driver->rails.ms->current==30000,"automatic legacy slot retains unlock but restores unspecified stock currents");
    p.gpu->bios_override_enabled=true;p.gpu->nvvdd_ma=160000;p.gpu->board_input_ma=12000;driver->rails.nv.max=130000;write_json(root/"gpu/userprofile_1.json",p,true);
    loaded=c.execute(SlotLoad{1,false});check(loaded.code==0 && driver->restarts==0 && driver->rails.nv.current==130000 && loaded.text.find("restart")!=std::string::npos,"automatic slot clamps active currents, skips only unsupported board input, and reports notice");
    check(c.execute(DlppcApply{true}).code==0 && read_json(root/"gpu/settings.json")["disableDlppc"]==true,"DLPPC uses existing global preference storage");
    driver->dlppc_active=true;c.execute(AcEvent{false});c.tick(GetTickCount64()+6000);check(!driver->dlppc_active,"event-delayed DLPPC reapplication restores saved disable after driver reset");
    auto saved=c.execute(SlotSave{2});auto capture=read_json(root/"gpu/userprofile_2.json");check(saved.code==0 && capture["gpu"]["nvvddMilliamps"]==130000 && capture["gpu"]["biosOverrideEnabled"]==true,"real owner capture marks override only when captured settings need it");
    auto no_rails=profile();no_rails.gpu->bios_override_enabled=true;write_json(root/"gpu/userprofile_3.json",no_rails,true);driver->fail_currents=true;
    check(c.execute(SlotLoad{3,false}).code==0,"explicit override profile without currents does not depend on current-policy preflight");driver->fail_currents=false;
    check(c.execute(ThermalTest{}).code==0 && driver->request && *driver->request<=95000,"GPU thermal hold includes verified lower driver request");
    auto other=std::make_unique<GpuFixture>();auto restart_driver=other.get();other->enabled=false;Rig* pending=nullptr;
    bool trip_pending=true;Rig restarted(root/"gpu-restart",true,std::move(other),[&](std::function<void()> work) {
        if(!pending || !trip_pending) {work();return;}trip_pending=false;
        auto& ctrl=*pending->controller;pending->state->bytes[0x44F]=80;
        ctrl.tick();ctrl.tick();ctrl.tick();check(ctrl.thermal().tripped(),"thermal sampling continues while restart work is outstanding");
        check(std::none_of(pending->state->writes.begin(),pending->state->writes.end(),[](auto w){return w.first>=0xF00 && w.first<0xF60;}),"normal linked-fan writes pause during GPU restart; emergency EC actions remain serialized");work();
    });pending=&restarted;
    auto linked_fan=*profile().fan;linked_fan.linked=true;restarted.controller->execute(FanApply{linked_fan});restarted.state->writes.clear();
    restarted.controller->execute(ProtectionApply{{true,50,true,true,false}});
    auto unlocked=restarted.controller->execute(GpuUnlock{true});
    check(unlocked.code==0 && restart_driver->restarts==1 && restarted.controller->thermal().complete() && restart_driver->request && *restart_driver->request<=95000,"restart completion invalidates old verification and applies intervening thermal hold before normal power");
    auto failed_driver=std::make_unique<GpuFixture>();auto failed_gpu=failed_driver.get();failed_driver->enabled=false;
    failed_driver->fail_configure=true;Rig failed(root/"gpu-restart-failed",true,std::move(failed_driver));
    auto failure=failed.controller->execute(GpuUnlock{true});
    check(failure.code==1 && failure.partial && failed.controller->execute(GpuApply{g}).code==1,"failed restart cannot authorize or report normal GPU application");
    check(failed.controller->execute(ThermalTest{}).code==0 && failed_gpu->request && *failed_gpu->request<=95000,"uncertain restart still attempts and verifies both EC and driver emergency minimum");
    auto untouched=std::make_unique<GpuFixture>();untouched->enabled=false;bool allocation_failure=true;
    Rig allocation(root/"gpu-allocation-failure",true,std::move(untouched),[&](std::function<void()> work){if(allocation_failure) {allocation_failure=false;throw std::bad_alloc();}work();});
    auto no_change=allocation.controller->execute(GpuUnlock{true});g.tgp=120;
    check(no_change.code==1 && !no_change.partial && allocation.controller->execute(GpuApply{g}).code==0,"pre-work allocation failure does not latch restart uncertainty or claim partial hardware changes");
    auto locked_driver=std::make_unique<GpuFixture>();locked_driver->enabled=false;locked_driver->fail_currents=true;Rig locked(root/"gpu-stock-no-rails",true,std::move(locked_driver));
    write_json(root/"gpu-stock-no-rails/userprofile_1.json",profile(),true);check(locked.controller->execute(SlotLoad{1,false}).code==0,"connected locked stock profile does not read unused current policies");
    Rig* defaults_pending=nullptr;bool defaults_trip=true;auto default_driver=std::make_unique<GpuFixture>();auto defaults_gpu=default_driver.get();
    Rig defaults(root/"gpu-defaults-trip",true,std::move(default_driver),[&](std::function<void()> work) {
        if(defaults_pending && defaults_trip) {
            defaults_trip=false;auto& s=*defaults_pending->state;
            check(s.bytes[0x783]==0 && s.bytes[0x744]==0 && (s.bytes[0x751]&64)==0,"firmware EC defaults finish before slow NVIDIA restoration begins");
            s.bytes[0x44F]=80;for(int i=0;i<3;++i) defaults_pending->controller->tick();
        }
        work();
    });defaults_pending=&defaults;
    defaults.controller->execute(ProtectionApply{{true,50,true,true,false}});
    auto reset=defaults.controller->execute(DefaultsApply{""});auto& ds=*defaults.state;
    check(reset.code==0 && defaults_gpu->restarts==1 && defaults.controller->thermal().complete() && ds.bytes[0x783]==control_limit(defaults.session->profile,"PL1").min && ds.bytes[0x744]==0 && (ds.bytes[0x751]&64)!=0,"trip during firmware defaults/restart retains GPU/CPU/fan holds without normal post-wait overwrite");
    Rig* capture_pending=nullptr;bool capture_trip=true;
    Rig interrupted_capture(root/"gpu-capture-trip",true,std::make_unique<GpuFixture>(),[&](std::function<void()> work) {
        if(capture_pending && capture_trip) {capture_trip=false;capture_pending->state->bytes[0x44F]=80;for(int i=0;i<3;++i) capture_pending->controller->tick();}work();
    });capture_pending=&interrupted_capture;
    interrupted_capture.controller->execute(ProtectionApply{{true,50,true,true,false}});
    check(interrupted_capture.controller->execute(SlotSave{1}).code==1 && !std::filesystem::exists(root/"gpu-capture-trip/userprofile_1.json") && interrupted_capture.controller->thermal().complete(),"trip during slow NVIDIA capture cannot overwrite a normal slot with emergency settings");
    Rig* combined_pending=nullptr;bool combined_trip=true;auto combined_driver=std::make_unique<GpuFixture>();auto combined_gpu=combined_driver.get();size_t trip_writes=0;
    Rig combined_rig(root/"gpu-combined-trip",true,std::move(combined_driver),[&](std::function<void()> work) {
        auto sets=combined_gpu->sets;work();
        if(combined_pending && combined_trip && combined_gpu->sets>sets) {combined_trip=false;combined_pending->state->bytes[0x44F]=80;for(int i=0;i<3;++i) combined_pending->controller->tick();trip_writes=combined_pending->state->writes.size();}
    });combined_pending=&combined_rig;
    combined_rig.controller->execute(ProtectionApply{{true,50,true,true,false}});
    GpuSettings with_currents;with_currents.tgp=120;with_currents.nvvdd_ma=125000;auto& cs=*combined_rig.state;
    combined_rig.controller->execute(GpuApply{with_currents,true,false,CpuSettings{110,150,200,95}});
    check(!combined_trip && combined_rig.controller->thermal().tripped() && std::none_of(cs.writes.begin()+trip_writes,cs.writes.end(),[](auto w){return w.first==0x783 && w.second==110;}) && cs.bytes[0x783]==control_limit(combined_rig.session->profile,"PL1").min,"trip during NVIDIA work of a combined apply keeps the CPU hold without a transient normal write");
    auto startup_path=root/"gpu-saved-dlppc";std::filesystem::create_directories(startup_path);{ProfileStore store(startup_path,false);store.set_disable_dlppc(true);}
    auto saved_driver=std::make_unique<GpuFixture>();auto saved_gpu=saved_driver.get();Rig saved_rig(startup_path,true,std::move(saved_driver));
    check(!saved_gpu->dlppc_active,"only saved DLPPC disable is reapplied at controller startup");
    Handle lock(CreateFileW((startup_path/"settings.json").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
    auto not_saved=saved_rig.controller->execute(DlppcApply{false});
    check(not_saved.code==1 && not_saved.partial && saved_gpu->dlppc_active && read_json(startup_path/"settings.json")["disableDlppc"]==true,"DLPPC save failure rolls back stored preference, reports unsaved hardware change without invented rollback");
}
void integration(const std::filesystem::path& root) {
    Rig r(root/"integrated");auto& c=*r.controller;auto s=r.state;
    check(c.execute(CpuApply{{0,0,0,0}}).code==1 && s->writes.empty(),"zero-valued CPU group fails before any write");
    auto partial_limits=r.session->profile;partial_limits.limits.erase("PL1");
    check(cpu_writes(partial_limits,15,150,200,95).front().value==15 && !partial_limits.limits.contains("PL1"),"per-key controller fallback does not change whole-map default semantics");
    write_json(root/"integrated/userprofile_1.json",{{"cpu",nullptr}},false);
    check(c.execute(SlotLoad{1}).code==1 && s->writes.empty(),"null required persisted group fails before writes");
    write_json(root/"integrated/userprofile_1.json",{{"fan",{{"cpuCurve",{{"tempUp",nullptr}}}}}},false);
    check(c.execute(SlotLoad{1}).code==1 && s->writes.empty(),"null required fan array is not defaulted into an applicable curve");
    auto oversized=*profile().fan;Curve large;
    for(int i=0;i<17;++i) {large.temp_up.push_back(19+i*4);large.duty.push_back(i*10);}
    oversized.cpu_curve=large;
    check(c.execute(FanApply{oversized}).code==1 && s->writes.empty(),"oversized active fan curve is refused rather than silently truncated");
    auto p=profile();p.cpu->temp_limit=200;write_json(root/"integrated/userprofile_1.json",p,true);
    check(c.execute(SlotLoad{1}).code==1 && s->writes.empty(),"invalid final CPU profile field blocks unrelated fan writes");
    p=profile();p.project_id="other-model";write_json(root/"integrated/userprofile_1.json",p,true);
    check(c.execute(SlotLoad{1}).code==1 && s->writes.empty(),"foreign-model slot refuses before any core write");
    p=profile();write_json(root/"integrated/userprofile_1.json",p,true);
    check(c.execute(SlotLoad{1}).code==0 && c.active_slot()==1,"real slot load applies on controller and commits selection");
    check(s->bytes[0x783]==100 && s->bytes[0x744]==25 && s->bytes[0x7A6]==0x80,"slot values and runtime battery bit3 preserve unrelated bits");
    check(s->writes.front().first==0x7C5 && s->writes[96].first==0x7C6,"slot fan tables precede CPU/GPU settings");
    s->writes.clear();auto status=c.execute(CliCommand{{"status","--json"}});
    auto j=parse(status.text);check(status.code==0 && j["model"]=="IDY_7Y" && j["cpu"]["pl4"]["value"]==200 && s->writes.empty(),"native status JSON reads live settings without writes");
    for(auto args:std::vector<std::vector<std::string>>{{"cpu","--pl1","80","--pl1","90"},{"cpu","--pl1","80","--temp-limit","no"},{"fan","boost","ON"}})
        check(c.execute(CliCommand{args}).code==2 && s->writes.empty(),"invalid CLI request refuses without hardware writes");
    check(c.execute(CliCommand{{"gpu","--nvvdd","0.0001A"}}).code==1 && s->writes.empty(),"disconnected current request refuses before numeric parsing, matching source ordering");
    check(c.execute(CliCommand{{"cpu","--pl1","80","--json"}}).code==0 && s->bytes[0x783]==80,"native CLI uses real CPU controller");
    s->writes.clear();s->fail_write=0xF22;
    auto failed=c.execute(FanApply{*p.fan});check(failed.code==1 && failed.partial && failed.fan_application==FanApplication::partial && s->writes.back().first!=0x7C6,"partial fan write loses applied confidence and reports failure without final latch");
    s->fail_write.reset();s->writes.clear();check(c.execute(FanApply{*p.fan}).fan_application==FanApplication::applied && s->writes.size()==97,"fan retry sends entire required batch and commits applied confidence");
    auto f=*p.fan;f.linked=true;s->writes.clear();check(c.execute(FanApply{f}).code==0 && c.linked() && s->writes.size()==97,"linked enable synchronously writes fresh safety curves");
    s->writes.clear();c.tick();c.tick();check(s->writes.empty(),"stable linked duty/headroom does not rewrite curves");
    auto edit=f;edit.cpu_curve->temp_up[1]+=1;
    check(c.execute(FanEdit{edit}).code==0 && s->writes.empty() && c.fan_intent().cpu_curve->temp_up[1]==edit.cpu_curve->temp_up[1],"linked editor updates owned intent without immediate or redundant hardware writes");
    check(c.execute(FanRead{}).code==1 && c.linked(),"EC Read cannot replace the linked editor with temporary safety ramps");
    s->fail_read=0x44F;c.tick();check(s->writes.empty() && s->bytes[0xF29]==200 && s->bytes[0xF59]==200,"invalid sensors retain crash-safe tails");
    s->fail_read.reset();s->bytes[0x43E]=70;s->fail_write=0xF22;c.tick();s->fail_write.reset();s->writes.clear();c.tick();
    check(s->writes.size()==97,"partial linked batch is retried even when duty request repeats");
    f.linked=false;c.execute(FanApply{f});s->writes.clear();c.tick();check(s->writes.empty(),"linked disable writes panel curves once and unsubscribes");
    auto save=c.execute(SlotSave{2});check(save.saved && save.code==0,"controller captures and saves a normal slot");
    auto saved=read_json(root/"integrated/userprofile_2.json").get<UserProfile>();
    check(saved.fan->cpu_curve->temp_up.size()==16 && saved.fan->cpu_curve->temp_down[1]==15 && saved.fan->cpu_curve->temp_down[15]==255,"capture recomputes hysteresis and padding before serialization");
    {
        Handle lock(CreateFileW((root/"integrated/settings.json").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
        auto partial=c.execute(SlotSave{3});
        check(partial.code==1 && !partial.saved && partial.partial && partial.slot_committed && c.active_slot()==2 && std::filesystem::exists(root/"integrated/userprofile_3.json") && partial.text.find("was saved")!=std::string::npos,"committed slot plus failed selection save reports partial outcome and retains selection");
        partial=c.execute(SlotLoad{1});check(partial.code==1 && partial.applied && !partial.saved && c.active_slot()==1,"successful synchronous load plus failed selection save does not claim rollback");
    }
    c.execute(SlotLoad{2});
    s->bytes[0x741]=0;s->ignore=0x783;s->recover_on_activation=true;s->bytes[0x783]=100;
    auto lost=c.execute(CpuApply{{90,150,200,95}});check(lost.code==1 && lost.partial,"activation loss cannot falsely complete a recovered single-write retry");
    c.tick();check(s->bytes[0x783]==80,"deferred nonrecursive recovery reapplies active saved slot once");
    c.execute(Resume{});s->reads.clear();c.tick();check(!s->reads.empty() && !s->reads.contains(0x741),"resume keeps monitoring and defers activation probing during three-second settle");
    Sleep(3100);s->reads.clear();c.tick();check(s->reads[0x741]==1,"settled resume and watchdog share one activation probe");
}
void thermal(const std::filesystem::path& root) {
    auto path=root/"thermal";Rig r(path);auto& c=*r.controller;auto s=r.state;
    ProtectionSettings settings;settings.enabled=true;settings.threshold=50;settings.reduce_cpu=true;settings.fan_boost=true;
    c.execute(ProtectionApply{settings});s->writes.clear();s->bytes[0x44F]=55;
    c.tick();check(!c.thermal().tripped(),"thermal minimum valid sample count prevents early trip");c.tick();
    check(c.thermal().tripped() && c.thermal().state().held==7 && c.thermal().complete(),"three valid samples trip shared thermal controller and verify all actions");
    check(c.thermal().peak_temperature()==55 && c.thermal().temperature()==55,"controller retains peak and current thermal readings independently of UI delivery");
    auto first_cpu=std::find_if(s->writes.begin(),s->writes.end(),[](auto w){return w.first==0x783;});
    auto first_fan=std::find_if(s->writes.begin(),s->writes.end(),[](auto w){return w.first==0x751;});
    check(first_fan<first_cpu && read_json(path/"settings.json")["thermalProtection"]["Held"]==7,"GPU then fan then CPU actions precede durable trip state");
    s->writes.clear();for(int i=0;i<5;++i)c.tick();check(s->writes.empty(),"five-tick held maintenance uses verified same-epoch readback");
    check(c.execute(ThermalReset{}).code==1 && c.thermal().tripped(),"enabled reset refuses a fresh valid reading while rolling average remains above trigger");
    s->bytes[0x751]&=~64;for(int i=0;i<5;++i)c.tick();check((s->bytes[0x751]&64)!=0 && c.thermal().lost_alerts()==0,"maintenance reasserts firmware-cleared boost without a lost-protection alert");
    c.execute(Resume{});Sleep(3100);c.tick();
    check(c.thermal().complete() && c.thermal().lost_alerts()==0,"resume invalidation alone does not raise a lost-protection alert");
    s->bytes[0x751]&=~64;s->fail_write=0x751;for(int i=0;i<5;++i)c.tick();
    check(!c.thermal().complete() && c.thermal().lost_alerts()==1,"failed maintenance write counts one lost-protection alert");
    s->fail_write.reset();for(int i=0;i<5;++i)c.tick();
    check(c.thermal().complete() && c.thermal().lost_alerts()==1,"restored protection does not alert again");
    s->writes.clear();check(c.execute(CpuApply{{80,150,200,95}}).code==1 && s->writes.empty(),"held state blocks deliberate power writes");
    check(c.execute(FanBoost{false}).code==1 && c.execute(FanBoost{true}).code==0,"fan hold refuses off but allows added cooling");
    check(c.execute(SlotSave{1}).code==1 && !std::filesystem::exists(path/"userprofile_1.json"),"tripped emergency settings cannot overwrite a normal slot");
    s->fail_read=0x44F;check(c.execute(ThermalReset{}).code==1,"enabled reset requires fresh valid temperature");s->fail_read.reset();
    settings.enabled=false;c.execute(ProtectionApply{settings});check(c.thermal().tripped(),"disabling detection does not release hold");
    Handle lock(CreateFileW((path/"settings.json").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
    auto failed=c.execute(ThermalReset{});check(failed.code==1 && c.thermal().tripped(),"failed release persistence retains controller authority");
    s->writes.clear();check(c.execute(CpuApply{{80,150,200,95}}).code==1 && s->writes.empty(),"failed release cannot authorize subsequent command");
    lock.reset();check(c.execute(ThermalReset{}).code==0 && !c.thermal().tripped(),"durable reset releases hold when detection disabled");
    check(!c.thermal().average() && !c.thermal().temperature(),"disabled reset clears live temperature window without discarding trip history");
    settings.enabled=true;c.execute(ProtectionApply{settings});s->fail_read=0x44F;
    for(int i=0;i<4;++i)c.tick();check(!c.thermal().tripped(),"four invalid recurring samples do not trip");c.tick();check(c.thermal().tripped(),"fifth invalid sample trips even hidden");
    // All independent actions are still attempted when GPU verification fails.
    s->fail_read.reset();c.execute(ProtectionApply{ProtectionSettings{}});c.execute(ThermalReset{});
    settings.enabled=false;c.execute(ProtectionApply{settings});s->fail_write=0x744;s->writes.clear();c.execute(ThermalTest{});
    check(c.thermal().tripped() && !c.thermal().complete() && (s->bytes[0x751]&64) && s->bytes[0x783]==10,"failed GPU action does not suppress independent fan/CPU emergency writes");
}
void recovery(const std::filesystem::path& root) {
    auto path=root/"recovery";std::filesystem::create_directories(path);
    write_json(path/"settings.json",{{"thermalProtection",{{"Stage",0},{"Held",0}}}},false);
    write_json(path/"settings.json.tmp",{{"thermalProtection",{{"Stage",0},{"Held",6}}}},false);
    Rig r(path,false);r.controller->start();
    check(r.controller->thermal().state().held==7 && r.controller->thermal().complete(),"restart retains invalid-pending CPU/Fan obligations and enforces before normal writes");
    r.state->writes.clear();check(r.controller->execute(CpuApply{{80,150,200,95}}).code==1 && r.state->writes.empty(),"recovered hold actually blocks normal command");
    auto p=profile();write_json(path/"userprofile_1.json",p,true);
    auto automatic=r.controller->execute(SlotLoad{1,false});
    check(automatic.code==0 && r.controller->thermal().tripped() && r.state->bytes[0x783]==10 && r.state->bytes[0x744]==0,"automatic slot application retains and substitutes recovered held intent");
    check(std::none_of(r.state->writes.begin(),r.state->writes.end(),[](auto w){return w.first==0x783 || w.first==0x744;}),"same-epoch verified holds are not duplicated by automatic slot recovery");
    r.state->writes.clear();
    r.controller->execute(ProtectionApply{{true,50,false,false,false}});r.state->bytes[0x44F]=70;
    check(r.controller->execute(ThermalReset{}).code==1 && r.controller->thermal().tripped(),"hot fresh startup mean cannot release recovered hold before three detector samples");
    Handle lock(CreateFileW((path/"settings.json").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
    check(r.controller->execute(SlotLoad{1,true}).code==1 && r.controller->thermal().tripped() && r.state->writes.empty(),"deliberate profile release failure performs no normal writes");
    lock.reset();check(r.controller->execute(SlotLoad{1,true}).code==0 && !r.controller->thermal().tripped(),"deliberate profile release commits before synchronous application");
    auto no_gpu=root/"no-gpu";std::filesystem::create_directories(no_gpu);write_json(no_gpu/"settings.json",{{"thermalProtection",{{"Stage",0},{"Held",0}}}},false);
    write_json(no_gpu/"settings.json.tmp",{{"thermalProtection",{{"Stage",0},{"Held",6}}}},false);
    auto s=std::make_shared<State>();s->bytes[0x7D2]=0;
    EcSession session([&](const std::wstring&){return std::make_unique<Fake>(s);},[](const std::string&){},false,[](unsigned){},profiles_directory(),Identity{"X6AR55xY","fixture","intel",uint8_t{26}});
    Controller c(session,no_gpu,[](const std::string&){},[](unsigned){},{},false,{},false);c.start();
    check(c.thermal().state().held==6 && c.thermal().complete(),"no-GPU recovery preserves Held 6 without inventing GPU fallback");
    auto failed_path=root/"failed-trip";std::filesystem::create_directories(failed_path);
    write_json(failed_path/"settings.json",{{"thermalProtection",{{"Stage",0},{"Held",0}}}},false);
    Rig failed(failed_path);failed.controller->execute(ProtectionApply{{false,50,true,true,false}});
    Handle trip_lock(CreateFileW((failed_path/"settings.json").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
    failed.controller->execute(ThermalTest{});
    check(failed.controller->thermal().tripped() && !failed.controller->thermal().complete() && failed.state->bytes[0x783]==10 && (failed.state->bytes[0x751]&64),"failed trip save retains intent after all hardware emergency actions");
    trip_lock.reset();for(int i=0;i<5;++i) failed.controller->tick();
    check(failed.controller->thermal().complete() && read_json(failed_path/"settings.json")["thermalProtection"]["Held"]==7,"held maintenance retries a failed trip save without losing obligations");
    Rig reset(root/"reset-profile");write_json(root/"reset-profile/userprofile_1.json",profile(),true);
    reset.controller->execute(SlotLoad{1});reset.controller->execute(ProtectionApply{{false,50,true,true,false}});reset.controller->execute(ThermalTest{});
    check(reset.controller->execute(ThermalReset{}).code==0 && reset.state->bytes[0x783]==100 && reset.state->bytes[0x744]==25 && reset.controller->active_slot()==1,"successful durable reset reapplies active normal profile");
}
void defaults_and_no_slot(const std::filesystem::path& root) {
    Rig r(root/"defaults");auto& c=*r.controller;auto s=r.state;
    Mode mode;mode.pl1=80;mode.pl2=120;mode.pl4=200;mode.tcc_offset=95;mode.ctgp=110;mode.dynamic_boost=15;
    r.session->profile.modes=std::map<std::string,Mode>{{"gaming",mode}};
    check(c.execute(DefaultsApply{"gaming"}).code==0 && s->bytes[0x783]==80 && s->bytes[0x744]==15,"model-derived mode defaults apply through guarded core controller");
    r.session->profile.modes->at("gaming").ctgp=500;s->writes.clear();
    check(c.execute(DefaultsApply{"gaming"}).code==1 && s->writes.empty(),"invalid later GPU default preflights before unrelated CPU writes");
    check(c.execute(DefaultsApply{""}).code==0 && s->bytes[0x783]==0 && s->bytes[0x743]%8==0 && s->bytes[0x744]==0 && (s->bytes[0x7C5]&128)==0,"firmware defaults hand core power/fan control back without inferred preset");
    Rig no_slot(root/"no-slot");auto n=no_slot.state;
    for(int i=0;i<16;++i) n->bytes[0xF00+i]=0;
    n->bytes[0x741]=0;n->ignore=0x783;n->recover_on_activation=true;
    no_slot.controller->execute(CpuApply{{90,150,200,95}});n->writes.clear();no_slot.controller->tick();
    check(n->bytes[0xF00]==19 && n->bytes[0xF09]==91 && n->writes.size()==97,"no-slot activation recovery repairs invalid tables from retained controller curves");
    auto f=*profile().fan;f.linked=true;c.execute(FanApply{f});
    mode.fan_curve=FanDefaults{Curve{{30,40,50},{},{0,80,200}},Curve{{35,45,55},{},{0,90,200}}};
    r.session->profile.modes->at("gaming")=mode;s->fail_write=0xF22;s->writes.clear();
    auto partial=c.execute(DefaultsApply{"gaming"});
    check(partial.code==1 && partial.partial && !partial.applied && !c.linked() && partial.fan_application==FanApplication::partial && c.fan_intent().cpu_curve->temp_up[0]==30,"partial mode defaults retain requested intent without claiming success and stop linked writes");
    s->fail_write.reset();s->writes.clear();c.tick();check(s->writes.empty(),"linked tracking cannot undo partially applied mode defaults on the next tick");
    auto done=c.execute(DefaultsApply{"gaming"});
    check(done.applied && !done.saved && done.fan_application==FanApplication::applied && done.text.find("unsaved")!=std::string::npos,"completed mode defaults report unsaved core application and successful fan request");
    check(c.execute(SlotSave{4}).saved && read_json(root/"defaults/userprofile_4.json")["fan"]["cpuCurve"]["tempUp"][0]==30,"capture after defaults saves the new fan intent rather than stale pre-default curves");
    c.execute(FanApply{f});s->fail_write=0x744;s->writes.clear();partial=c.execute(DefaultsApply{""});
    check(partial.code==1 && partial.partial && !c.linked() && partial.fan_application==FanApplication::partial,"early firmware-default failure also disables linking before its first write");
    s->fail_write.reset();s->writes.clear();c.tick();check(s->writes.empty(),"partial firmware defaults are not overwritten by an old linked request");
}
void startup_and_eligibility(const std::filesystem::path& root) {
    Rig post(root/"post-activation",false);auto s=post.state;
    for(int i=0;i<16;++i) s->bytes[0xF00+i]=0;
    s->repair_fans_on_activation=true;post.controller->start();
    check(s->fan_reads_before_activation==0 && post.session->fans.valid() && post.controller->fan_intent().cpu_curve->temp_up.front()==19,"control fan detection and seeding observe the post-activation table only");
    Rig cold(root/"cold",false);s=cold.state;
    for(int i=0;i<16;++i) {s->bytes[0xF00+i]=0;s->bytes[0xF30+i]=255;}
    cold.session->profile.default_fan=FanDefaults{Curve{{42,52},{},{0,200}},std::nullopt};
    cold.controller->start();
    auto tables=[](auto w){return w.first>=0xF00 && w.first<0xF60;};
    check(cold.session->profile.fan_table.has_gpu && (s->bytes[0x7C5]&128)==0 && std::none_of(s->writes.begin(),s->writes.end(),tables),"invalid no-slot startup retains profile GPU presence and parks custom mode without seed writes");
    check(cold.controller->fan_review_required() && cold.controller->fan_application()==FanApplication::unapplied && cold.controller->fan_intent().cpu_curve->temp_up.front()==30 && cold.controller->fan_intent().gpu_curve->duty[1]==50,"missing GPU default uses the reference editable starter, not a CPU-derived GPU default");
    check(cold.controller->execute(SlotSave{1}).code==1 && !std::filesystem::exists(root/"cold/userprofile_1.json"),"unreviewed generic seed cannot be saved into an automatically applied slot");
    s->bytes[0x741]=0;s->ignore=0x783;s->recover_on_activation=true;cold.controller->execute(CpuApply{{90,150,200,95}});s->writes.clear();cold.controller->tick();
    check(std::none_of(s->writes.begin(),s->writes.end(),tables) && cold.controller->fan_review_required(),"no-slot recovery cannot auto-qualify or write an editable generic starter");
    auto reviewed=cold.controller->fan_intent();reviewed.curve=true;
    check(cold.controller->execute(FanApply{reviewed}).code==0 && !cold.controller->fan_review_required() && cold.controller->execute(SlotSave{1}).saved,"explicit reviewed Apply makes fallback intent eligible for capture");
    Rig single(root/"single",false);s=single.state;
    for(int i=0;i<16;++i) s->bytes[0xF30+i]=255;
    single.controller->start();s->writes.clear();auto p=profile();p.fan->gpu_curve=Curve{};p.fan->linked=true;write_json(root/"single/userprofile_1.json",p,true);
    check(single.controller->execute(SlotLoad{1}).code==0 && !single.controller->linked() && std::none_of(s->writes.begin(),s->writes.end(),[](auto w){return w.first>=0xF30 && w.first<0xF60;}),"single-fan slot skips unused GPU curve validation/writes and unavailable linked mode");
    check(single.controller->execute(SlotSave{1}).saved,"single-fan capture does not require an unused GPU curve");
    Rig unsupported(root/"unsupported",false);unsupported.session->profile.fan_table.format="unsupported";unsupported.controller->start();s=unsupported.state;s->writes.clear();
    p=profile();p.fan->cpu_curve=Curve{};p.fan->gpu_curve=Curve{};write_json(root/"unsupported/userprofile_1.json",p,true);
    check(unsupported.controller->execute(SlotLoad{1}).code==0 && s->bytes[0x783]==100 && std::none_of(s->writes.begin(),s->writes.end(),tables),"unsupported curve format skips curves without blocking power, boost, switch or battery slot controls");
    check(unsupported.controller->execute(FanApply{*p.fan}).code==1,"direct unsupported curve request does not falsely report application");
}
void capture_preservation(const std::filesystem::path& root) {
    auto path=root/"capture";Rig r(path);auto p=profile();p.lighting->local_dimming=true;p.lighting->logo={true,12,34,56,80};p.lighting->idle_seconds=73;
    p.cooler=CoolerSettings{};p.cooler->fan_duty=82;p.cooler->voltage=PumpVoltage::V11;p.cooler->rgb_mode=CoolerRgbMode::BreatheColor;
    write_json(path/"userprofile_1.json",p,true);auto before=read_json(path/"userprofile_1.json");
    auto saved=r.controller->execute(SlotSave{1});auto after=read_json(path/"userprofile_1.json");
    check(saved.saved && saved.text.find("preserved")!=std::string::npos && before["lighting"]==after["lighting"] && before["liquidCooler"]==after["liquidCooler"],"core-only overwrite preserves every known target-slot lighting/dimming/cooler field without claiming hardware capture");
    write_json(path/"userprofile_2.json",{{"lighting",nullptr},{"liquidCooler",nullptr}},false);
    check(r.controller->execute(SlotSave{2}).saved && read_json(path/"userprofile_2.json")["lighting"].is_null(),"core-only capture preserves explicit-null peripheral intent rather than replacing it");
    r.session->validation.enabled.erase("cpuPower");r.session->validation.enabled.erase("gpuPower");r.session->validation.enabled.erase("fanControl");
    saved=r.controller->execute(SlotSave{3});after=read_json(path/"userprofile_3.json");
    check(saved.saved && after["cpu"]==Json(CpuSettings{45,65,90,95}) && after["gpu"]["tgp"]==115 && after["fan"]["cpuCurve"]["tempUp"][0]==0 && after["fan"]["cpuCurve"]["tempUp"][1]==255 && !after["fan"]["curveEnabled"].get<bool>(),"unavailable groups capture reference placeholders, not zero power or fabricated fan readings");
    {std::ofstream bad(path/"userprofile_4.json");bad<<"{broken";}
    check(r.controller->execute(SlotSave{4}).code==1 && std::filesystem::file_size(path/"userprofile_4.json")==7,"unreadable existing target refuses core-only overwrite instead of discarding unknown peripheral intent");
}
void monitor(const std::filesystem::path& root) {
    Rig r(root/"monitor");auto& c=*r.controller;auto s=r.state;auto p=profile();p.fan->linked=true;c.execute(FanApply{*p.fan});
    ProtectionSettings protection;protection.enabled=true;protection.threshold=95;c.execute(ProtectionApply{protection});s->reads.clear();c.tick();
    check(s->reads[0x44F]==1 && s->reads[0x43E]==1 && s->reads[0x464]==1 && s->reads.size()==6,"hidden tick shares each required temperature once plus four RPM bytes");
    c.execute(Visibility{true});s->reads.clear();c.tick();int count=0;for(auto [a,n]:s->reads)count+=n;
    check(count==13 && s->reads[0x44F]==1 && s->reads[0x43E]==1 && !s->reads.contains(0x60C),"visible Power tab tick reads the monitor bar, not System Info sensors");
    c.execute(UiPage{6});s->reads.clear();c.tick();count=0;for(auto [a,n]:s->reads)count+=n;
    check(count==18 && s->reads[0x44F]==1 && s->reads[0x43E]==1,"System Info tick reuses shared samples for eighteen EC bytes");c.execute(UiPage{0});
    s->fail_read=0x464;s->bytes[0x44F]=110;for(int i=0;i<5;++i)c.tick();
    check(c.thermal().tripped(),"unrelated RPM/telemetry failures cannot suppress safety dispatch");
    Rig retry(root/"monitor-restart");auto t=GetTickCount64();auto rs=retry.state;rs->fail_read=0x464;
    for(int i=0;i<5;++i) retry.controller->tick(t);
    rs->fail_read.reset();rs->reads.clear();retry.controller->tick(t+59999);
    check(retry.controller->monitor_stopped() && !rs->reads.contains(0x464),"telemetry stop does not continue RPM polling before retry deadline");
    auto resumed=retry.controller->tick(t+60000);
    check(resumed.has_value() && !retry.controller->visible() && !retry.controller->monitor_stopped(),"hidden RPM monitoring restarts at sixty seconds without a visible window");
    rs->fail_read=0x464;for(int i=0;i<5;++i) retry.controller->tick(t+60001);rs->fail_read.reset();rs->reads.clear();retry.controller->execute(Visibility{true});
    check(retry.controller->tick(t+60002).has_value() && rs->reads.contains(0x464),"Show immediately restarts stopped monitoring without waiting out the old retry deadline");
}
void x6fr(const std::filesystem::path& root) {
    auto path=root/"x6fr";std::filesystem::create_directories(path);auto s=std::make_shared<State>();s->amd=true;s->bytes[0x463]=95;
    s->bytes[0x7C6]=0xC4;s->bytes[0x7A7]=145;s->bytes[0x7A8]=145;s->bytes[0x7A9]=80;
    EcSession session([&](const std::wstring&){return std::make_unique<Fake>(s);},[](const std::string&){},false,[](unsigned){},profiles_directory(),Identity{"X6FR5xxY","fixture","amd",uint8_t{26}});
    check(session.profile.project_id=="X6FR5xxY" && session.profile.modes && !session.profile.modes->contains("batterySaver"),"X6FR reviewed 145 W preset is not exposed as Battery Saver");
    Controller c(session,path,[](const std::string&){},[](unsigned){},{},false,{},false);c.start();s->writes.clear();
    auto f=*profile().fan;check(c.execute(FanApply{f}).code==0 && s->bytes[0x7C6]==0xC4,"X6FR controller preserves shared latch bits around real fan batch");
    c.execute(SlotSave{1});check(read_json(path/"userprofile_1.json")["fan"]["curveEnabled"]==true,"X6FR capture readback agrees with shared-bit write semantics");
}
void worker(const std::filesystem::path& root) {
    auto path=root/"worker";std::filesystem::create_directories(path);auto state=std::make_shared<State>();
    auto delivery=std::make_shared<Delivery>();Log log(path/"worker.log");WorkerOptions options;options.nvidia_hardware=false;options.peripheral_hardware=false;
    options.identity=Identity{"X6AR55xY","fixture","intel",uint8_t{26}};options.state_directory=path;options.wait=[](unsigned){};
    options.channel_factory=[state](const std::wstring&,HANDLE){return std::make_unique<Fake>(state);};
    ControlWorker worker(delivery,log,std::move(options));
    auto ui=worker.request(UiRead{}).get();
    check(ui.code==0 && ui.peripherals && ui.peripherals->at("ui").at("fan").contains("cpuCurve"),"UI receives an owned editor snapshot through the real worker queue");
    size_t write_count;{std::lock_guard lock(state->mutex);write_count=state->writes.size();state->fail_read=0x783;}
    auto independent=worker.request(UiRead{0}).get().peripherals->at("ui");
    {std::lock_guard lock(state->mutex);state->fail_read.reset();check(state->writes.size()==write_count,"UI Read does not apply editor intent or write hardware");}
    check(independent.contains("cpuError") && independent.contains("gpu") && !independent["gpu"].is_null(),"failed UI CPU read preserves independent successful GPU values");
    auto editor=ui.peripherals->at("ui").at("fan").get<FanSettings>();editor.cpu_curve->duty[1]+=2;
    check(worker.request(ProtectionApply{{false,65,false,false,true}}).get().code==0,"applied protection is explicitly configured before profile capture");
    SlotSave save{5};save.editor_fan=editor;
    auto saved=worker.request(save).get();auto saved_profile=read_json(path/"userprofile_5.json");
    auto after_save=worker.request(UiRead{}).get().peripherals->at("ui");
    {std::lock_guard lock(state->mutex);check(state->writes.size()==write_count,"saving pending UI editor settings does not apply them to hardware");}
    check(saved.saved && saved.slot_committed && saved_profile["fan"]["cpuCurve"]["duty"][1]==22 && saved_profile["gpuFailsafe"]["thresholdC"]==65 && after_save["protection"]["thresholdC"]==65,"original Save captures pending fan curves but applied thermal settings, without changing either live group");
    {std::lock_guard lock(state->mutex);state->reads.clear();}
    worker.request(UiRead{-1}).get();worker.request(UiRead{2}).get();
    {std::lock_guard lock(state->mutex);check(state->reads.empty(),"catalog and thermal presentation do not reread unrelated hardware");}
#ifdef MECHCONTROL_WITH_CLI
    std::wstring pipe_name=L"MechControl.Native.ControllerFixture."+std::to_wstring(GetCurrentProcessId());
    auto server=std::make_unique<CliServer>(worker,log,pipe_name);
    auto reply=cli::exchange({"status","--json"},pipe_name);
    check(reply.code==0 && parse(reply.text)["model"]=="IDY_7Y","real native server routes framed status through controller worker");
    reply=cli::exchange({"cpu","--pl1","81","--json"},pipe_name);
    check(reply.code==0 && parse(reply.text)["pl1"]["value"]==81,"real pipe write returns actual controller readback");
    {
        auto name=L"\\\\.\\pipe\\"+pipe_name;Handle pipe;
        for(int i=0;i<100 && !pipe;++i) {pipe=Handle(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));if(!pipe)Sleep(10);}
        check(static_cast<bool>(pipe),"fixture pipe accepts next connection");
        auto frame=cli::request({"status","--json"});frame[0]=2;
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        for(auto& byte:frame) cli::transfer(pipe.get(),std::span(&byte,1),true,deadline);
        std::array<uint8_t,8> prefix;cli::transfer(pipe.get(),prefix,false,deadline);
        std::vector<uint8_t> response(cli::integer(prefix.data()+4));cli::transfer(pipe.get(),response,false,deadline);
        check(cli::integer(prefix.data())==2,"fragmented wrong-version request is consumed and rejected by production server");
    }
#endif
    int gpu_reads;{std::lock_guard lock(state->mutex);gpu_reads=state->reads[0x744];}
    std::vector<std::future<ControlResult>> futures;
    for(int i=0;i<12;++i) futures.push_back(worker.request(CpuApply{{80+i,150,200,95}}));
    for(auto& f:futures) check(f.wait_for(std::chrono::seconds(5))==std::future_status::ready && f.get().code==0,"simultaneous commands serialize through single worker owner");
    {std::lock_guard lock(state->mutex);check(state->bytes[0x783]==91,"FIFO command results reflect final complete CPU group");check(state->reads[0x744]==gpu_reads,"hidden CPU commands do not trigger unrelated GPU presentation reads");}
    int hidden_reads;{std::lock_guard lock(state->mutex);hidden_reads=state->reads[0x464];}
    delivery->take();Sleep(1200);auto hidden=delivery->take();
    check(std::none_of(hidden.begin(),hidden.end(),[](const auto& r){return r.kind==ResultKind::sample || r.kind==ResultKind::presentation;}),"hidden real controller does not format or enqueue presentation samples");
    {std::lock_guard lock(state->mutex);check(state->reads[0x464]>hidden_reads,"hidden sampling continues for passive counters without UI sample delivery");}
    worker.request(Visibility{true}).get();Sleep(1200);auto shown=delivery->take();
    check(std::any_of(shown.begin(),shown.end(),[](const auto& r){return r.kind==ResultKind::sample && r.sensors.has_value();}),"Show restores owned visible sample delivery");
    worker.request(Visibility{false}).get();delivery->take();
    // Not draining delivery simulates a slow/hidden UI; the monitor keeps working.
    auto future=worker.request(ProtectionApply{{true,50,false,false,false}});future.get();
    {std::lock_guard lock(state->mutex);state->bytes[0x44F]=70;}
    Sleep(3200);auto status=worker.request(CliCommand{{"status","--json"}}).get();
    check(parse(status.text)["failsafe"]["tripped"]==true,"thermal recovery is independent of blocked/undrained UI delivery");
    auto notices=delivery->take();check(std::any_of(notices.begin(),notices.end(),[](const auto& r){return r.kind==ResultKind::command && r.text.find(L"TRIPPED")!=std::wstring::npos;}),"hidden presentation suppression retains significant protection transitions");
#ifdef MECHCONTROL_WITH_CLI
    server.reset();
#endif
    delivery->close();worker.stop();check(worker.request(ThermalReset{}).get().code==1,"shutdown rejects subsequent typed control commands");
}
void worker_restart(const std::filesystem::path& root) {
    auto path=root/"worker-gpu-restart";std::filesystem::create_directories(path);auto state=std::make_shared<State>();
    auto delivery=std::make_shared<Delivery>();Log log(path/"worker.log");WorkerOptions options;options.nvidia_hardware=false;options.peripheral_hardware=false;
    options.identity=Identity{"X6AR55xY","fixture","intel",uint8_t{26}};options.state_directory=path;options.wait=[](unsigned){};
    options.channel_factory=[state](const std::wstring&,HANDLE){return std::make_unique<Fake>(state);};
    GpuFixture* driver=nullptr;options.nvidia_factory=[&] {auto gpu=std::make_unique<GpuFixture>();driver=gpu.get();gpu->enabled=false;gpu->restart_delay=3500;return gpu;};
    ControlWorker worker(delivery,log,std::move(options));check(worker.request(ProtectionApply{{true,50,true,true,false}}).get().code==0,"restart workflow arms shared thermal sampling");
    {std::lock_guard lock(state->mutex);state->bytes[0x44F]=80;}
    auto future=worker.request(GpuUnlock{true});Sleep(3100);auto messages=delivery->take();
    check(future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready && std::any_of(messages.begin(),messages.end(),[](auto& r){return r.text.find(L"TRIPPED")!=std::wstring::npos || r.text.find(L"PROTECTION INCOMPLETE")!=std::wstring::npos;}),"real worker timer trips and delivers thermal state before slow GPU restart completes");
    auto result=future.get();check(result.code==0 && driver->request && *driver->request<=95000,"owned GPU completion returns to serialized worker and preserves intervening hold");
    {std::lock_guard lock(state->mutex);state->bytes[0x44F]=40;}
    check(worker.request(ProtectionApply{{false,50,true,true,false}}).get().code==0 && worker.request(ThermalReset{}).get().code==0,"slow-read workflow releases prior hold deliberately");
    check(worker.request(ProtectionApply{{true,50,true,true,false}}).get().code==0,"slow-read workflow rearms protection");
    driver->read_delay=3500;
    {std::lock_guard lock(state->mutex);state->bytes[0x44F]=80;}
    auto reading=worker.request(CliCommand{{"status","--json"}});Sleep(100);
    auto queued=worker.request(CpuApply{{100,150,200,95}});
    auto visibility=worker.request(Visibility{true});check(visibility.wait_for(std::chrono::milliseconds(500))==std::future_status::ready && visibility.get().code==0,"lifecycle notifications pass an exclusive slow NVIDIA read");
    Sleep(3000);messages=delivery->take();
    check(reading.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready && std::any_of(messages.begin(),messages.end(),[](auto& r){return r.text.find(L"TRIPPED")!=std::wstring::npos || r.text.find(L"PROTECTION INCOMPLETE")!=std::wstring::npos;}),"actual worker keeps safety sampling ahead of completion of an ordinary slow NVIDIA read");
    check(reading.get().code==0 && driver->request && *driver->request<=95000,"slow-read completion returns to the same owner and enforces the intervening hold");
    check(queued.get().code==1,"ordinary queued feature work resumes after NVIDIA completion and respects the new hold");
    delivery->close();worker.stop();
}
void worker_hid_delay(const std::filesystem::path& root) {
    auto path=root/"worker-hid-delay";std::filesystem::create_directories(path);auto state=std::make_shared<State>();
    auto delivery=std::make_shared<Delivery>();Log log(path/"worker.log");WorkerOptions options;options.nvidia_hardware=false;options.peripheral_hardware=false;
    options.identity=Identity{"X6AR55xY","fixture","intel",uint8_t{26}};options.state_directory=path;options.wait=[](unsigned){};
    options.channel_factory=[state](const std::wstring&,HANDLE){return std::make_unique<Fake>(state);};
    std::atomic<bool> delay=true;options.lighting_send=[&](int,const HidReport&){if(delay.exchange(false)) Sleep(3500);};
    ControlWorker worker(delivery,log,std::move(options));
    worker.request(LightingIdle{0}).get();worker.request(ProtectionApply{{true,50,false,true,false}}).get();
    {std::lock_guard lock(state->mutex);state->bytes[0x44F]=80;}
    auto operation=worker.request(LightingApply{0,{true,1,2,3,50}});Sleep(100);
    auto page=worker.request(BatteryPage{true});auto visible=worker.request(Visibility{true});
    check(page.wait_for(std::chrono::milliseconds(500))==std::future_status::ready && visible.wait_for(std::chrono::milliseconds(500))==std::future_status::ready,"page and lifecycle delivery remain responsive during delayed HID");
    Sleep(3100);auto messages=delivery->take();
    check(operation.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready && std::any_of(messages.begin(),messages.end(),[](const auto& r){return r.text.find(L"TRIPPED")!=std::wstring::npos;}),"shared EC safety sampling trips before delayed HID completion");
    check(std::any_of(messages.begin(),messages.end(),[](const auto& r){return r.kind==ResultKind::battery && r.control && r.control->peripherals->contains("battery");}),"visible-page battery values reach the real worker delivery mailbox during delayed HID");
    check(operation.get().code==0,"delayed HID completion rejoins controller without moving EC transport ownership");delivery->close();worker.stop();
}
}
int main(int argc,char** argv) {
    try {
        if(argc==3 && std::string_view(argv[1])=="--nvidia-same-values") {
            Handle mutex(CreateMutexW(nullptr,TRUE,L"Global\\MechControl"));
            if(!mutex || GetLastError()==ERROR_ALREADY_EXISTS) throw std::runtime_error("A controller already owns Global\\MechControl");
            std::filesystem::path directory=argv[2];std::filesystem::create_directories(directory);
            bool active=false;
            {nv::Api api;auto d=api.dlppc();if(!d) throw std::runtime_error("No DLPPC policy for same-value check");active=d->active;
             nv::Store store(api.identity);auto image=api.bios();bool staged=false,discarded=false;auto original=store.open(image,staged,discarded);auto c=api.currents();
             auto rail=[](nv::Rail r,nv::Field f,uint32_t percent=140){r.def=f.def;r.stock_max=f.max;r.unlock_percent=percent;return nv::RailOverride{r.current,static_cast<uint32_t>(r.allowed(true))};};
             if(staged) {nv::Override same; same.power=original.power.max+75000;same.nv=rail(c.nv,original.nv);if(c.ms && original.ms) same.ms=rail(*c.ms,*original.ms);if(c.input && original.input) same.input=rail(*c.input,*original.input,154);
                 check(store.matches(original.build(same)),"same-value current workflow will not change existing BIOS override records");}
             auto start=GetTickCount64();for(int i=0;i<100;++i) api.telemetry();
             PROCESS_MEMORY_COUNTERS memory{};K32GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory));
             std::cout<<"Telemetry 100 fresh reads: "<<GetTickCount64()-start<<" ms; working set "<<memory.WorkingSetSize<<"; peak "<<memory.PeakWorkingSetSize<<" bytes\n";
            }
            {auto log=[](const std::string& s){std::cout<<s<<'\n';};
             EcSession session([](const std::wstring& p){return open_ec_channel(p,nullptr);},log,true);
             nv::Power power(control_limit(session.profile,"TGP").min,log);
             std::cout<<"Verified owned override enabled: "<<power.enabled<<'\n';
             bool budget_supported=true;try {power.budget();} catch(const std::exception& ex) {budget_supported=false;std::cout<<"Budget unavailable: "<<ex.what()<<'\n';}
             if(!budget_supported && power.enabled) {auto before=power.limits();
                 bool refused=false;try {power.apply(power.stock_watts()+1);} catch(const std::exception& ex){refused=true;std::cout<<ex.what()<<'\n';}
                 auto after=power.limits();check(refused && before.requested==after.requested,"invalid budget refuses above-stock F8 raise without changing the driver request");}
            }
            auto delivery=std::make_shared<Delivery>();Log log(directory/"worker.log");WorkerOptions options;options.state_directory=directory;
            ControlWorker worker(delivery,log,std::move(options));
            auto status=worker.request(CliCommand{{"status","--json"}}).get();check(status.code==0,"integrated NVIDIA worker status succeeds");auto before=parse(status.text);write_json(directory/"status-before.json",before,false);
            auto& gpu=before["gpu"];check(gpu["nvidia"].is_null() && !gpu["nvvdd"].is_null(),"F7 failure does not discard known override state, telemetry or rail policies");
            std::vector<std::string> args={"gpu","--nvvdd",std::to_string(gpu["nvvdd"]["limitMa"].get<int>())+"mA","--json"};
            if(!gpu["msvdd"].is_null()) {args.insert(args.end(),{"--msvdd",std::to_string(gpu["msvdd"]["limitMa"].get<int>())+"mA"});}
            if(!gpu["boardInput"].is_null()) {args.insert(args.end(),{"--board-input",std::to_string(gpu["boardInput"]["limitMa"].get<int>())+"mA"});}
            auto currents=worker.request(CliCommand{args}).get();check(currents.code==0,"integrated same-value current command succeeds through the real owner");write_json(directory/"currents.json",parse(currents.text),false);
            auto dlp=worker.request(DlppcApply{!active}).get();check(dlp.code==0 && dlp.saved,"integrated same-value DLPPC applies and saves through the same worker");
            auto after=worker.request(CliCommand{{"status","--json"}}).get();check(after.code==0,"integrated readback after same-value operations succeeds");write_json(directory/"status-after.json",parse(after.text),false);
            worker.stop();delivery->close();
            {nv::Api api;check(api.dlppc()->active==active,"physical DLPPC readback agrees after worker shutdown");}
            return 0;
        }
        if(argc==3 && std::string_view(argv[1])=="--capture-physical") {
            Handle mutex(CreateMutexW(nullptr,TRUE,L"Global\\MechControl"));
            if(!mutex || GetLastError()==ERROR_ALREADY_EXISTS) throw std::runtime_error("A controller already owns Global\\MechControl");
            std::filesystem::path directory=argv[2];std::filesystem::create_directories(directory);
            auto log=[](const std::string& s){std::cout<<s<<'\n';};
            EcSession session([](const std::wstring& p){return open_ec_channel(p,nullptr);},log,false);
            Controller controller(session,directory,log);controller.start();
            auto result=controller.execute(SlotSave{1});
            check(result.code==0 && result.saved,"physical current-state capture saves a source-compatible slot");
            auto status=controller.execute(CliCommand{{"status","--json"}});check(status.code==0,"physical controller status succeeds");
            write_json(directory/"capture-status.json",parse(status.text),false);return 0;
        }
        if(argc!=1) throw std::runtime_error("Unexpected check arguments");
        auto base=std::filesystem::path("C:/Temp/tandem/mechcontrol-native-phase4/check");
        std::filesystem::create_directories(base);
        auto stem=std::to_string(GetCurrentProcessId());auto root=base/stem;
        for(unsigned suffix=1;!std::filesystem::create_directory(root);++suffix) root=base/(stem+"-"+std::to_string(suffix));
        integration(root);startup_degradation(root);gpu_integration(root);thermal(root);recovery(root);defaults_and_no_slot(root);startup_and_eligibility(root);capture_preservation(root);peripheral_intent(root);peripheral_regressions(root);monitor(root);x6fr(root);worker(root);worker_restart(root);worker_hid_delay(root);
        check(!GetModuleHandleW(L"nvapi64.dll") && !GetModuleHandleW(L"nvml.dll"),"injected/stock controller status and capture do not load physical NVIDIA libraries");
        std::cout<<"Evidence: "<<root.string()<<'\n';return 0;
    } catch(const std::exception& ex) {std::cerr<<"FAIL "<<ex.what()<<'\n';return 1;}
}
