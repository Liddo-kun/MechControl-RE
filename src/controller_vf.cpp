// The V/F Curve tab's driver commands. The tab does its calculation itself; here are the reads and writes, on the
// NVIDIA connection the other GPU features use and through the same blocking wait.
#include "controller.h"
#include "vf_tuner.h"

namespace mech {
nv::Api& Controller::vf_api() {
    if (nvidia_) return nvidia_->api();
    open_api(vf_available_, "No NVIDIA GPU on this machine", "V/F curve");
    return *api_;
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
ControlResult Controller::run(const VfRead& c) {
    auto& api = vf_api();
    // Fixtures keep the stock records beside their state.
    auto directory = vf_directory_.empty() ? nv::vf::store_directory() : vf_directory_;
    std::shared_ptr<VfReading> r;
    nvidia_work([&] {
        std::shared_ptr<const nv::vf::Tuner> tuner;
        if (c.open) tuner = std::make_shared<nv::vf::Tuner>(nv::vf::open(api, directory));
        r = vf_read(api);
        r->tuner = std::move(tuner);
    });
    return {.text = c.open ? "V/F curve tuner opened" : "V/F curve read", .vf = std::move(r)};
}
ControlResult Controller::run(const VfApply& c) {
    auto& api = vf_api();
    auto directory = vf_directory_.empty() ? nv::vf::store_directory() : vf_directory_;
    nv::vf::Applied written;
    std::shared_ptr<VfReading> r;
    nvidia_work([&] {
        written = c.curve ? nv::vf::apply(api, *c.curve, directory) : nv::vf::restore(api, directory);
        r = vf_read(api);
    });
    r->equations = written.equations;
    r->offsets = written.offsets;
    writes_started_ |= written.equations || written.offsets;
    auto count = [](int n, const char* one) { return std::to_string(n) + " " + one + (n == 1 ? "" : "s"); };
    auto text = std::string(c.curve ? "V/F curve applied: " : "Stock V/F curve restored: ") +
                count(written.equations, "equation") + " and " + count(written.offsets, "offset") + " written";
    diagnostic_(text);
    return {.text = text, .applied = true, .vf = std::move(r)};
}
}
