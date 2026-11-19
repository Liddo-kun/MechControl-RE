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
    return count(w.equations, "equation") + " and " + count(w.offsets, "offset") + " written";
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
    vf_curve_ = curve;
    if (!written.equations && !written.offsets) return;
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
        r->tuner = std::move(tuner);
    });
    r->tune = vf_tune_;
    return {.text = c.open ? "V/F curve tuner opened" : "V/F curve read", .vf = std::move(r)};
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
    vf_curve_ = c.curve ? *c.curve : nv::vf::Curve{r->gpu, {}, std::vector<int>(r->points.graphics.size(), 0)};
    vf_tune_ = c.tune;
    r->equations = written.equations;
    r->offsets = written.offsets;
    r->tune = vf_tune_;
    writes_started_ |= written.equations || written.offsets;
    auto text = std::string(c.curve ? "V/F curve applied: " : "Stock V/F curve restored: ") + written_text(written);
    diagnostic_(text);
    return {.text = text, .applied = true, .vf = std::move(r)};
}
}
