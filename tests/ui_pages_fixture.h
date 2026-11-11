#pragma once
#include <atomic>
#include <fstream>

// These owners exist only in ui-pages-check. The production pages, commands and
// serialized controller are unchanged; EC/NVIDIA failures are explicitly injected.
struct UiPageState {
    std::map<int, uint8_t> bytes;
    std::atomic<bool> fail_gpu = false, asleep = false;
    std::atomic<int> budgets = 0;
    UiPageState() {
        for (auto [a, v] : std::initializer_list<std::pair<int, int>>{
                 {0x740, 26},  {0x741, 1},    {0x727, 128}, {0x7D2, 1},  {0x783, 100}, {0x784, 150},  {0x785, 100},
                 {0x463, 10},  {0x743, 7},    {0x744, 45},  {0x746, 15}, {0x745, 255}, {0x7A6, 32},   {0x751, 144},
                 {0x7C5, 224}, {0x7C6, 4},    {0x43E, 40},  {0x44F, 40}, {0x464, 4},   {0x465, 42},   {0x46B, 20},
                 {0x46C, 4},   {0x454, 0x4D}, {0x47D, 0},   {0x6E6, 1},  {0x490, 1},   {0x7A4, 0x80}, {0x7C7, 0x0C}})
            bytes[a] = uint8_t(v); // EC version 77.0 and cooler support: the advanced EC controls.
        for (int i = 0; i < 16; ++i)
            for (int base : {0xF00, 0xF30}) {
                bytes[base + i] = uint8_t(i < 10 ? 19 + i * 8 : 255);
                bytes[base + 16 + i] = uint8_t(i ? i < 10 ? 11 + i * 8 : 255 : 0);
                bytes[base + 32 + i] = uint8_t(i < 10 ? i * 20 : 180);
            }
    }
};
class UiPageEc final : public mech::EcChannel {
    std::shared_ptr<UiPageState> state_;

public:
    explicit UiPageEc(std::shared_ptr<UiPageState> s) : state_(std::move(s)) {}
    uint32_t ioctl(uint32_t code, std::span<const int32_t> words) override {
        int a = words[0];
        if (code == mech::ecSpecIoctl) {
            static constexpr uint32_t spec[]{200, 200, 200, 10, 55, 25, 0, 95, 0, 0xBB, 0, 0};
            return spec[a >> 8];
        }
        if (code == mech::ecReadIoctl) {
            if (a == 0x744 && state_->fail_gpu) throw std::runtime_error("Injected GPU EC read failure");
            return state_->bytes[a];
        }
        state_->bytes[a] = uint8_t(words[1]);
        if (a == 0x741 && words[1] == 5) state_->bytes[a] = 1;
        if (a == 0x786) state_->bytes[0x463] = uint8_t(words[1] & 127);
        return 0;
    }
    void reopen() override {}
};
class UiPageGpu final : public mech::nv::Power {
    std::shared_ptr<UiPageState> state_;
    int requested_ = 95;

public:
    explicit UiPageGpu(std::shared_ptr<UiPageState> s) : state_(std::move(s)) { enabled = true; }
    int stock_watts() const override { return 175; }
    int minimum_watts() const override { return 5; }
    bool awake() const override { return !state_->asleep; }
    mech::nv::PowerLimits limits() override {
        return {5000, 95000, 250000, uint32_t(requested_ * 1000), uint32_t(requested_ * 1000)};
    }
    mech::nv::Currents currents() override {
        return {{1000, 120000, 168000, 120000, 120000},
                mech::nv::Rail{1000, 30000, 42000, 30000, 30000},
                mech::nv::Rail{1000, 11053, 15474, 11053, 11053}};
    }
    // Loaded readings that span the live table's green-to-red tints.
    mech::nv::Telemetry telemetry() override {
        mech::nv::Telemetry t;
        t.nv = 110400;
        t.ms = 21300;
        t.input = 8150;
        t.nv_limit = 120000;
        t.ms_limit = 30000;
        t.input_limit = 11053;
        t.current_khz = 2730000;
        t.power_khz = 2655000;
        t.ceiling_khz = 3090000;
        t.clock_mhz = 2655;
        t.enforced_mw = uint32_t(requested_ * 1000);
        t.gpu_mw = uint32_t(requested_ * 960);
        t.core_mw = t.gpu_mw.value() * 7 / 10;
        t.ipc = 0.61;
        return t;
    }
    mech::nv::Budget budget() override {
        ++state_->budgets;
        return {250000, 350000};
    }
    std::optional<mech::nv::Dlppc> dlppc() override { return mech::nv::Dlppc{true, 7, 2048}; }
    bool needs_restart(bool, const mech::config::GpuSettings&) override { return false; }
    void configure(bool unlock, const mech::config::GpuSettings&, bool) override { enabled = unlock; }
    void apply(int watts, bool) override { requested_ = watts; }
    bool at_most(int watts) override { return requested_ <= watts; }
    bool applied(int watts) override { return requested_ == watts; }
    bool restore_currents() override { return false; }
};
inline mech::WorkerOptions ui_page_options(const std::filesystem::path& root, std::shared_ptr<UiPageState> state) {
    mech::WorkerOptions options;
    options.identity = mech::config::Identity{"X6AR55xY", "UI fixture", "intel", uint8_t{26}};
    options.state_directory = root;
    options.nvidia_hardware = false;
    options.peripheral_hardware = false;
    options.wait = [](unsigned) {};
    options.channel_factory = [state](const std::wstring&, HANDLE) { return std::make_unique<UiPageEc>(state); };
    options.nvidia_factory = [state] { return std::make_unique<UiPageGpu>(state); };
    options.lighting_send = [](int, const mech::HidReport&) {};
    return options;
}
