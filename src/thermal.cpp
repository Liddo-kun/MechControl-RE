#include "thermal.h"
#include <windows.h>
#include <numeric>
#include <algorithm>
#include <cstdio>

namespace mech {
std::string timestamp_now() {
    SYSTEMTIME t; GetSystemTime(&t); char text[40];
    sprintf_s(text,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
    return text;
}
static bool valid(std::optional<int> t) { return t && *t>0 && *t<128; }
ThermalProtection::ThermalProtection(config::ProfileStore& store,int supported,std::function<void(int)> apply,
    std::function<bool(int)> matches,std::function<std::optional<int>()> temperature,
    std::function<void(const std::string&)> diagnostic):store_(store),supported_(supported),state_(store.settings().thermal),
    apply_(std::move(apply)),matches_(std::move(matches)),temperature_(std::move(temperature)),diagnostic_(std::move(diagnostic)) {
    if(state_.last_trip) settings_.show_window=state_.last_trip->show_window;
}
void ThermalProtection::validate(const config::ProtectionSettings& s) const {
    if(s.threshold<50 || s.threshold>95) throw std::runtime_error("Protection threshold must be 50-95 C");
    if(s.enabled && !(supported_&config::Gpu)) throw std::runtime_error("GPU protection unavailable on this model");
}
void ThermalProtection::configure(const config::ProtectionSettings& s) {
    validate(s);
    if(s.enabled && !settings_.enabled && !tripped()) {window_.clear();invalid_=0;peak_temperature_.reset();}
    settings_=s;
    if(!monitoring()) {window_.clear();current_temperature_.reset();}
}
std::optional<double> ThermalProtection::average() const {
    if(window_.empty()) return {};
    return std::accumulate(window_.begin(),window_.end(),0.0)/window_.size();
}
bool ThermalProtection::over() const { return window_.size()>=3 && *average()>=settings_.threshold; }
void ThermalProtection::add(int t) {
    current_temperature_=t;peak_temperature_=std::max(peak_temperature_.value_or(t),t);
    invalid_=0;window_.push_back(t);if(window_.size()>5) window_.pop_front();
}
void ThermalProtection::sample(std::optional<int> t,bool defer_maintenance) {
    if(!monitoring()) return;
    if(valid(t)) add(*t); else {current_temperature_.reset();++invalid_;}
    if(!tripped() && settings_.enabled) {
        if(invalid_>=5) {trip("GPU temperature unavailable for 5 consecutive checks",{});return;}
        if(valid(t) && over()) {trip("GPU rolling average reached the configured trigger",static_cast<int>(*average()+0.5));return;}
    }
    // Periodic re-verification waits out in-flight NVIDIA work, which cannot verify the GPU hold;
    // a new trip above still enforces immediately.
    if(tripped() && ++retry_>=5 && !defer_maintenance) enforce(true);
}
void ThermalProtection::check_before_apply() {
    if(!settings_.enabled || tripped() || enforcing_) return;
    auto t=temperature_(); if(valid(t)) add(*t);
    if(window_.empty()) {current_temperature_.reset();trip("GPU temperature unavailable before applying protected settings",{});}
    else if(over()) trip("GPU rolling average reached the configured trigger",static_cast<int>(*average()+0.5));
}
void ThermalProtection::trip(const std::string& reason,std::optional<int> t) {
    if(tripped()) return;
    state_.stage=config::Tripped;
    state_.held|=config::Gpu | ((settings_.reduce_cpu?config::Cpu:0)|(settings_.fan_boost?config::Fan:0))&supported_;
    config::Trip trip;trip.time=timestamp_now();trip.reason=reason;trip.temperature=t;trip.threshold=settings_.threshold;
    trip.actions=state_.held;trip.show_window=settings_.show_window;trip.description="Emergency EC requests; verification does not prove physical enforcement.";
    state_.last_trip=trip;persisted_=false;
    if(diagnostic_) diagnostic_("[Thermal] "+reason);
    enforce();
}
void ThermalProtection::enforce(bool maintenance) {
    if(!tripped() || enforcing_) return;
    enforcing_=true;retry_=0;
    int epoch=epoch_,previous=verified_;verified_=0;
    for(int action:{config::Gpu,config::Fan,config::Cpu}) if(state_.held&action) {
        std::string result;
        try {
            bool matches=false;
            if(maintenance && (previous&action) && epoch==epoch_) try { matches=matches_(action); } catch(...) {}
            if(!matches) apply_(action);
            if(epoch!=epoch_) throw std::runtime_error("EC reactivated during emergency apply; complete group must be retried");
            verified_|=action;errors_.erase(action);result="written and verified (EC request, not physical enforcement)";
        } catch(const std::exception& ex) {
            result=std::string("NOT VERIFIED: ")+ex.what();
            if(errors_[action]!=ex.what() && diagnostic_) diagnostic_("[Thermal] "+result);
            errors_[action]=ex.what();
        }
        const char* key=action==config::Gpu?"GPU":action==config::Cpu?"CPU":"fan boost";
        if(state_.last_trip && state_.last_trip->results[key]!=result) {
            state_.last_trip->results[key]=result;persisted_=false;
            if(diagnostic_) diagnostic_(std::string("[Thermal] ")+key+": "+result);
        }
    }
    // Every independent hardware action precedes persistence, including failures.
    if(!persisted_) try {store_.save_thermal(state_);persisted_=true;persistence_error_.clear();}
    catch(const std::exception& ex) {
        if(persistence_error_!=ex.what() && diagnostic_) diagnostic_(std::string("[Thermal] Persistence not verified: ")+ex.what());
        persistence_error_=ex.what();
    }
    if(was_complete_ && !complete()) ++lost_alerts_;
    was_complete_=complete();enforcing_=false;
}
void ThermalProtection::release() {
    if(!tripped()) return;
    auto next=state_;next.stage=config::Released;next.held=0;next.recovery_details.clear();
    try {store_.save_thermal(next);} catch(...) {persisted_=false;throw;}
    state_=std::move(next);persisted_=true;verified_=0;invalid_=0;was_complete_=false;peak_temperature_=current_temperature_;persistence_error_.clear();errors_.clear();
}
void ThermalProtection::reset() {
    if(!tripped()) return;
    if(settings_.enabled) {
        auto t=temperature_();if(valid(t)) add(*t);else current_temperature_.reset();
        // Reset authorizes normal power; unlike detecting a new trip, it may
        // not release a recovered hold on a hot one/two-sample startup mean.
        if(!valid(t) || *average()>=settings_.threshold) throw std::runtime_error("Reset refused: fresh GPU temperature unavailable or average at/above trigger");
    }
    release();
    if(!monitoring()) {window_.clear();current_temperature_.reset();}
}
void ThermalProtection::test() {
    if(!(supported_&config::Gpu) || tripped()) throw std::runtime_error("Thermal test unavailable");
    trip("Manual failsafe test (real emergency settings requested)",{});
}
std::string ThermalProtection::status() const {
    if(tripped()) return complete()?"TRIPPED - emergency settings written and verified":"PROTECTION INCOMPLETE - held actions or persistence not verified";
    if(!(supported_&config::Gpu)) return "Unavailable - GPU power/temperature support not validated";
    if(!settings_.enabled) return "Disabled";
    if(!current_temperature_) return "Temperature unavailable - "+std::to_string(invalid_)+"/5 failed checks";
    return "Armed - trips when the 5 s average reaches "+std::to_string(settings_.threshold)+" C";
}
}
