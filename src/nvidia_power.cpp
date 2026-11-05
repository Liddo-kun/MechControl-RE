#include "nvidia.h"
#include "config_text.h"
#include "win_handle.h"
#include <algorithm>

namespace mech::nv {
Power::Power(int minimum,std::function<void(const std::string&)> log):log_(std::move(log)) {
    api_=std::make_unique<Api>();auto& id=api_->identity;
    char pci[32];sprintf_s(pci,"PCI 10DE:%04X/%08X",id.device,id.subsystem);auto gpu=id.name+" ("+pci+"), driver "+id.driver+", BIOS "+id.bios;
    try {
        store_=std::make_unique<Store>(id);auto active=api_->bios();bool staged=false,discarded=false;original_=store_->open(active,staged,discarded);
        if(discarded) log_("NVIDIA owned overrides no longer apply (BIOS replaced or driver reinstalled); discarded");
        if(original_.device!=id.device || !config::equal_ignore_case(original_.version(),id.bios)) throw std::runtime_error("NVIDIA BIOS and GPU identity differ");
        if(original_.nv.def>INT_MAX/2 || (original_.ms && original_.ms->def>INT_MAX/2) || (original_.input && original_.input->def>INT_MAX/2)) throw std::runtime_error("NVIDIA current ranges exceed supported slider units");
        verify(active);bool patched=active!=original_.image;if(staged!=patched) throw std::runtime_error("NVIDIA staged/active BIOS limits differ. Restart Windows, then reopen MechControl.");enabled=staged;
        auto limits=api_->power();if(int64_t(minimum)*1000<limits.min || int64_t(minimum)*1000>original_.power.max) throw std::runtime_error("NVIDIA driver cannot accept this model's minimum TGP");
    } catch(...) {log_("NVIDIA GPU: "+gpu);throw;}
    log_("NVIDIA connected: "+gpu+"; "+(original_.ada?"RTX 40":"RTX 50")+" BIOS table; stock ceiling "+std::to_string(stock_watts())+" W");
}
void Power::verify(const Bytes& active) {
    auto b=Bios::parse(active);auto p=api_->power();auto c=api_->currents();
    auto matches=[](Rail r,Field f){return r.min==f.min && r.def==f.def && r.max==f.max;};
    if(p.min!=b.power.min || p.def!=b.power.def || p.max!=b.power.max || !matches(c.nv,b.nv) || c.ms.has_value()!=b.ms.has_value() || (c.ms && !matches(*c.ms,*b.ms))) {
        auto text=[](uint32_t min,uint32_t def,uint32_t max){return std::to_string(min)+"/"+std::to_string(def)+"/"+std::to_string(max);};
        auto ms=[&](auto& r){return r?text(r->min,r->def,r->max):std::string("none");};
        throw std::runtime_error("NVIDIA driver limits do not match parsed BIOS policies (BIOS power "+text(b.power.min,b.power.def,b.power.max)+" NVVDD "+text(b.nv.min,b.nv.def,b.nv.max)+" MSVDD "+ms(b.ms)
            +"; driver power "+text(p.min,p.def,p.max)+" NVVDD "+text(c.nv.min,c.nv.def,c.nv.max)+" MSVDD "+ms(c.ms)+")");
    }
    board_verified_=b.input && c.input && matches(*c.input,*b.input);if(b.input && !board_verified_) log_("NVIDIA board input unavailable: no driver policy matches BIOS");
}
Currents Power::currents() {
    auto c=api_->currents();auto convert=[](Rail r,Field f){r.min=std::max(r.min,f.min);r.def=f.def;r.stock_max=f.max;return r;};c.nv=convert(c.nv,original_.nv);if(c.ms && original_.ms) c.ms=convert(*c.ms,*original_.ms);else c.ms={};
    // Board input gets more unlock room than the core rails, as in the source (11.053 A stock -> 17.02 A).
    if(board_verified_ && c.input && original_.input) {c.input=convert(*c.input,*original_.input);c.input->unlock_percent=154;}else c.input={};return c;
}
Telemetry Power::telemetry() {auto t=api_->telemetry();if(!board_verified_) {t.input={};t.input_limit={};}return t;}
bool Power::needs_restart(bool unlock,const config::GpuSettings& g) {
    auto c=currents();return unlock!=enabled || (unlock && (api_->power().max<original_.power.max+75000 || (g.nvvdd_ma && *g.nvvdd_ma>int(c.nv.max)) || (g.msvdd_ma && c.ms && *g.msvdd_ma>int(c.ms->max)) || (g.board_input_ma && c.input && *g.board_input_ma>int(c.input->max))));
}
void Power::configure(bool unlock,const config::GpuSettings& g,bool allow_restart) {
    modified=false;
    auto c=currents();auto validate=[&](std::optional<int> value,std::optional<Rail> r,const char* name){if(value && !r) throw std::runtime_error(std::string(name)+" current unavailable on this GPU");if(value && (*value<int(r->min) || *value>r->allowed(unlock))) throw std::runtime_error("NVIDIA current outside detected range");};
    validate(g.nvvdd_ma,c.nv,"NVVDD");validate(g.msvdd_ma,c.ms,"MSVDD");validate(g.board_input_ma,c.input,"Board input");
    bool reset=needs_restart(unlock,g);if(reset && !allow_restart) throw std::runtime_error("These NVIDIA limits require a GPU restart. Apply manually first.");
    Override request;if(unlock) {
        auto rail=[](std::optional<int> value,Rail r){auto max=static_cast<uint32_t>(r.allowed(true));return RailOverride{value?static_cast<uint32_t>(*value):std::min(r.current,max),max};};
        request.power=original_.power.max+75000;request.nv=rail(g.nvvdd_ma,c.nv);if(c.ms) request.ms=rail(g.msvdd_ma,*c.ms);if(c.input) request.input=rail(g.board_input_ma,*c.input);
    }
    auto records=original_.build(request);auto input=g.board_input_ma;
    if(reset) {
        restore_budget();auto expected=original_.image;for(auto& r:records) put(expected,u32(r.data,5),u32(r.data,10));
        try {store_->stage(original_,records);} catch(...) {modified=uncertain=store_->modified;throw;}
        modified=uncertain=store_->modified;restart();auto active=api_->bios();if(active!=expected) throw std::runtime_error("NVIDIA did not consume staged BIOS limits. Restart Windows to activate them.");
        verify(active);enabled=unlock;uncertain=false;if(!board_verified_) input={};if(!unlock) store_->delete_recovery();
    } else if(unlock && !store_->matches(records)) {
        try {store_->stage(original_,records);modified=store_->modified;} catch(...) {modified=uncertain=store_->modified;throw;}
    }
    if(g.nvvdd_ma || g.msvdd_ma || input) {modified=true;api_->set_currents(g.nvvdd_ma,g.msvdd_ma,input);}
    log_(std::string("NVIDIA limits configured; unlock ")+(enabled?"on":"off")+"; GPU restart "+(reset?"yes":"no"));
}
void Power::restart() {
    wchar_t system[MAX_PATH]{};if(!GetSystemDirectoryW(system,MAX_PATH)) throw std::runtime_error("Cannot locate pnputil");
    auto exe=std::filesystem::path(system)/L"pnputil.exe";std::wstring command=L"\""+exe.wstring()+L"\" /restart-device \""+store_->instance+L"\"";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)) throw std::system_error(GetLastError(),std::system_category(),"NVIDIA restart");
    modified=uncertain=true;
    Handle thread(process.hThread),handle(process.hProcess);if(WaitForSingleObject(handle.get(),60000)!=WAIT_OBJECT_0) throw std::runtime_error("GPU restart did not finish within 60 seconds; restart Windows before applying limits");
    DWORD code=0;if(!GetExitCodeProcess(handle.get(),&code) || code) throw std::runtime_error("GPU restart failed; restart Windows to activate staged limits");
    for(int i=0;;++i) {try {api_->reinitialize();Sleep(5000);return;} catch(...) {if(i>=19) throw;Sleep(250);}}
}
void Power::restore_budget() {
    if(!budget_original_ || !budget_applied_) return;auto saved=*budget_original_,applied=*budget_applied_;budget_original_=budget_applied_={};
    try {auto current=api_->budget(),desired=current;if(current.f7==applied.f7) desired.f7=saved.f7;if(current.platform==applied.platform) desired.platform=saved.platform;if(desired!=current) api_->set_budget(desired);}
    catch(const std::exception& ex) {log_(std::string("NVIDIA laptop budget not restored: ")+ex.what());}
}
void Power::apply(int watts) {
    if(watts<0 || int64_t(watts)*1000>UINT32_MAX) throw std::runtime_error("Invalid NVIDIA watts");uint32_t target=uint32_t(watts)*1000;
    bool raise=enabled && target>original_.power.max;
    if(raise) {auto current=api_->budget();if(!budget_applied_ || budget_applied_->platform!=current.platform) budget_original_=current;
        auto desired=current.raise(target,*budget_original_);if(desired!=current) {budget_applied_=desired;api_->set_budget(desired);}}
    api_->set_power(target);if(!raise) restore_budget();
}
bool Power::at_most(int watts) {auto p=api_->power();return p.requested && *p.requested<=uint32_t(watts)*1000 && p.enforced<=uint32_t(watts)*1000;}
bool Power::applied(int watts) {return api_->power().requested==uint32_t(watts)*1000 && (watts<=stock_watts() || api_->budget().f7>=uint32_t(watts)*1000);}
}
