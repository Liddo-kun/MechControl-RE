// The V/F curve's driver commands. The tab does its calculation itself; here are the reads and writes, on the NVIDIA
// connection the other GPU features use and through the same blocking wait. A slot's curve is written by apply_vf,
// which needs vf_curve.* alone, so loading a slot never builds the tuner.
#include "controller.h"
#include "vf_tuner.h"

namespace mech {
nv::Api& Controller::vf_api() {
    if (nvidia_) return nvidia_->api();
    open_api(vf_available_, "No NVIDIA GPU on this machine", "V/F curve");
    return *api_;
}
// Fixtures keep the stock records beside their state.
std::filesystem::path Controller::vf_directory() const {
    return vf_directory_.empty() ? nv::vf::store_directory() : vf_directory_;
}
static std::string written_text(const nv::vf::Applied& w) {
    auto count = [](int n, const char* one) { return std::to_string(n) + " " + one + (n == 1 ? "" : "s"); };
    return count(w.equations, "equation") + (w.memory ? ", " : " and ") + count(w.offsets, "offset") +
           (w.memory ? " and the memory offset" : "") + " written";
}
// Call inside nvidia_work.
std::shared_ptr<VfReading> Controller::vf_read(nv::Api& api) {
    auto r = std::make_shared<VfReading>();
    auto info = nv::vf::equation_info(api);
    r->gpu = nv::vf::gpu(api.identity, info);
    r->records = nv::vf::records(api, info);
    r->points = nv::vf::points(api);
    return r;
}
// A curve already on the GPU writes nothing.
void Controller::apply_vf(const nv::vf::Curve& curve, const char* what) {
    auto& api = vf_api();
    nv::vf::Applied written;
    nvidia_work([&] { written = nv::vf::apply(api, curve, vf_directory()); });
    // Offsets that were neither 0 nor this curve's, and not those of another curve this run wrote: another tool's.
    if (written.replaced && (!vf_curve_ || *vf_curve_ == curve)) {
        vf_foreign_ = true;
        diagnostic_("Another tool's V/F curve offsets were on the GPU; the profile's curve replaces them");
    }
    vf_curve_ = curve;
    if (!written.equations && !written.offsets && !written.memory) return;
    writes_started_ = true;
    diagnostic_(std::string(what) + ": " + written_text(written));
}
ControlResult Controller::run(const VfRead& c) {
    auto& api = vf_api();
    auto directory = vf_directory();
    std::shared_ptr<VfReading> r;
    nvidia_work([&] {
        std::shared_ptr<const nv::vf::Tuner> tuner;
        if (c.open) tuner = std::make_shared<nv::vf::Tuner>(nv::vf::open(api, directory));
        r = vf_read(api);
        // Opened before any curve was written this run: a curve another tool left is found, for Save.
        if (tuner && !vf_curve_ && !vf_found_) try {
                auto now = nv::vf::current(api, directory);
                if (!now.records.empty() || std::ranges::any_of(now.offsets_khz, [](int o) { return o != 0; }) ||
                    now.memory_khz.value_or(0)) {
                    vf_found_ = std::move(now);
                    vf_tune_ = tuner->state(tuner->adopt(r->points));
                    diagnostic_("V/F curve found on the GPU (" + std::to_string(vf_found_->records.size()) +
                                " equations differ from stock); a profile Save stores it");
                }
            } catch (const std::exception& e) {
                diagnostic_(std::string("The V/F curve on the GPU cannot be stored: ") + e.what());
            }
        r->tuner = std::move(tuner);
    });
    r->found = vf_found_ && !vf_curve_;
    r->tune = vf_tune_;
    return {.text = c.open ? "V/F curve tuner opened" : "V/F curve read", .vf = std::move(r)};
}
// Once, when the GPU status first has the driver open: offsets that no curve of this run wrote (another tool's).
bool Controller::vf_overclock() {
    if (!vf_looked_ && !vf_curve_ && vf_available_ && (nvidia_ || api_) && !nvidia_busy_ && !gpu_driver_uncertain_ &&
        gpu_awake()) {
        vf_looked_ = true;
        try {
            auto& api = vf_api();
            nv::vf::Points p;
            nvidia_work([&] { p = nv::vf::points(api); });
            auto set = [](const nv::vf::Point& x) { return x.offset_khz != 0; };
            vf_seen_ = std::ranges::any_of(p.graphics, set) ||
                       std::ranges::any_of(p.memory, [&](auto& x) { return x.programmable && set(x); });
            if (vf_seen_) diagnostic_("V/F curve offsets found on the GPU that MechControl did not write");
        } catch (const std::exception& e) {
            diagnostic_(std::string("V/F curve offsets not read: ") + e.what());
        }
    }
    return vf_seen_ && !vf_curve_;
}
ControlResult Controller::run(const VfApply& c) {
    auto& api = vf_api();
    auto directory = vf_directory();
    nv::vf::Applied written;
    std::shared_ptr<VfReading> r;
    nvidia_work([&] {
        written = c.curve ? nv::vf::apply(api, *c.curve, directory) : nv::vf::restore(api, directory);
        r = vf_read(api);
    });
    // Stock is kept as a curve without records, so a slot saved now puts stock back when loaded.
    vf_curve_ = c.curve ? *c.curve : nv::vf::Curve{r->gpu, {}, std::vector<int>(r->points.graphics.size(), 0), 0};
    vf_tune_ = c.tune;
    r->equations = written.equations;
    r->offsets = written.offsets;
    r->memory = written.memory;
    r->tune = vf_tune_;
    writes_started_ |= written.equations || written.offsets || written.memory;
    auto text = std::string(c.curve ? "V/F curve applied: " : "Stock V/F curve restored: ") + written_text(written);
    diagnostic_(text);
    return {.text = text, .applied = true, .vf = std::move(r)};
}
ControlResult Controller::run(const VfForget&) {
    if (!saved_profile_ || !saved_profile_->gpu) throw std::runtime_error("No profile with GPU settings is loaded");
    saved_profile_->gpu->vf_curve = nullptr;
    store_.save(active_slot_, *saved_profile_);
    vf_curve_.reset();
    vf_found_.reset();
    vf_tune_ = nullptr;
    diagnostic_("V/F curve removed from slot " + std::to_string(active_slot_));
    return {.text = "V/F curve removed from the profile; the GPU keeps the curve it has now", .saved = true};
}
ControlResult Controller::run(const VfIntro& c) {
    store_.set_vf_intro(c.open);
    return {};
}
}
