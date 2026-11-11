#include "ec.h"
#include <system_error>
#include <thread>
#include <algorithm>
#include <bit>

namespace mech {
namespace {
[[noreturn]] void io_error(const char* operation, DWORD error = GetLastError()) {
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}
class WindowsEcChannel final : public EcChannel {
public:
    WindowsEcChannel(std::wstring path, HANDLE stopping) : path_(std::move(path)), stopping_(stopping) { reopen(); }
    void reopen() override {
        device_.reset();
        if (stopped()) io_error("EC shutdown", ERROR_OPERATION_ABORTED);
        device_ = Handle(CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_EXISTING, 0, nullptr));
        if (!device_) io_error("Open EC driver");
    }
    uint32_t ioctl(uint32_t code, std::span<const int32_t> input) override {
        if (stopped()) io_error("EC shutdown", ERROR_OPERATION_ABORTED);
        uint32_t output = 0;
        DWORD returned = 0;
        BOOL ok = DeviceIoControl(device_.get(), code, const_cast<int32_t*>(input.data()),
                                  static_cast<DWORD>(input.size_bytes()), &output, sizeof output, &returned, nullptr);
        if (!ok) io_error("EC DeviceIoControl");
        // The serialized worker owns this synchronous call through completion.
        // Stop prevents subsequent calls; it does not cancel an in-flight driver call.
        return output; // Reference ignores bytesReturned, including the write acknowledgement.
    }

private:
    bool stopped() const { return stopping_ && WaitForSingleObject(stopping_, 0) == WAIT_OBJECT_0; }
    std::wstring path_;
    HANDLE stopping_;
    Handle device_;
};
void within(const config::MachineProfile& p, const char* name, int value) {
    const auto l = control_limit(p, name);
    if (value < l.min || value > l.max) throw std::out_of_range(std::string(name) + " exceeds model limits");
}
}
std::unique_ptr<EcChannel> open_ec_channel(const std::wstring& path, HANDLE stopping) {
    return std::make_unique<WindowsEcChannel>(path, stopping);
}
EcTransport::EcTransport(std::unique_ptr<EcChannel> channel, std::function<void(const std::string&)> diagnostic)
    : channel_(std::move(channel)), diagnostic_(std::move(diagnostic)) {}
