#pragma once
#include "settings.h"
#include <deque>

namespace mech {
// Serialized by the EC owner. Saved history never supplies current verification.
class ThermalProtection {
public:
    ThermalProtection(config::ProfileStore&, int supported, std::function<void(int)> apply,
                      std::function<bool(int)> matches, std::function<std::optional<int>()> temperature,
                      std::function<void(const std::string&)> diagnostic);
    void configure(const config::ProtectionSettings&);
    void validate(const config::ProtectionSettings&) const;
    void sample(std::optional<int>, bool defer_maintenance = false);
    void check_before_apply();
    void enforce(bool maintenance = false);
    void release();
    void reset();
    void test();
    void invalidate() {
        ++epoch_;
        verified_ = 0;
    }
    bool tripped() const { return state_.stage == config::Tripped; }
    bool holds(int action) const { return tripped() && (state_.held & action) != 0; }
    bool verified(int action) const { return (verified_ & action) == action; }
    bool monitoring() const { return settings_.enabled || tripped(); }
    int supported_actions() const { return supported_; }
    bool complete() const { return tripped() && persisted_ && (verified_ & state_.held) == state_.held; }
    // Counts enforcements that found previously complete protection incomplete, as the source re-alerts.
    // invalidate() alone does not count; the next enforcement decides.
    int lost_alerts() const { return lost_alerts_; }
    const config::ThermalState& state() const { return state_; }
    const config::ProtectionSettings& settings() const { return settings_; }
    std::optional<double> average() const;
    std::optional<int> temperature() const { return current_temperature_; }
    std::optional<int> peak_temperature() const { return peak_temperature_; }
    int invalid_samples() const { return invalid_; }
    std::string status() const;

private:
    void add(int);
    bool over() const;
    void trip(const std::string&, std::optional<int>);
    config::ProfileStore& store_;
    int supported_, verified_ = 0, epoch_ = 0, retry_ = 0, invalid_ = 0, lost_alerts_ = 0;
    bool persisted_ = false, enforcing_ = false, was_complete_ = false;
    config::ThermalState state_;
    config::ProtectionSettings settings_;
    std::deque<int> window_;
    std::optional<int> current_temperature_, peak_temperature_;
    std::function<void(int)> apply_;
    std::function<bool(int)> matches_;
    std::function<std::optional<int>()> temperature_;
    std::function<void(const std::string&)> diagnostic_;
    std::string persistence_error_;
    std::map<int, std::string> errors_;
};
std::string timestamp_now();
}
