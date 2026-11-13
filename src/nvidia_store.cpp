#include "nvidia.h"
#include "config_text.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <shlobj.h>
#include <knownfolders.h>

namespace mech::nv {
static std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}
static void wincheck(LSTATUS s) {
    if (s != ERROR_SUCCESS) throw std::system_error(s, std::system_category(), "NVIDIA registry");
}
static void external() {
    throw std::runtime_error(
        "External NVIDIA ROM overrides exist; restore them with their owning tool and restart the GPU first. MechControl will not take ownership.");
}
static constexpr GUID display = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0, 0x2b, 0xe1, 3, 0x18}};
struct Devices {
    HDEVINFO set = SetupDiGetClassDevsW(&display, L"PCI", nullptr, DIGCF_PRESENT);
    ~Devices() {
        if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    }
};
template <class F> static bool adapters(F f) {
    Devices d;
    if (d.set == INVALID_HANDLE_VALUE)
        throw std::system_error(GetLastError(), std::system_category(), "NVIDIA display adapters");
    for (DWORD i = 0;; ++i) {
        SP_DEVINFO_DATA info{};
        info.cbSize = sizeof(info);
        if (!SetupDiEnumDeviceInfo(d.set, i, &info)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) break;
            throw std::system_error(GetLastError(), std::system_category(), "NVIDIA device enumeration");
        }
        wchar_t hardware[2048]{};
        if (SetupDiGetDeviceRegistryPropertyW(d.set, &info, SPDRP_HARDWAREID, nullptr,
                                              reinterpret_cast<BYTE*>(hardware), sizeof(hardware), nullptr) &&
            f(d.set, info, std::wstring(hardware)))
            return true;
    }
    return false;
}
Store::Store(HKEY key, std::filesystem::path path) : key_(key), path_(std::move(path)) {}
Store::Store(const Identity& id) : path_(recovery_path()) {
    if (id.domain != 0) throw std::runtime_error("NVIDIA overrides do not support this PCI domain");
    wchar_t prefix[128];
    swprintf_s(prefix, L"PCI\\VEN_10DE&DEV_%04X&SUBSYS_%08X", id.device, id.subsystem);
    bool found = adapters([&](HDEVINFO set, SP_DEVINFO_DATA info, const std::wstring& hardware) {
        auto hw = config::utf8(hardware), p = config::utf8(prefix);
        if (!config::equal_ignore_case(hw, p) &&
            !(hw.size() > p.size() && hw[p.size()] == '&' && config::equal_ignore_case(hw.substr(0, p.size()), p)))
            return false;
        auto number = [&](DWORD prop) -> std::optional<DWORD> {
            DWORD value = 0, size = 0;
            if (!SetupDiGetDeviceRegistryPropertyW(set, &info, prop, nullptr, reinterpret_cast<BYTE*>(&value), 4,
                                                   &size) ||
                size != 4)
                return {};
            return value;
        };
        if (number(SPDRP_BUSNUMBER) != id.bus || number(SPDRP_ADDRESS) != ((id.slot << 16) | id.function)) return false;
        wchar_t instance_id[512]{};
        if (!SetupDiGetDeviceInstanceIdW(set, &info, instance_id, 512, nullptr))
            throw std::system_error(GetLastError(), std::system_category(), "NVIDIA instance ID");
        auto key = SetupDiOpenDevRegKey(set, &info, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ | KEY_WRITE);
        if (key == INVALID_HANDLE_VALUE)
            throw std::system_error(GetLastError(), std::system_category(), "NVIDIA driver key");
        key_ = key;
        instance = instance_id;
        return true;
    });
    if (!found) throw std::runtime_error("No present NVIDIA PnP adapter matches the selected NVAPI GPU");
}
Store::~Store() {
    if (key_) RegCloseKey(key_);
}
bool Store::present() {
    try {
        return adapters([](HDEVINFO, SP_DEVINFO_DATA, const std::wstring& hw) {
            return hw.size() >= 13 && _wcsnicmp(hw.c_str(), L"PCI\\VEN_10DE&", 13) == 0;
        });
    } catch (const std::system_error&) {
        return false;
    }
}
std::filesystem::path Store::recovery_path() {
    PWSTR p = nullptr;
    auto hr = SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &p);
    if (FAILED(hr)) throw std::runtime_error("Cannot locate ProgramData");
    std::filesystem::path path(p);
    CoTaskMemFree(p);
    return path / L"MechControl/NvidiaOverrides/originals.txt";
}
bool Store::saved() { return std::filesystem::exists(recovery_path()); }
bool Store::awake() const { return power_state(instance) == 0; }
int Store::power_state(const std::wstring& instance) {
    DEVINST node = 0;
    if (instance.empty() || CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(instance.c_str()), 0) != CR_SUCCESS)
        return -1;
    std::array<uint8_t, 64> data{};
    ULONG size = static_cast<ULONG>(data.size());
    DEVPROPTYPE type = 0;
    // CM_POWER_DATA.PD_MostRecentPowerState: PowerDeviceD0 = 1.
    if (CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_PowerData, &type, data.data(), &size, 0) != CR_SUCCESS ||
        size < 8)
        return -1;
    auto state = u32(data, 4);
    return state >= 1 && state <= 4 ? int(state) - 1 : -1;
}
std::map<std::string, Bytes> Store::records() {
    std::map<std::string, Bytes> result;
    for (DWORD i = 0;; ++i) {
        wchar_t name[16384]{};
        DWORD count = 16384, size = 0, type = 0;
        auto status = RegEnumValueW(key_, i, name, &count, nullptr, &type, nullptr, &size);
        if (status == ERROR_NO_MORE_ITEMS) break;
        wincheck(status);
        auto name_lower = lower(config::utf8(name));
        if (name_lower == "vbios") external();
        if (!name_lower.starts_with("romoverride")) continue;
        if (name_lower.size() != 13 || !isxdigit(static_cast<unsigned char>(name_lower[11])) ||
            !isxdigit(static_cast<unsigned char>(name_lower[12])) || type != REG_BINARY)
            external();
        Bytes value(size);
        wincheck(RegQueryValueExW(key_, name, nullptr, &type, value.data(), &size));
        if (type != REG_BINARY) external();
        value.resize(size);
        result.emplace(name_lower, std::move(value));
    }
    return result;
}
std::optional<Store::Recovery> Store::recovery() {
    if (!std::filesystem::exists(path_)) return {};
    // The version line, then "offset=value" lines. CRLF line ends are accepted.
    std::istringstream file(config::read_text(path_));
    Recovery r;
    std::string line;
    auto next = [&] {
        if (!std::getline(file, line)) return false;
        if (line.ends_with('\r')) line.pop_back();
        return true;
    };
    if (!next()) throw std::runtime_error("Empty NVIDIA originals");
    r.version = line;
    auto number = [](std::string_view text) {
        uint32_t n = 0;
        auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), n);
        if (error != std::errc{} || end != text.data() + text.size())
            throw std::runtime_error("Malformed NVIDIA originals");
        return n;
    };
    while (next()) {
        auto equal = line.find('=');
        if (equal == std::string::npos) throw std::runtime_error("Malformed NVIDIA originals");
        auto offset = number(std::string_view(line).substr(0, equal));
        auto value = number(std::string_view(line).substr(equal + 1));
        if (offset > INT_MAX || !r.originals.emplace(static_cast<int>(offset), value).second)
            throw std::runtime_error("Malformed NVIDIA originals");
    }
    return r;
}
bool Store::owned(const Bytes& b, const Recovery& r) {
    if (b.size() != 14 || b[9] != 4) return false;
    Bios ver;
    std::copy_n(b.begin(), 5, ver.version_bytes.begin());
    return ver.version() == r.version && r.originals.contains(static_cast<int32_t>(u32(b, 5)));
}
void Store::require_owned(const std::map<std::string, Bytes>& current, const std::optional<Recovery>& r) {
    for (auto& [name, b] : current) {
        if (!r || !owned(b, *r)) external();
    }
}
Bios Store::open(const Bytes& image, bool& staged, bool& discarded) {
    auto active = Bios::parse(image);
    auto current = records();
    auto r = recovery();
    discarded = false;
    if (r) {
        bool stock = current.empty();
        if (stock)
            for (auto [p, v] : r->originals)
                if (u32(image, p) != v) {
                    stock = false;
                    break;
                }
        discarded = r->version != active.version() || stock;
        if (discarded) {
            for (auto& [name, b] : current)
                if (owned(b, *r)) wincheck(RegDeleteValueW(key_, config::wide(name).c_str()));
            std::filesystem::remove(path_);
            current = records();
            r = {};
        }
    }
    require_owned(current, r);
    staged = !current.empty();
    if (!r) return active;
    auto original = image;
    for (auto [p, v] : r->originals) put(original, p, v);
    return Bios::parse(std::move(original));
}
bool Store::matches(const std::vector<Record>& desired) {
    auto current = records();
    if (current.size() != desired.size()) return false;
    for (auto& r : desired) {
        auto it = current.find(lower(r.name));
        if (it == current.end() || it->second != r.data) return false;
    }
    return true;
}
void Store::stage(const Bios& original, const std::vector<Record>& desired) {
    modified = false;
    auto current = records();
    auto r = recovery();
    require_owned(current, r);
    if (!desired.empty()) {
        auto complete = r.value_or(Recovery{original.version(), {}});
        auto before = complete.originals.size();
        for (int p : original.offsets()) complete.originals.try_emplace(p, u32(original.image, p));
        if (!r || before != complete.originals.size()) {
            std::filesystem::create_directories(path_.parent_path());
            std::ostringstream text;
            text << complete.version;
            for (auto [p, v] : complete.originals) text << '\n' << p << '=' << v;
            config::write_text(path_, text.str(), true);
        }
    }
    for (auto& [name, b] : current)
        if (std::none_of(desired.begin(), desired.end(),
                         [&](auto& v) { return config::equal_ignore_case(v.name, name); })) {
            modified = true;
            wincheck(RegDeleteValueW(key_, config::wide(name).c_str()));
        }
    for (auto& r2 : desired) {
        auto old = current.find(lower(r2.name));
        if (old != current.end() && old->second == r2.data) continue;
        modified = true;
        wincheck(RegSetValueExW(key_, config::wide(r2.name).c_str(), 0, REG_BINARY, r2.data.data(),
                                static_cast<DWORD>(r2.data.size())));
    }
    wincheck(RegFlushKey(key_));
    if (!matches(desired)) throw std::runtime_error("NVIDIA override registry readback failed");
}
void Store::delete_recovery() {
    require_owned(records(), {});
    std::filesystem::remove(path_);
}
}