uint32_t EcTransport::retry(uint32_t code, std::span<const int32_t> input) {
    try {
        return channel_->ioctl(code, input);
    } catch (const std::system_error& ex) {
        if (ex.code().value() == ERROR_OPERATION_ABORTED) throw;
        if (diagnostic_) diagnostic_(std::string("EC I/O failed; reopen and retry once: ") + ex.what());
        channel_->reopen();
        return channel_->ioctl(code, input);
    }
}
uint8_t EcTransport::read(int32_t address) { return static_cast<uint8_t>(retry(ecReadIoctl, {&address, 1})); }
void EcTransport::write(int32_t address, uint8_t value) {
    const int32_t input[]{address, value};
    retry(ecWriteIoctl, input);
}
uint32_t EcTransport::spec(int index) {
    const int32_t input = std::bit_cast<int32_t>(0xBBu | (static_cast<uint32_t>(index) << 8));
    return channel_->ioctl(ecSpecIoctl, {&input, 1}); // Reference does not reconnect SMAPC calls.
}
WriteGuard::WriteGuard(EcTransport& ec, const config::MachineProfile& profile, bool read_only,
                       std::function<void(unsigned)> wait)
    : ec_(ec), read_only_(read_only), allow_(profile.expanded_allowlist()), skip_(profile.expanded_skip_verify()),
      redirects_(profile.expanded_redirects()), wait_(std::move(wait)) {
    if (!wait_) wait_ = [](unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
}
void WriteGuard::admit(int32_t address) const {
    if (read_only_) throw std::runtime_error("EC diagnostic session is read-only");
    if (!allow_.contains(address))
        throw std::runtime_error("EC write address is not in the effective profile allowlist");
}
bool WriteGuard::verifies_same_address(int32_t a) const { return !skip_.contains(a) && !redirects_.contains(a); }
void WriteGuard::write_verify(int32_t address, uint8_t value) {
    ec_.write(address, value);
    if (skip_.contains(address)) return;
    if (auto i = redirects_.find(address); i != redirects_.end()) {
        uint8_t mask = (i->second.transform == "direct" || i->second.transform == "inverted_offset") ? 127 : 255;
        for (unsigned delay : {450, 200, 350, 500}) {
            wait_(delay);
            if ((ec_.read(config::hex(i->second.read_addr)) & mask) == (value & mask)) return;
        }
    } else {
        if (ec_.read(address) == value) return;
        ec_.write(address, value);
        if (ec_.read(address) == value) return;
    }
    throw VerifyError("EC write verification failed at " + std::to_string(address));
}
void WriteGuard::write(int32_t address, uint8_t value) {
    admit(address);
    try {
        write_verify(address, value);
    } catch (const VerifyError&) {
        if (recovering_ || !recover) throw;
        struct Scope {
            bool& value;
            Scope(bool& v) : value(v) { value = true; }
            ~Scope() { value = false; }
        } scope(recovering_);
        if (!recover()) throw;
        write_verify(address, value); // Exactly one recovery and no recursive catch.
    }
}
void WriteGuard::bitfield(int32_t address, uint8_t mask, uint8_t value) {
    admit(address);
    auto current = ec_.read(address);
    write(address, static_cast<uint8_t>((current & ~mask) | (value & mask)));
}
void WriteGuard::apply(std::span<const EcWrite> writes) {
    for (const auto& w : writes) admit(w.address);
    for (const auto& w : writes) {
        if (w.mask == 255)
            write(w.address, w.value);
        else
            bitfield(w.address, w.mask, w.value);
    }
}
config::Limit control_limit(const config::MachineProfile& p, const std::string& key) {
    if (auto i = p.limits.find(key); i != p.limits.end()) return i->second;
    // Controller per-key fallbacks, not UniversalDefaults' whole-map semantics.
    if (key == "cpuTempLimit") return {80, p.cpu_direct() ? 120 : 95, p.cpu_encoding(), p.tj_max(), {}};
    static const auto defaults = [] {
        config::MachineProfile p;
        config::apply_universal_defaults(p);
        return p;
    }();
    return defaults.limits.at(key);
}
ControlLimits control_limits(const config::MachineProfile& p) {
    return {control_limit(p, "PL1"),          control_limit(p, "PL2"), control_limit(p, "PL4"),
            control_limit(p, "cpuTempLimit"), control_limit(p, "TGP"), control_limit(p, "dynamicBoost")};
}
std::vector<EcWrite> cpu_writes(const config::MachineProfile& p, int pl1, int pl2, int pl4, int temperature) {
    using namespace config::encoding;
    within(p, "PL1", pl1);
    within(p, "PL2", pl2);
    within(p, "PL4", pl4);
    within(p, "cpuTempLimit", temperature);
    if (control_limit(p, "PL1").encoding != "direct" || control_limit(p, "PL2").encoding != "direct" ||
        (control_limit(p, "PL4").encoding != "direct" && control_limit(p, "PL4").encoding != "double"))
        throw std::out_of_range("CPU request cannot be represented by model encoding");
    auto raw_pl4 = config::encoding::pl4(pl4, p.pl4_doubled());
    within(p, "PL4", decode_pl4(raw_pl4, p.pl4_doubled())); // A doubled register rounds an odd request down.
    return {{p.reg("PL1"), config::encoding::pl1(pl1)},
            {p.reg("PL2"), config::encoding::pl1(pl2)},
            {p.reg("PL4"), raw_pl4},
            {p.reg("cpuTempLimitWrite"), cpu_temperature(temperature, p.cpu_direct(), p.tj_max())}};
}
std::vector<EcWrite> gpu_writes(const config::MachineProfile& p, int tgp, bool dynamic_boost, int db,
                                std::optional<int> maximum) {
    auto l = control_limit(p, "TGP");
    if (tgp < l.min || tgp > maximum.value_or(l.max)) throw std::out_of_range("GPU TGP exceeds model range");
    if (l.encoding != "offset") throw std::out_of_range("GPU request cannot be encoded");
    uint8_t boost = 0;
    if (dynamic_boost) {
        within(p, "dynamicBoost", db);
        if (control_limit(p, "dynamicBoost").encoding != "direct")
            throw std::out_of_range("DB request cannot be encoded");
        boost = config::encoding::dynamic_boost(db);
    }
    return {
        {p.reg("tgpDbCtrl"), static_cast<uint8_t>(dynamic_boost ? 7 : 5), p.bitfields.at("dynamicBoostEn").get_mask()},
        {p.reg("tgp"), config::encoding::tgp(tgp, p.tgp_offset())},
        {p.reg("dbTotalPower"), static_cast<uint8_t>(dynamic_boost ? 255 : 0)},
        {p.reg("dbMaxTgp"), boost}};
}
EcWrite fan_boost_write(const config::MachineProfile& p, bool enabled) {
    auto mask = p.bitfields.at("fanBoost").get_mask();
    return {p.reg("fanControlMode"), static_cast<uint8_t>(enabled ? mask : 0), mask};
}
std::vector<EcWrite> fan_curve_writes(const config::MachineProfile& p, const config::Curve& cpu,
                                      const std::optional<config::Curve>& gpu, bool enabled, uint8_t latch_mask) {
    const auto& ft = p.fan_table;
    if (ft.format != "RamFan1") throw std::runtime_error("Fan table format is not RamFan1");
    if (latch_mask != 4 && latch_mask != 255)
        throw std::runtime_error("Fan latch requires a qualified shared-bit or whole-byte mask");
    auto enable_mask = p.bitfields.at("fanCurveEnable").get_mask();
    std::vector<EcWrite> writes{{p.reg("fanCurveEnable"), static_cast<uint8_t>(enabled ? enable_mask : 0), enable_mask},
                                {0x7C6, 0, latch_mask}};
    auto curve = [&](const config::Curve& c, const std::string& up, const std::string& down, const std::string& duty) {
        auto n = c.temp_up.size();
        if (n == 0 || n > static_cast<size_t>(std::max(0, ft.entries)) || c.duty.size() != n)
            throw std::runtime_error("Invalid fan active-entry/duty count");
        for (size_t i = 0; i < n; ++i) {
            within(p, "fanTemp", c.temp_up[i]);
            within(p, "fanDuty", c.duty[i]);
            if (c.temp_up[i] < 0 || c.temp_up[i] > 253 || c.duty[i] < 0 || c.duty[i] > 200)
                throw std::out_of_range("Fan point cannot be represented");
        }
        if (ft.padding_temp < 0 || ft.padding_temp > 255) throw std::out_of_range("Fan padding must fit a byte");
        auto append = [&](const std::string& base, int index, int value) {
            writes.push_back({config::hex(base) + index, static_cast<uint8_t>(value)});
        };
        int active = static_cast<int>(n);
        for (int i = 0; i < ft.entries; ++i) append(up, i, i < active ? c.temp_up[i] : ft.padding_temp);
        // Recompute hysteresis; never trust saved/stale tempDown or write its index zero.
        for (int i = 1; i < active; ++i) append(down, i, std::max(0, c.temp_up[i - 1] - 4));
        if (active < ft.entries)
            append(down, active, std::min(255, (active > 1 ? std::max(0, c.temp_up[n - 2] - 4) : 0) + 4));
        for (int i = active + 1; i < ft.entries; ++i) append(down, i, ft.padding_temp);
        append(duty, 0, 0); // Required EC marker, regardless of supplied duty[0].
        int last = std::max(2, c.duty.back() / 2 * 2);
        for (int i = 1; i < ft.entries; ++i) append(duty, i, i < active ? std::max(2, c.duty[i] / 2 * 2) : last);
    };
    curve(cpu, ft.cpu_up, ft.cpu_down, ft.cpu_duty);
    if (ft.has_gpu && gpu) curve(*gpu, ft.gpu_up, ft.gpu_down, ft.gpu_duty);
    writes.push_back({0x7C6, 4, latch_mask});
    return writes;
}
}
