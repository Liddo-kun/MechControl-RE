#include "controller.h"
#include "config_text.h"
#include <algorithm>
#include <cmath>
#include <thread>

namespace mech {
using namespace config;
static constexpr int fixed_temps[]{19,29,39,49,57,65,73,81,97,101};
static int ramp_start(int t) {return static_cast<int>(std::upper_bound(std::begin(fixed_temps),std::end(fixed_temps),t+4)-std::begin(fixed_temps));}
static int round_even(float n) { // C# Math.Round(float promoted to double), midpoint-to-even.
    int v=static_cast<int>(std::floor(n));float f=n-v;return v+(f>0.5f || (f==0.5f && (v&1)));
}
Curve active_curve(const Curve& input,int entries) {
    Curve c;int previous=-1;
    for(size_t i=0;i<input.temp_up.size();++i) {
        int t=input.temp_up[i];if(t==0 || t>=254 || t<=previous) break;
        if(i>=input.duty.size()) throw std::runtime_error("Fan duty array is incomplete");
        c.temp_up.push_back(t);c.duty.push_back(i?input.duty[i]/2*2:0);previous=t;
    }
    if(c.temp_up.size()>static_cast<size_t>(std::max(0,entries))) throw std::runtime_error("Fan curve has more active points than the model table");
    if(c.temp_up.empty()) throw std::runtime_error("Fan curve has no active points");
    for(size_t i=0;i<c.temp_up.size();++i) {
        if(c.duty[i]<0 || c.duty[i]>200) throw std::runtime_error("Fan duty outside 0-200");
        if(i && (c.temp_up[i]-c.temp_up[i-1]<4 || c.duty[i]<c.duty[i-1]))
            throw std::runtime_error("Fan points require 4 C spacing and monotonic duty");
        c.temp_down.push_back(i?std::max(0,c.temp_up[i-1]-4):0);
    }
    return c;
}
Curve saved_curve(const Curve& input,const FanTable& ft) {
    auto c=active_curve(input,ft.entries);auto last=c.duty.back();
    while(c.temp_up.size()<static_cast<size_t>(ft.entries)) {c.temp_up.push_back(ft.padding_temp);c.temp_down.push_back(ft.padding_temp);c.duty.push_back(last);}
    return c;
}
int curve_duty(const Curve& c,int t) {
    if(t<=c.temp_up.front()) return c.duty.front()/2;
    for(size_t i=1;i<c.temp_up.size();++i) if(t<=c.temp_up[i])
        return round_even(c.duty[i-1]/2+static_cast<float>(t-c.temp_up[i-1])/(c.temp_up[i]-c.temp_up[i-1])*(c.duty[i]/2-c.duty[i-1]/2));
    return c.duty.back()/2;
}
Curve linked_curve(int duty,int ramp) {
    Curve c;duty=std::clamp(duty,1,100);int count=10-ramp;
    for(int i=0;i<10;++i) {
        c.temp_up.push_back(fixed_temps[i]);c.temp_down.push_back(i?fixed_temps[i-1]-4:0);
        int value=i==0?0:i==9?100:i<ramp || count<=0?duty:round_even(duty+static_cast<float>(i-ramp)/(count-1)*(100-duty));
        c.duty.push_back(value*2);
    }
    return c;
}
Controller::Controller(EcSession& session,std::filesystem::path directory,std::function<void(const std::string&)> diagnostic,
    std::function<void(unsigned)> wait,std::function<void(std::function<void()>)> run_blocking,bool nvidia_hardware,std::unique_ptr<nv::Power> driver,bool peripheral_hardware,
    std::function<void(Json)> cooler_delivery,Lighting::Send lighting_send):session_(session),
    store_(std::move(directory),gpu_recovery_supported(session.profile,session.hardware.at(0x7D2)),diagnostic),
    diagnostic_(std::move(diagnostic)),wait_(std::move(wait)),run_blocking_(std::move(run_blocking)),cooler_delivery_(std::move(cooler_delivery)),lighting_send_(std::move(lighting_send)),peripheral_hardware_(peripheral_hardware) {
    if(!wait_) wait_=[](unsigned ms){std::this_thread::sleep_for(std::chrono::milliseconds(ms));};
    if(!run_blocking_) run_blocking_=[](std::function<void()> work){work();};
    nvidia_available_=nvidia_hardware && enabled("gpuPower") && nv::Store::present();
    nvidia_=std::move(driver);if(nvidia_) {nvidia_available_=true;gpu_unlocked_=nvidia_->enabled;nvidia_min_watts_=nvidia_->minimum_watts();}
    SYSTEM_POWER_STATUS power{};on_ac_=GetSystemPowerStatus(&power) && power.ACLineStatus==1;
    const auto& p=session_.profile;
    // Model-qualified correction based on XxFR5xxx_Pro_14.810's shared mode bits.
    // Other profiles retain the reference whole-byte contract, not inferred Intel bit semantics.
    if(equal_ignore_case(p.project_id,"X6FR5xxY")) latch_mask_=4;
    try {
        if(enabled("cpuPower") && !p.cpu_fallback && p.limits.contains("PL1") && p.limits.contains("PL2") && p.limits.contains("PL4")) {
            auto m=minimum_cpu();auto w=cpu_writes(p,m.pl1,m.pl2,m.pl4,m.temp_limit);
            cpu_minimum_=m.pl1>0 && m.pl2>0 && m.pl4>0 && encoding::decode_pl4(encoding::pl4(m.pl4,p.pl4_doubled()),p.pl4_doubled())==m.pl4;
            for(auto x:w) session_.guard->admit(x.address);
            for(size_t i=0;i<3;++i) if(session_.transport().read(w[i].address)==255) cpu_minimum_=false;
        }
    } catch(const std::exception& ex) {diagnostic_(std::string("CPU emergency capability unavailable: ")+ex.what());}
    try {
        if(enabled("gpuPower") && gpu_recovery_supported(p,session.hardware.at(0x7D2)) && p.limits.contains("TGP") && p.limits.at("TGP").offset) {
            auto g=gpu();int minimum=control_limit(p,"TGP").min;
            auto w=gpu_writes(p,minimum,false,0);
            gpu_minimum_=minimum>0 && g.tgp>=p.tgp_offset() && g.tgp<=control_limit(p,"TGP").max+75 && g.dynamic_boost_watts<=control_limit(p,"dynamicBoost").max && read("tgpDbCtrl")!=255;
            for(auto x:w) session_.guard->admit(x.address);
        }
    } catch(const std::exception& ex) {diagnostic_(std::string("GPU emergency capability unavailable: ")+ex.what());}
    try {session_.guard->admit(p.reg("fanControlMode"));fan_boost_=enabled("fanControl") && read("fanControlMode")!=255;} catch(...) {}
    int supported=(cpu_minimum_?Cpu:0)|(gpu_minimum_?Gpu:0)|(fan_boost_?Fan:0);
    thermal_=std::make_unique<ThermalProtection>(store_,supported,[this](int a){emergency(a);},[this](int a){return matches(a);},
        [this]{return temperature("gpuTemp");},diagnostic_);
    session_.guard->recover=[this]{return recover();};
}
Controller::~Controller() {session_.guard->recover={};} // Leave linked crash-safe tails and all EC state unchanged.
bool Controller::enabled(const char* feature) const {return session_.validation.enabled.contains(feature);}
bool Controller::curves_supported() const {return enabled("fanControl") && session_.profile.fan_table.format=="RamFan1";}
void Controller::seed_fans() {
    if(!curves_supported()) return;
    const auto& p=session_.profile;const auto& s=session_.fans;
    fan_intent_.curve=s.custom_mode(latch_mask_);
    try {
        if(s.valid()) {
            fan_intent_.cpu_curve=active_curve(s.cpu,p.fan_table.entries);
            if(p.fan_table.has_gpu) fan_intent_.gpu_curve=active_curve(s.gpu,p.fan_table.entries);
            return;
        }
        if(p.default_fan && p.default_fan->cpu && p.default_fan->gpu) {
            fan_intent_.cpu_curve=active_curve(*p.default_fan->cpu,p.fan_table.entries);
            fan_intent_.gpu_curve=active_curve(*p.default_fan->gpu,p.fan_table.entries);
            diagnostic_("Fan seed: model defaults (EC snapshot invalid)");return;
        }
    } catch(const std::exception& ex) {diagnostic_(std::string("Fan seed unavailable: ")+ex.what());}
    // Reference editable starter, not a model-qualified automatic recovery table.
    Curve generic{{30,40,50,60,65,70,75,80,85,90},{},{0,50,70,90,110,130,150,170,190,200}};
    for(size_t i=0;i<generic.temp_up.size();++i) generic.temp_down.push_back(i?generic.temp_up[i-1]-4:0);
    fan_intent_.cpu_curve=generic;fan_intent_.gpu_curve=generic;fan_review_required_=true;
    diagnostic_("Fan seed: generic editable starter; review before Apply, never used for automatic recovery");
}
uint8_t Controller::read(const char* name) {return session_.transport().read(session_.profile.reg(name));}
std::optional<int> Controller::temperature(const char* name) {
    try {return read(name);} catch(...) {return {};}
}
CpuSettings Controller::cpu() {return {read("PL1"),read("PL2"),encoding::decode_pl4(read("PL4"),session_.profile.pl4_doubled()),
    encoding::decode_cpu_temperature(read("cpuTempLimitRead"),session_.profile.cpu_encoding()=="direct",session_.profile.tj_max())};}
GpuSettings Controller::gpu(bool driver_read) {
    GpuSettings g;auto ctrl=read("tgpDbCtrl");g.tgp=read("tgp")+session_.profile.tgp_offset();
    read("dbTotalPower");g.dynamic_boost_watts=read("dbMaxTgp");g.dynamic_boost_enabled=(ctrl&2)!=0;
    // Passive reads never contact NVIDIA, even at the EC floor. Retain our
    // lower request while asleep; only report it when the EC still matches it.
    if(g.tgp==control_limit(session_.profile,"TGP").min && gpu_unlocked_ && driver_watts_) {
        int below=*driver_watts_-(g.dynamic_boost_enabled?g.dynamic_boost_watts:0);if(below<g.tgp) g.tgp=std::max(below,gpu_min());
    }
    if(driver_read && gpu_unlocked_ && g.tgp<=control_limit(session_.profile,"TGP").min) {
        nv::PowerLimits power;nvidia_work([&]{power=nvidia_->limits();});int floor=control_limit(session_.profile,"TGP").min;
        int below=power.requested?int(*power.requested/1000)-(g.dynamic_boost_enabled?g.dynamic_boost_watts:0):floor;g.tgp=std::clamp(below,gpu_min(),floor);
    }
    return g;
}
CpuSettings Controller::minimum_cpu() {
    auto& p=session_.profile;auto t=read("cpuTempLimitRead");
    int limit=encoding::decode_cpu_temperature(t,p.cpu_encoding()=="direct",p.tj_max());auto bounds=control_limit(p,"cpuTempLimit");
    if(t==255 || limit<bounds.min || limit>bounds.max) throw std::runtime_error("Current CPU temperature limit invalid; no fallback replacement");
    return {control_limit(p,"PL1").min,control_limit(p,"PL2").min,control_limit(p,"PL4").min,limit};
}
void Controller::batch(const std::vector<EcWrite>& writes) {
    for(auto w:writes) session_.guard->admit(w.address);
    int epoch=epoch_;writes_started_=true;
    session_.guard->apply(writes);
    if(epoch!=epoch_) throw std::runtime_error("EC reactivated during application; complete group must be reapplied");
}
void Controller::peripheral_io(const std::function<void()>& work) {
    peripheral_busy_=true;
    try {run_blocking_(work);} catch(...) {peripheral_busy_=false;throw;}
    peripheral_busy_=false;
}
void Controller::dimming(bool value) {
    if(!peripheral_hardware_) throw std::runtime_error("Local dimming hardware unavailable");
    int epoch=epoch_;writes_started_=true;
    peripheral_io([&]{set_local_dimming(value);});
    if(epoch!=epoch_) throw std::runtime_error("EC reactivated during local dimming; reapply required");
    if(((session_.transport().read(0x72B)&2)!=0)!=value) throw std::runtime_error("Local dimming EC readback mismatch");
    dimming_available_=true;
}
void Controller::emergency(int action) {
    auto& p=session_.profile;
    if(action==Gpu) {if(!gpu_minimum_) throw std::runtime_error("Model-specific GPU minimum unavailable");GpuSettings g;g.tgp=control_limit(p,"TGP").min;apply_gpu(g);}
    if(action==Cpu) {if(!cpu_minimum_) throw std::runtime_error("Model-specific CPU minimum unavailable");auto c=minimum_cpu();batch(cpu_writes(p,c.pl1,c.pl2,c.pl4,c.temp_limit));}
    if(action==Fan) {if(!fan_boost_) throw std::runtime_error("Fan boost unavailable");batch({fan_boost_write(p,true)});}
}
bool Controller::matches(int action) {
    if(action==Gpu && nvidia_busy_) return false;
    auto& p=session_.profile;std::vector<EcWrite> w;
    if(action==Gpu) {if(!gpu_minimum_) return false;w=gpu_writes(p,control_limit(p,"TGP").min,false,0);}
    if(action==Cpu) {if(!cpu_minimum_) return false;auto c=minimum_cpu();w=cpu_writes(p,c.pl1,c.pl2,c.pl4,c.temp_limit);w.pop_back();}
    if(action==Fan) {if(!fan_boost_) return false;w={fan_boost_write(p,true)};}
    for(auto x:w) if(!session_.guard->verifies_same_address(x.address) || (session_.transport().read(x.address)&x.mask)!=(x.value&x.mask)) return false;
    if(action==Gpu && (gpu_unlocked_ || gpu_driver_uncertain_)) {bool minimum=false;nvidia_work([&]{minimum=nvidia_->at_most(control_limit(p,"TGP").min);});return minimum;}
    return true;
}
void Controller::initialize() {
    // Each reference init step runs even after an independent step fails.
    const std::vector<std::vector<EcWrite>> steps={{{0x709,5},{0x708,1}},{{0x741,5}},{{0x748,129}},
        {{0x726,128,128},{0x727,64,64}},{{0x7C5,96,96}},{{0x7C7,12}},{{0x706,65}}};
    for(const auto& step:steps) {try {session_.guard->apply(step);} catch(const std::exception& ex) {diagnostic_(std::string("EC init step failed: ")+ex.what());} wait_(150);}
    for(unsigned i=0;i<=30;++i) {
        if((session_.transport().read(0x741)&5)==1) return;
        if(i!=30) wait_(100);
    }
    throw std::runtime_error("EC activation not acknowledged (bit 0 set, bit 2 cleared required)");
}
bool Controller::recover() {
    if(recovering_ || (session_.transport().read(0x741)&1)) return false;
    recovering_=true;++epoch_;thermal_->invalidate();last_duty_=-1;fan_application_=FanApplication::unapplied;
    try {initialize();recovering_=false;reapply_=true;watchdog_=GetTickCount64()+30000;return true;}
    catch(...) {recovering_=false;throw;}
}
void Controller::start() {
    recovering_=true;
    try {initialize();} catch(...) {recovering_=false;throw;}
    recovering_=false;watchdog_=GetTickCount64()+30000;
    session_.snapshot_fans();seed_fans();
    diagnostic_("Post-activation fan snapshot: active="+std::to_string(session_.fans.active_entries())+"; valid="+std::to_string(session_.fans.valid())+"; GPU="+std::to_string(session_.profile.fan_table.has_gpu));
    thermal_->enforce(); // Persisted obligations outrank optional NVIDIA startup work.
    lighting_=std::make_unique<Lighting>(session_,store_,diagnostic_,peripheral_hardware_,lighting_send_,
        [this](auto work){peripheral_io(work);},[this](const auto& writes){batch(writes);});
    if(peripheral_hardware_) {
        peripheral_io([&]{dimming_available_=local_dimming_available();});
        diagnostic_(std::string("Local dimming WMI ")+(dimming_available_?"available":"unavailable"));
        if(session_.hardware.liquid_cooling()) cooler_=std::make_unique<Cooler>(store_,diagnostic_,Cooler::Resolver{},cooler_delivery_);
    }
    if(nvidia_available_ && (nv::Store::saved() || store_.settings().disable_dlppc)) {
        try {open_nvidia();if(store_.settings().disable_dlppc) {nvidia_work([&]{nvidia_->set_dlppc(false);});dlppc_disabled_=true;}}
        catch(const std::exception& ex) {diagnostic_(std::string("Saved NVIDIA state not applied: ")+ex.what());}
    }
    int slot=store_.settings().last_slot;std::optional<UserProfile> startup;nv::ProfilePlan gpu_plan_value;
    if(slot) try {startup=store_.load(slot);if(startup) gpu_plan_value=plan(*startup,true);}
    catch(const std::exception& ex) {startup.reset();diagnostic_(std::string("Startup profile preflight refused: ")+ex.what());}
    thermal_->enforce(); // Recovered obligations precede all automatic normal settings.
    if(startup) try {apply_slot(*startup,gpu_plan_value,slot,false,false);} catch(const std::exception& ex) {diagnostic_(std::string("Startup profile not applied: ")+ex.what());}
    if(!slot && enabled("fanControl") && !session_.fans.valid()) {
        batch({{session_.profile.reg("fanCurveEnable"),0,128}});fan_intent_.curve=false;
        diagnostic_("EC fan table invalid at startup; custom mode disabled without applying seed curves");
    }
    if(cooler_) cooler_->start(); // Selected-slot intent is installed before auto-connect can write it.
}
void Controller::explicit_apply(const std::function<void()>& apply) {
    thermal_->check_before_apply();
    if(thermal_->tripped()) throw std::runtime_error("Thermal failsafe is tripped; Reset or load a profile first");
    try {apply();} catch(...) {thermal_->enforce();throw;}
}
nv::ProfilePlan Controller::plan(const UserProfile& profile,bool automatic) {
    profile.preflight(session_.profile.project_id);thermal_->validate(*profile.protection);
    const auto& p=session_.profile;
    std::vector<EcWrite> writes;
    nv::ProfilePlan gpu_value;
    auto append=[&](std::vector<EcWrite> w){writes.insert(writes.end(),w.begin(),w.end());};
    if(enabled("cpuPower")) {auto c=*profile.cpu;append(cpu_writes(p,c.pl1,c.pl2,c.pl4,c.temp_limit));}
    if(enabled("gpuPower")) {
        gpu_value=plan_gpu(*profile.gpu,automatic);auto g=gpu_value.settings;
        auto l=control_limit(p,"TGP");append(gpu_writes(p,std::max(g.tgp,l.min),g.dynamic_boost_enabled,g.dynamic_boost_watts,gpu_value.unlock && gpu_extend()?gpu_unlock_max():l.max));
    }
    if(enabled("fanControl")) {
        if(curves_supported()) {
            auto c=active_curve(*profile.fan->cpu_curve,p.fan_table.entries);std::optional<Curve> g;
            if(p.fan_table.has_gpu) g=active_curve(*profile.fan->gpu_curve,p.fan_table.entries);
            append(fan_curve_writes(p,c,g,profile.fan->curve,latch_mask_));
        }
        append({fan_boost_write(p,profile.fan->boost)});
        append({{p.reg("fanSwitchSpeed"),encoding::switch_speed(profile.fan->switch_enabled,profile.fan->switch_ms)}});
    }
    if(enabled("battery")) {
        if(profile.battery->mode<0 || profile.battery->mode>2) throw std::runtime_error("Battery mode must be 0-2");
        append({{p.reg("batteryMode"),encoding::battery(profile.battery->mode,session_.battery_bit3),p.bitfields.at("batteryMode").get_mask()}});
    }
    for(auto w:writes) session_.guard->admit(w.address);
    return gpu_value;
}
void Controller::apply_profile(const UserProfile& profile,const nv::ProfilePlan& gpu_value) {
    auto& p=session_.profile;
    if(enabled("gpuPower")) configure_gpu(gpu_value); // Driver ceilings/currents precede other feature writes.
    if(curves_supported()) fans(*profile.fan);
    if(enabled("cpuPower")) {
        auto c=*profile.cpu;
        if(thermal_->holds(Cpu)) {if(!thermal_->verified(Cpu) || !matches(Cpu)) emergency(Cpu);}
        else batch(cpu_writes(p,c.pl1,c.pl2,c.pl4,c.temp_limit));
    }
    if(enabled("gpuPower")) {
        auto g=gpu_value.settings;
        if(thermal_->holds(Gpu)) {if(!thermal_->verified(Gpu) || !matches(Gpu)) emergency(Gpu);}
        else apply_gpu(g);
    }
    if(enabled("fanControl")) {boost(profile.fan->boost);speed(profile.fan->switch_enabled,profile.fan->switch_ms);}
    if(enabled("battery")) battery(profile.battery->mode);
    // Peripheral failures are independent and log-only, as in the source.
    if(lighting_) lighting_->apply(*profile.lighting);
    if(peripheral_hardware_) try {dimming(profile.lighting->local_dimming);}
        catch(const std::exception& e){diagnostic_(std::string("Slot local dimming failed: ")+e.what());}
    if(cooler_ && profile.cooler) cooler_->request({CoolerAction::apply,0,*profile.cooler,false,false});
}
void Controller::load_slot(int slot,bool deliberate,bool preserve) {
    auto profile=store_.load(slot);if(!profile) throw std::runtime_error("Slot is empty");
    auto gpu_value=plan(*profile,!deliberate); // Complete enabled plans before release, curves, or unrelated writes.
    apply_slot(*profile,gpu_value,slot,deliberate,preserve);
}
void Controller::apply_slot(const UserProfile& profile,const nv::ProfilePlan& gpu_value,int slot,bool deliberate,bool preserve) {
    if(deliberate) thermal_->release(); // A failed durable release cannot authorize any normal write.
    if(!preserve) thermal_->configure(*profile.protection);
    active_slot_=0;saved_profile_.reset();
    thermal_->check_before_apply();
    if(deliberate && thermal_->tripped()) throw std::runtime_error("Failsafe tripped before profile apply");
    try {apply_profile(profile,gpu_value);} catch(...) {thermal_->enforce();throw;}
    application_completed_=true;active_slot_=slot;if(!profile.requires_save) saved_profile_=profile;
    if(deliberate) store_.select(slot); // Hardware can be applied even when this save fails.
    diagnostic_("Slot "+std::to_string(slot)+" applied; peripheral failures are logged independently; BLE completion is asynchronous");
}
void Controller::boost(bool value) {
    if(!enabled("fanControl")) throw std::runtime_error("Fan control unavailable");
    if(thermal_->holds(Fan)) {
        value=true;
        if(thermal_->verified(Fan) && matches(Fan)) return;
    }
    batch({fan_boost_write(session_.profile,value)});
}
void Controller::battery(int mode) {
    if(!enabled("battery")) throw std::runtime_error("Battery control unavailable");
    if(mode<0 || mode>2) throw std::runtime_error("Battery mode must be 0-2");
    auto& p=session_.profile;batch({{p.reg("batteryMode"),encoding::battery(mode,session_.battery_bit3),p.bitfields.at("batteryMode").get_mask()}});
}
void Controller::speed(bool enabled_value,int ms) {
    if(!enabled("fanControl")) throw std::runtime_error("Fan control unavailable");
    batch({{session_.profile.reg("fanSwitchSpeed"),encoding::switch_speed(enabled_value,ms)}});
}
void Controller::fans(const FanSettings& f) {
    auto& p=session_.profile;
    if(!enabled("fanControl")) throw std::runtime_error("Fan control unavailable");
    if(!curves_supported()) throw std::runtime_error("Fan curve format unsupported");
    if(!f.cpu_curve || (p.fan_table.has_gpu && !f.gpu_curve)) throw std::runtime_error("Fan curves are null");
    auto c=active_curve(*f.cpu_curve,p.fan_table.entries);std::optional<Curve> g;
    if(p.fan_table.has_gpu) g=active_curve(*f.gpu_curve,p.fan_table.entries);
    auto w=fan_curve_writes(p,c,g,f.curve,latch_mask_); // Preflight even linked intent before changing live state.
    for(auto x:w) session_.guard->admit(x.address);
    linked_=false;last_duty_=-1;fan_intent_=f;fan_intent_.cpu_curve=c;++fan_revision_;
    if(g) fan_intent_.gpu_curve=g;
    fan_review_required_=false;fan_application_=FanApplication::unapplied;
    // Source single-fan UI has no linked controller; a saved linked flag cannot create one.
    fan_intent_.linked=f.linked && p.fan_table.has_gpu && p.fan_table.entries>=10;
    if(fan_intent_.linked) {link(temperature("cpuTemp"),temperature("gpuTemp"));linked_=true;}
    else {
        try {batch(w);written_cpu_=c;written_gpu_=g;++written_revision_;fan_application_=FanApplication::applied;}
        catch(...) {fan_application_=FanApplication::partial;throw;}
    }
}
void Controller::link(std::optional<int> c,std::optional<int> g) {
    if(!c || !g || *c<=0 || *g<=0 || *c>=128 || *g>=128) throw std::runtime_error("Linked temperatures unavailable; retaining EC safety ramps");
    int duty=std::max(curve_duty(*fan_intent_.cpu_curve,*c),curve_duty(*fan_intent_.gpu_curve,*g));
    int cr=ramp_start(*c),gr=ramp_start(*g);
    if(last_duty_>=0 && cr<=last_cpu_ramp_ && gr<=last_gpu_ramp_ && (std::abs(duty-last_duty_)<2 || (duty<=7 && last_duty_<=7))) return;
    last_duty_=-1;
    auto cpu=linked_curve(duty,cr),gpu=linked_curve(duty,gr);
    try {batch(fan_curve_writes(session_.profile,cpu,gpu,true,latch_mask_));written_cpu_=std::move(cpu);written_gpu_=std::move(gpu);++written_revision_;fan_application_=FanApplication::applied;}
    catch(...) {fan_application_=FanApplication::partial;throw;}
    last_duty_=duty;last_cpu_ramp_=cr;last_gpu_ramp_=gr;
}
UserProfile Controller::capture(int slot) {
    if(thermal_->tripped()) throw std::runtime_error("Reset thermal protection before saving normal profile settings");
    if(curves_supported() && fan_review_required_) throw std::runtime_error("Review and Apply the editable fallback fan curves before saving a profile");
    UserProfile profile;profile.project_id=session_.profile.project_id;profile.saved_at=timestamp_now();
    profile.cpu=enabled("cpuPower")?cpu():CpuSettings{45,65,90,95};
    if(enabled("gpuPower")) profile.gpu=gpu(true);else {profile.gpu->tgp=115;profile.gpu->dynamic_boost_enabled=false;}
    if(enabled("gpuPower") && nvidia_ && !nvidia_busy_) {
        if(gpu_driver_uncertain_) throw std::runtime_error(nvidia_error_);
        nv::Currents c;nvidia_work([&]{c=nvidia_->currents();});auto& g=*profile.gpu;g.nvvdd_ma=c.nv.current;g.msvdd_ma=c.ms.current;if(c.input) g.board_input_ma=c.input->current;
        auto l=control_limit(session_.profile,"TGP");g.bios_override_enabled=gpu_unlocked_ && (g.tgp<l.min || g.tgp>l.max || g.tgp+(g.dynamic_boost_enabled?g.dynamic_boost_watts:0)>nvidia_->stock_watts() || c.nv.current>c.nv.stock_max || c.ms.current>c.ms.stock_max || (c.input && c.input->current>c.input->stock_max));
    }
    profile.protection=thermal_->settings();profile.fan=enabled("fanControl")?fan_intent_:FanSettings{};
    if(enabled("fanControl")) {
        profile.fan->boost=(read("fanControlMode")&session_.profile.bitfields.at("fanBoost").get_mask())!=0;
        profile.fan->curve=(read("fanCurveEnable")&128)!=0 && (session_.transport().read(0x7C6)&latch_mask_)==4;
        profile.fan->linked=linked_;auto [en,ms]=encoding::decode_switch_speed(read("fanSwitchSpeed"));profile.fan->switch_enabled=en;profile.fan->switch_ms=ms;
    }
    auto placeholder=[&] {
        Curve c;c.temp_up.assign(session_.profile.fan_table.entries,session_.profile.fan_table.padding_temp);c.temp_down=c.temp_up;c.duty.assign(c.temp_up.size(),0);
        if(!c.temp_up.empty()) c.temp_up[0]=c.temp_down[0]=0;return c;
    };
    if(curves_supported()) {
        profile.fan->cpu_curve=saved_curve(*fan_intent_.cpu_curve,session_.profile.fan_table);
        profile.fan->gpu_curve=session_.profile.fan_table.has_gpu?saved_curve(*fan_intent_.gpu_curve,session_.profile.fan_table):placeholder();
    } else {profile.fan->cpu_curve=placeholder();profile.fan->gpu_curve=placeholder();}
    if(enabled("battery")) profile.battery->mode=encoding::decode_battery(read("batteryMode"));
    // Unavailable owners must not discard target-slot peripheral intent. An unreadable
    // existing slot refuses capture rather than being silently replaced with defaults.
    if(auto previous=store_.load(slot)) {profile.lighting=previous->lighting;profile.cooler=previous->cooler;}
    else {
        auto lighting=store_.load_lighting();
        if(!lighting.logo || !lighting.keyboard || !lighting.light_bar) throw std::runtime_error("Cannot capture explicit-null saved lighting zones as off defaults");
        if(lighting.logo) profile.lighting->logo=*lighting.logo;if(lighting.keyboard) profile.lighting->keyboard=*lighting.keyboard;
        if(lighting.light_bar) profile.lighting->light_bar=*lighting.light_bar;profile.lighting->idle_seconds=lighting.idle_seconds;
    }
    if(lighting_ && (peripheral_hardware_ || lighting_send_)) {
        if(!profile.lighting) profile.lighting=LightingSettings{};
        lighting_->capture(*profile.lighting);
    }
    if(peripheral_hardware_) {
        if(!dimming_available_) peripheral_io([&]{dimming_available_=local_dimming_available();});
        if(dimming_available_) try {if(!profile.lighting) profile.lighting=LightingSettings{};profile.lighting->local_dimming=(session_.transport().read(0x72B)&2)!=0;}
            catch(const std::exception& e){diagnostic_(std::string("Dimming capture failed; target intent retained: ")+e.what());}
    }
    if(cooler_) profile.cooler=cooler_->intent(); // Requested settings, never GATT readback.
    if(thermal_->tripped()) throw std::runtime_error("Thermal protection tripped during capture; normal slot was not overwritten");
    return profile;
}
void Controller::recover_settings() {
    reapply_=false;thermal_->enforce();
    int slot=active_slot_?active_slot_:store_.settings().last_slot;
    if(slot) {load_slot(slot,false,true);return;}
    if(curves_supported() && !read_fans(session_.transport(),session_.profile.fan_table).valid()) {
        if(!fan_review_required_ && fan_intent_.cpu_curve && !fan_intent_.cpu_curve->temp_up.empty()) fans(fan_intent_);
        else batch({{session_.profile.reg("fanCurveEnable"),0,128}});
    }
}
std::optional<Sensors> Controller::tick(ULONGLONG now) {
    if(suspended_) return {};
    if(resume_at_ && now>=resume_at_) {
        resume_at_=0;watchdog_=now+30000;
        try {if(!recover()) thermal_->enforce();} catch(const std::exception& ex){diagnostic_(ex.what());}
    }
    if(now>=watchdog_) {watchdog_=now+30000;try {recover();} catch(const std::exception& ex){diagnostic_(ex.what());}}
    Sensors shared;bool safety=thermal_->monitoring();
    if(safety || linked_) {shared.gpu_temp=temperature("gpuTemp").value_or(0);thermal_->sample(shared.gpu_temp);}
    if(lighting_ && !peripheral_busy_) lighting_->tick(now);
    if(reapply_ && !nvidia_busy_ && !peripheral_busy_) try {recover_settings();} catch(const std::exception& ex) {diagnostic_(std::string("Recovery not applied: ")+ex.what());}
    if(gpu_check_at_ && now>=gpu_check_at_ && !nvidia_busy_ && !peripheral_busy_) {gpu_check_at_=0;try {check_gpu();} catch(const std::exception& ex){diagnostic_(std::string("NVIDIA delayed check failed: ")+ex.what());}thermal_->enforce(true);}
    if(visible_ && battery_page_ && enabled("battery") && now>=battery_at_) {
        battery_at_=now+2000;
        try {battery_sample_=battery_telemetry(session_.transport());}catch(const std::exception&) {} // Source telemetry is best effort.
    }
    if(monitor_stopped_ && !thermal_->monitoring() && !linked_) {
        if(now<restart_at_) return {};
        monitor_stopped_=false;telemetry_failures_=0;
    }
    if(linked_ && !nvidia_busy_) { // Only emergency writes may interleave with exclusive NVIDIA work.
        shared.cpu_temp=temperature("cpuTemp").value_or(0);
        try {link(shared.cpu_temp,shared.gpu_temp);linked_error_.clear();}
        catch(const std::exception& ex) {if(linked_error_.empty()) diagnostic_(ex.what());linked_error_=ex.what();}
    }
    try {
        auto s=read_sensors(session_.transport(),session_.profile,!visible_,shared);telemetry_failures_=0;monitor_stopped_=false;return s;
    } catch(const std::exception& ex) {
        ++telemetry_failures_;
        if(telemetry_failures_<=5 || telemetry_failures_%60==0) diagnostic_(std::string("EC telemetry failed: ")+ex.what());
        if(telemetry_failures_>=5 && !safety && !linked_) {monitor_stopped_=true;restart_at_=now+60000;}
        return {};
    }
}
std::optional<Json> Controller::take_battery() {return std::exchange(battery_sample_,{});}
ControlResult Controller::execute(const ControlCommand& command) {
    auto state=[&](ControlResult result=ControlResult{0,"Completed"}) {
        result.active_slot=active_slot_;result.fan_application=fan_application_;result.fan_review_required=fan_review_required_;result.linked=linked_;return result;
    };
    // Lifecycle notifications may pass the exclusive NVIDIA wait. They do not
    // reset the pending command's partial-application accounting or call NVIDIA.
    if(auto c=std::get_if<Visibility>(&command)) {visible_=c->visible;battery_at_=GetTickCount64()+2000;if(c->visible) {monitor_stopped_=false;telemetry_failures_=0;}else battery_sample_.reset();return state();}
    if(auto c=std::get_if<BatteryPage>(&command)) {battery_page_=c->active;battery_at_=GetTickCount64()+2000;if(!c->active) battery_sample_.reset();return state();}
    if(auto c=std::get_if<UiPage>(&command)) {ui_page_=c->page;return state();}
    if(auto c=std::get_if<UiRead>(&command)) {
        auto r=state();Json data=Json::object();
        if(c->page==4) data=*execute(PeripheralRead{}).peripherals;
        if(c->page==3 && enabled("battery")) try {data["battery"]=battery_telemetry(session_.transport());}catch(const std::exception& e){data["batteryError"]=e.what();}
        if(c->page==6) {data["coolerAvailable"]=bool(cooler_);if(cooler_) {data["cooler"]=cooler_->snapshot();if(data["cooler"]["bluetoothAvailable"].is_null()) cooler_->request({CoolerAction::availability});}}
        data["ui"]=presentation(true,false,c->page);r.peripherals=std::move(data);return r;
    }
    if(auto c=std::get_if<AcEvent>(&command)) {if(on_ac_!=c->on_ac) {on_ac_=c->on_ac;gpu_check_at_=GetTickCount64()+5000;}return state();}
    if(auto c=std::get_if<PowerEvent>(&command)) {
        suspended_=c->suspended;
        if(!c->suspended) {thermal_->invalidate();++epoch_;fan_application_=FanApplication::unapplied;resume_at_=GetTickCount64()+3000;watchdog_=resume_at_;gpu_check_at_=GetTickCount64()+8000;if(lighting_) lighting_->resume(GetTickCount64());}
        return state();
    }
    ControlResult result;writes_started_=false;application_completed_=false;
    try {
        std::visit([&](const auto& c) {
            using T=std::decay_t<decltype(c)>;auto& p=session_.profile;
            if constexpr(std::is_same_v<T,CpuApply>) {
                if(!enabled("cpuPower")) throw std::runtime_error("CPU control unavailable");
                auto s=c.settings;auto w=cpu_writes(p,s.pl1,s.pl2,s.pl4,s.temp_limit);explicit_apply([&]{batch(w);});result.applied=true;
            } else if constexpr(std::is_same_v<T,GpuApply>) {
                if(!enabled("gpuPower")) throw std::runtime_error("GPU control unavailable");
                std::vector<EcWrite> cpu;if(c.cpu) {if(!enabled("cpuPower")) throw std::runtime_error("CPU control unavailable");auto v=*c.cpu;cpu=cpu_writes(p,v.pl1,v.pl2,v.pl4,v.temp_limit);}
                auto g=c.settings;if(nvidia_busy_) throw std::runtime_error("NVIDIA operation in progress");if(c.power) gpu_plan(g);
                if(c.restart_confirmed) {
                    auto plan=plan_gpu(g,false);explicit_apply([&]{configure_gpu(plan);if(!cpu.empty()) batch(cpu);if(c.power) apply_gpu(g);});
                } else {validate_currents(g);explicit_apply([&]{if(g.nvvdd_ma || g.msvdd_ma || g.board_input_ma) nvidia_work([&]{nvidia_->configure(gpu_unlocked_,g,false);});if(!cpu.empty()) batch(cpu);if(c.power) apply_gpu(g);});}result.applied=true;
            } else if constexpr(std::is_same_v<T,UiPreflight>) {
                if(c.cpu && enabled("cpuPower")) {auto v=*c.cpu;cpu_writes(p,v.pl1,v.pl2,v.pl4,v.temp_limit);}
                std::optional<UserProfile> profile;if(c.slot) {profile=store_.load(c.slot);if(!profile) throw std::runtime_error("Requested slot is missing");}
                auto g=profile?profile->gpu:c.gpu;if(!g) throw std::runtime_error("GPU intent unavailable");
                auto value=enabled("gpuPower")?(profile?plan(*profile,false):plan_gpu(*g,false)):nv::ProfilePlan{};
                result.peripherals=Json{{"plan",{{"restart",value.restart},{"unlock",value.unlock},{"wasUnlocked",gpu_unlocked_},{"slot",c.slot},{"gpu",*g},{"stockWatts",nvidia_?nvidia_->stock_watts():0}}}};
            } else if constexpr(std::is_same_v<T,GpuUnlock>) {unlock_gpu(c.enabled);result.applied=true;
            } else if constexpr(std::is_same_v<T,DlppcApply>) {dlppc(c.disabled);result.applied=true;result.saved=true;
            } else if constexpr(std::is_same_v<T,FanBoost>) {
                if(!c.enabled) {thermal_->check_before_apply();if(thermal_->holds(Fan)) throw std::runtime_error("Fan boost is held by thermal protection");}
                boost(c.enabled);result.applied=true;
            } else if constexpr(std::is_same_v<T,FanMaster>) {
                if(!enabled("fanControl")) throw std::runtime_error("Fan control unavailable");
                auto mask=p.bitfields.at("fanCurveEnable").get_mask();batch({{p.reg("fanCurveEnable"),uint8_t(c.enabled?mask:0),mask}});fan_intent_.curve=c.enabled;++fan_revision_;result.applied=true;
            } else if constexpr(std::is_same_v<T,FanEdit>) {
                if(!linked_ || !curves_supported()) throw std::runtime_error("Linked editor is not active");
                if(!c.settings.cpu_curve || !c.settings.gpu_curve) throw std::runtime_error("Linked editor curves are null");
                auto cpu=active_curve(*c.settings.cpu_curve,p.fan_table.entries),gpu=active_curve(*c.settings.gpu_curve,p.fan_table.entries);
                fan_intent_.cpu_curve=std::move(cpu);fan_intent_.gpu_curve=std::move(gpu);fan_application_=FanApplication::unapplied;++fan_revision_;
            } else if constexpr(std::is_same_v<T,FanRead>) {
                if(linked_) throw std::runtime_error("Disable linked fans before reading EC tables");
                if(curves_supported()) {auto snapshot=read_fans(session_.transport(),p.fan_table);if(!snapshot.valid()) throw std::runtime_error("Current EC fan table is invalid; editor unchanged");
                    auto cpu=active_curve(snapshot.cpu,p.fan_table.entries);std::optional<Curve> gpu;if(p.fan_table.has_gpu) gpu=active_curve(snapshot.gpu,p.fan_table.entries);
                    fan_intent_.cpu_curve=std::move(cpu);if(gpu) fan_intent_.gpu_curve=std::move(gpu);fan_intent_.curve=snapshot.custom_mode(latch_mask_);++fan_revision_;}
                result.peripherals=Json{{"ui",presentation(true,false,1)}};
            } else if constexpr(std::is_same_v<T,FanApply>) {fans(c.settings);result.applied=true;
            } else if constexpr(std::is_same_v<T,BatteryApply>) {battery(c.mode);result.applied=true;
            } else if constexpr(std::is_same_v<T,SwitchApply>) {speed(c.enabled,c.milliseconds);result.applied=true;
            } else if constexpr(std::is_same_v<T,LightingApply>) {
                if(!lighting_) throw std::runtime_error("Lighting owner unavailable");
                if(!lighting_->available(c.zone)) throw std::runtime_error("HID lighting zone unavailable");
                writes_started_=true;lighting_->change(c.zone,c.settings,c.flash,c.color_only);result.applied=true;result.text="Lighting applied; intent updated, persistence failures are logged";
            } else if constexpr(std::is_same_v<T,LightingIdle>) {
                if(!lighting_) throw std::runtime_error("Lighting owner unavailable");lighting_->idle(c.seconds);store_.save_lighting(lighting_->intent());result.text="Lighting idle intent updated; persistence failures are logged";
            } else if constexpr(std::is_same_v<T,DimmingApply>) {
                dimming(c.enabled);result.applied=true;
            } else if constexpr(std::is_same_v<T,PeripheralRead>) {
                Json j={{"dimmingAvailable",dimming_available_},{"coolerAvailable",bool(cooler_)}};
                if(lighting_) {j["lightingIntent"]=lighting_->intent();j["zones"]={lighting_->available(0),lighting_->available(2),lighting_->available(3)};
                    try {j["logo"]=lighting_->logo();}catch(const std::exception& e){j["logoError"]=e.what();}}
                if(peripheral_hardware_ && !dimming_available_) peripheral_io([&]{dimming_available_=local_dimming_available();});j["dimmingAvailable"]=dimming_available_;
                if(dimming_available_) try {j["localDimming"]=(session_.transport().read(0x72B)&2)!=0;}catch(const std::exception& e){j["dimmingError"]=e.what();}
                if(cooler_) j["cooler"]=cooler_->snapshot();
                if(c.battery && enabled("battery")) try {j["battery"]=battery_telemetry(session_.transport());}catch(const std::exception& e){j["batteryError"]=e.what();}result.peripherals=std::move(j);
            } else if constexpr(std::is_same_v<T,CoolerRequest>) {
                if(!cooler_) throw std::runtime_error("Model/runtime cooler support unavailable");cooler_->request(c);result.text="Cooler request queued; hardware completion is asynchronous";
            } else if constexpr(std::is_same_v<T,ProtectionApply>) {thermal_->configure(c.settings);thermal_->check_before_apply();result.applied=true;
            } else if constexpr(std::is_same_v<T,SlotLoad>) {
                load_slot(c.slot,c.deliberate);result.applied=true;result.saved=c.deliberate;
                  result.text=thermal_->tripped()?"Profile applied with thermal-held substitutions; normal held power intent was not applied":"Profile applied; peripheral errors are log-only, BLE completion is asynchronous";
                 if(!gpu_notice_.empty()) result.text+="; "+gpu_notice_;
            } else if constexpr(std::is_same_v<T,SlotSave>) {
                int slot=c.slot?c.slot:1;auto profile=capture(slot);
                if(c.editor_fan && curves_supported()) {
                    if(!c.editor_fan->cpu_curve || (p.fan_table.has_gpu && !c.editor_fan->gpu_curve)) throw std::runtime_error("Editor fan curves are null");
                    profile.fan->cpu_curve=saved_curve(*c.editor_fan->cpu_curve,p.fan_table);if(p.fan_table.has_gpu) profile.fan->gpu_curve=saved_curve(*c.editor_fan->gpu_curve,p.fan_table);
                    profile.fan->linked=c.editor_fan->linked && p.fan_table.has_gpu && p.fan_table.entries>=10;
                }
                if(c.editor_cooler && cooler_) profile.cooler=*c.editor_cooler;
                profile.name=store_.settings().names.at(slot-1);store_.save(slot,profile);saved_profile_=profile;active_slot_=slot;result.saved=true;result.slot_committed=true;
                result.text="Settings saved; peripheral reads are best effort, failures are logged; unavailable target intent preserved";
            } else if constexpr(std::is_same_v<T,SlotRename>) {store_.rename(c.slot,c.name);result.saved=true;
            } else if constexpr(std::is_same_v<T,SlotClear>) {store_.clear(c.slot);if(active_slot_==c.slot) {active_slot_=0;saved_profile_.reset();}result.saved=true;
            } else if constexpr(std::is_same_v<T,ThermalReset>) {thermal_->reset();if(active_slot_) load_slot(active_slot_,false,true);result.applied=true;
            } else if constexpr(std::is_same_v<T,ThermalTest>) {
                if(c.expected && Json(*c.expected)!=Json(thermal_->settings())) throw std::runtime_error("Applied settings changed during confirmation; review the actions and test again");
                thermal_->test();result.applied=thermal_->complete();if(!result.applied) {result.code=1;result.text=thermal_->status();}
            } else if constexpr(std::is_same_v<T,DefaultsApply>) {result.text=defaults(c.mode);result.applied=true;
            } else if constexpr(std::is_same_v<T,CliCommand>) {
                auto reply=cli(c.arguments);result.code=reply.code;result.text=reply.text;
                result.applied=reply.code==0 && writes_started_;result.partial=reply.code!=0 && writes_started_;
            }
        },command);
        thermal_->enforce(true);
        if(result.text.empty()) result.text=result.applied && thermal_->tripped()?"Completed with current thermal holds; normal held settings were not applied":"Completed";
    } catch(const std::exception& ex) {
        result.code=1;result.partial=writes_started_;result.text=(writes_started_?"Partial application possible: ":"")+std::string(ex.what());
        if(dynamic_cast<const SlotSelectionSaveError*>(&ex)) {result.slot_committed=true;result.partial=true;}
        if(std::holds_alternative<SlotLoad>(command) && application_completed_) result.applied=true;
        if(std::holds_alternative<DefaultsApply>(command)) result.text="Defaults not fully completed: "+result.text;
        diagnostic_("Controller command not completed: "+result.text);
        if(thermal_->tripped() && writes_started_) thermal_->enforce();
    }
    return state(std::move(result));
}
}
