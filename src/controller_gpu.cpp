#include "controller.h"
#include <algorithm>

namespace mech {
using namespace config;
int Controller::gpu_unlock_min() const {auto l=control_limit(session_.profile,"TGP");return std::max(nvidia_min_watts_,int((int64_t(l.min)*60+99)/100));}
int Controller::gpu_unlock_max() const {auto l=control_limit(session_.profile,"TGP");return int(std::min({int64_t(l.max)+75,int64_t(session_.profile.tgp_offset())+254,int64_t(INT_MAX)}));}
bool Controller::gpu_extend() const {auto& p=session_.profile;auto l=control_limit(p,"TGP");return !p.tgp_fallback && p.limits.contains("TGP") && l.encoding=="offset" && l.offset && gpu_unlock_max()>l.max;}
int Controller::gpu_min() const {auto l=control_limit(session_.profile,"TGP");return gpu_unlocked_ && gpu_extend()?gpu_unlock_min():l.min;}
int Controller::gpu_max() const {auto l=control_limit(session_.profile,"TGP");return gpu_unlocked_ && gpu_extend()?gpu_unlock_max():l.max;}
std::string Controller::nvidia_unavailable() const {
    if(nvidia_busy_) return "NVIDIA operation in progress; try again in a few seconds";
    if(!nvidia_error_.empty()) return nvidia_error_;
    if(!nvidia_available_) return "No NVIDIA GPU control on this machine";
    return gpu_extend()?"NVIDIA not connected; to use current limits, turn on the GPU unlock under Show advanced controls":"NVIDIA not connected; current limits need the GPU unlock, which is unavailable on this model";
}
nv::Power& Controller::open_nvidia() {
    if(nvidia_busy_) throw std::runtime_error("NVIDIA operation in progress; try again in a few seconds");
    if(gpu_driver_uncertain_) throw std::runtime_error(nvidia_error_);
    if(nvidia_) return *nvidia_;if(!nvidia_available_) throw std::runtime_error(nvidia_error_.empty()?"No NVIDIA GPU control on this machine":nvidia_error_);
    dlppc_api_.reset(); // One NVAPI owner: Power replaces the DLPPC-only Api; a refusal reopens it on demand.
    try {std::unique_ptr<nv::Power> opened;nvidia_work([&]{opened=std::make_unique<nv::Power>(control_limit(session_.profile,"TGP").min,diagnostic_);});nvidia_=std::move(opened);writes_started_|=nvidia_->modified;gpu_unlocked_=nvidia_->enabled;nvidia_min_watts_=nvidia_->minimum_watts();thermal_->invalidate();thermal_->enforce();return *nvidia_;}
    catch(const std::exception& ex) {nvidia_available_=false;nvidia_error_=ex.what();diagnostic_("NVIDIA unavailable: "+nvidia_error_);throw;}
}
void Controller::nvidia_work(const std::function<void()>& work) {
    if(nvidia_busy_) throw std::runtime_error("NVIDIA operation in progress; EC minimum applied, driver verification pending");
    if(nvidia_) nvidia_->modified=false;
    nvidia_busy_=true;
    auto complete=[&] {
        nvidia_busy_=false;
        if(nvidia_) {gpu_unlocked_=nvidia_->enabled;gpu_driver_uncertain_=nvidia_->uncertain;writes_started_|=nvidia_->modified;}
        if(gpu_driver_uncertain_) nvidia_error_="NVIDIA limits changed without active verification; restart Windows and reopen MechControl before normal GPU changes";
    };
    try {run_blocking_(work);} catch(...) {complete();throw;}
    complete();
}
std::vector<EcWrite> Controller::gpu_plan(const GpuSettings& g) {
    if(gpu_driver_uncertain_ && !thermal_->holds(Gpu)) throw std::runtime_error(nvidia_error_);
    if(g.tgp<gpu_min() || g.tgp>gpu_max()) throw std::runtime_error("GPU TGP exceeds active model range; enable/apply the unlock manually first");
    auto& p=session_.profile;auto l=control_limit(p,"TGP");
    return gpu_writes(p,std::max(g.tgp,l.min),g.dynamic_boost_enabled,g.dynamic_boost_watts,gpu_max());
}
nv::ProfilePlan Controller::plan_gpu(const GpuSettings& settings,bool automatic) {
    if(gpu_driver_uncertain_) throw std::runtime_error(nvidia_error_);
    if(nvidia_busy_) throw std::runtime_error("NVIDIA operation in progress");auto l=control_limit(session_.profile,"TGP");
    int lo=gpu_unlock_min(),hi=gpu_unlock_max();
    bool unlocked=nvidia_ && nvidia_->enabled;
    bool wants=(settings.bios_override_enabled==true || settings.tgp<l.min || settings.tgp>l.max) && settings.tgp>=lo && settings.tgp<=hi;
    if(!automatic && !unlocked && gpu_extend() && wants && nvidia_available_) {try {open_nvidia();} catch(...) {}}
    lo=gpu_unlock_min();
    std::optional<nv::Currents> currents;
    bool needs_currents=settings.nvvdd_ma || settings.msvdd_ma || settings.board_input_ma || (gpu_unlocked_ && settings.bios_override_enabled!=true);
    if(nvidia_ && needs_currents) nvidia_work([&]{currents=nvidia_->currents();});
    auto plan=nv::plan_profile(settings,automatic,gpu_extend(),l.min,l.max,lo,hi,nvidia_ && nvidia_->enabled,nvidia_available_,currents,
        [this](bool unlock,const GpuSettings& g){bool restart=false;nvidia_work([&]{restart=nvidia_->needs_restart(unlock,g);});return restart;});
    // Validate the complete proposed range, including a deliberate not-yet-active unlock.
    gpu_writes(session_.profile,std::max(plan.settings.tgp,l.min),plan.settings.dynamic_boost_enabled,plan.settings.dynamic_boost_watts,plan.unlock && gpu_extend()?hi:l.max);
    return plan;
}
void Controller::configure_gpu(const nv::ProfilePlan& plan) {
    gpu_notice_=plan.notice;if(!gpu_notice_.empty()) diagnostic_(gpu_notice_);
    auto& g=plan.settings;
    if(thermal_->holds(Gpu) || (!plan.restart && !g.nvvdd_ma && !g.msvdd_ma && !g.board_input_ma)) return;
    auto& driver=open_nvidia();if(thermal_->holds(Gpu)) return;
    if(!plan.restart) {nvidia_work([&]{driver.configure(plan.unlock,g,false);});return;}
    // The background work owns NVIDIA exclusively; only this thread can touch EC.
    // Its completion returns before any normal profile writes resume.
    try {nvidia_work([&]{driver.configure(plan.unlock,plan.settings,true);});}
    catch(...) {if(driver.uncertain) {writes_started_=true;driver_watts_={};thermal_->invalidate();thermal_->enforce();}throw;}
    writes_started_=true;driver_watts_={};thermal_->invalidate();
    if(dlppc_disabled_) {try {nvidia_work([&]{driver.set_dlppc(false);});} catch(const std::exception& ex){diagnostic_(ex.what());}}
    thermal_->enforce(); // A trip during restart outranks the pending normal request.
}
void Controller::validate_currents(const GpuSettings& g) {
    if(!g.nvvdd_ma && !g.msvdd_ma && !g.board_input_ma) return;
    if(gpu_driver_uncertain_) throw std::runtime_error(nvidia_error_);
    if(nvidia_busy_ || !nvidia_) throw std::runtime_error(nvidia_unavailable());
    nv::Currents c;nvidia_work([&]{c=nvidia_->currents();});bool unlock=nvidia_->enabled;
    auto validate=[&](std::optional<int> v,std::optional<nv::Rail> r,const char* name) {
        if(!v) return;if(!r) throw std::runtime_error(std::string("This GPU has no ")+name+" current limit");
        if(*v<std::max(1000,int(r->min)) || *v>std::min(r->allowed(unlock),int(r->max))) throw std::runtime_error("Current outside active range; higher ceilings need the GPU unlock (Power tab, advanced controls) or a GPU restart");
    };
    validate(g.nvvdd_ma,c.nv,"NVVDD");validate(g.msvdd_ma,c.ms,"MSVDD");validate(g.board_input_ma,c.input,"board input");
    bool restart=false;nvidia_work([&]{restart=nvidia_->needs_restart(unlock,g);});if(restart) throw std::runtime_error("Current limits need a GPU restart; apply manually from the Power tab");
}
void Controller::apply_gpu(GpuSettings g) {
    auto requested=g;auto min=control_limit(session_.profile,"TGP").min;
    auto held=[&] {if(thermal_->holds(Gpu)) {if(!gpu_minimum_) throw std::runtime_error("Model-specific GPU minimum unavailable");g.tgp=gpu_unlocked_?std::clamp(std::min(g.tgp,driver_watts_.value_or(g.tgp)),gpu_min(),min):min;g.dynamic_boost_enabled=false;g.dynamic_boost_watts=0;}};
    held();
    if(!nvidia_busy_ && gpu_unlocked_) {
        nvidia_work([&]{nvidia_->budget();});held(); // Qualify the live budget before EC writes, including a reduced Dynamic Boost-off budget.
    }
    batch(gpu_plan(g));
    if(nvidia_busy_) throw std::runtime_error("NVIDIA operation in progress; EC minimum applied, driver verification pending");
    if(gpu_unlocked_ || gpu_driver_uncertain_) {
        int watts=0;nvidia_work([&]{auto power=nvidia_->limits();watts=std::min(int(power.max/1000),g.tgp+(g.dynamic_boost_enabled?g.dynamic_boost_watts:0));nvidia_->apply(watts,g.dynamic_boost_enabled);});driver_watts_=watts;
    } else driver_watts_={};
    last_gpu_=requested;if(nvidia_) gpu_check_at_=GetTickCount64()+3000;
    thermal_->enforce(true);
}
void Controller::check_gpu() {
    if(!nvidia_ || nvidia_busy_ || gpu_driver_uncertain_ || !nvidia_->awake()) return; // All delayed reads are sleep-gated too.
    if(dlppc_disabled_) try {nvidia_work([&]{auto d=nvidia_->dlppc();if(d && d->active) {diagnostic_("NVIDIA DLPPC was re-enabled (GPU restart); disabling it again");nvidia_->set_dlppc(false);}});} catch(const std::exception& ex){diagnostic_(std::string("NVIDIA DLPPC check failed: ")+ex.what());}
    try {nvidia_work([&]{if(nvidia_->restore_currents()) diagnostic_("NVIDIA current limits were reset by the driver; reapplied");});} catch(const std::exception& ex){diagnostic_(std::string("NVIDIA current check failed: ")+ex.what());}
    bool applied=true;if(last_gpu_ && driver_watts_ && nvidia_->enabled) nvidia_work([&]{applied=nvidia_->applied(*driver_watts_);});
    if(!applied) {diagnostic_("NVIDIA driver request or laptop budget below "+std::to_string(*driver_watts_)+" W; reapplying");apply_gpu(*last_gpu_);return;}
    if(on_ac_) {
        auto g=gpu();nv::Budget budget;nvidia_work([&]{budget=nvidia_->budget();});auto expected=g.tgp+(g.dynamic_boost_enabled?g.dynamic_boost_watts:0);
        if(budget.f7/1000<uint32_t(expected)) diagnostic_("NVIDIA laptop budget "+std::to_string(budget.f7/1000)+" W is below the EC request "+std::to_string(expected)
            +" W on AC; Dynamic Boost may be stuck. Restart NVIDIA Platform Controllers and Framework to restore it.");
    }
}
void Controller::unlock_gpu(bool enabled_value) {
    thermal_->check_before_apply();if(thermal_->tripped()) throw std::runtime_error("Reset thermal failsafe before changing NVIDIA BIOS limits");
    auto& driver=open_nvidia();if(thermal_->tripped()) throw std::runtime_error("Thermal protection tripped while connecting NVIDIA; unlock was not changed");if(driver.enabled==enabled_value) return;
    nv::ProfilePlan plan;plan.unlock=enabled_value;plan.restart=true;configure_gpu(plan);
    auto g=gpu();g.tgp=std::clamp(g.tgp,gpu_min(),gpu_max());apply_gpu(g);
}
// DLPPC needs only NVAPI, so unsupported BIOS limits (no nv::Power) do not block it.
void Controller::open_dlppc() {
    if(nvidia_busy_) throw std::runtime_error("NVIDIA operation in progress; try again in a few seconds");
    if(gpu_driver_uncertain_) throw std::runtime_error(nvidia_error_);
    if(nvidia_ || dlppc_api_) return;if(!dlppc_available_) throw std::runtime_error(nvidia_unavailable());
    std::unique_ptr<nv::Api> opened;
    try {nvidia_work([&]{opened=std::make_unique<nv::Api>();});}
    catch(const std::exception& ex) {dlppc_available_=false;diagnostic_(std::string("NVIDIA DLPPC unavailable: ")+ex.what());throw;}
    dlppc_api_=std::move(opened);
}
// Call inside nvidia_work after open_dlppc.
std::optional<nv::Dlppc> Controller::read_dlppc() {return nvidia_?nvidia_->dlppc():dlppc_api_->dlppc();}
void Controller::write_dlppc(bool active) {
    if(nvidia_) nvidia_->set_dlppc(active);else dlppc_api_->set_dlppc(active);
    diagnostic_(std::string("NVIDIA DLPPC (AI-controlled memory clock) ")+(active?"enabled":"disabled"));
}
void Controller::dlppc(bool disabled) {
    if(!nvidia_ && nvidia_available_) try {open_nvidia();} catch(const std::exception&) {} // Logged by open_nvidia.
    open_dlppc();bool write=false;
    try {nvidia_work([&]{if(!read_dlppc()) throw std::runtime_error("This GPU/driver has no DLPPC policy");write=true;write_dlppc(!disabled);});}
    catch(...) {writes_started_|=write;throw;}
    writes_started_=true;dlppc_disabled_=disabled;
    store_.set_disable_dlppc(disabled); // The store owns preference save and rollback.
}
}
