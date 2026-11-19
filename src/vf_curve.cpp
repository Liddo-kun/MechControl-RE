#include "vf_curve.h"
#include <algorithm>
#include <cstring>
#include <shlobj.h>
#include <knownfolders.h>
#include <tlhelp32.h>

namespace mech::nv::vf {
namespace {
// NVAPI functions and packet version words that driver 621.24 accepts (found in nvapi64.dll; VFCURVE notes.md).
constexpr uint32_t equation_info_call = 0x8d49471c, equation_get_call = 0x4c75c9fe, equation_set_call = 0x68b798c4;
constexpr uint32_t equation_info_version = 0xd8444, equation_info_bytes = 0x98444;
constexpr uint32_t equation_records_version = 0x158444, equation_records_bytes = 0x158444;
constexpr uint32_t variable_info_call = 0xb9da41d6, variable_get_call = 0x5d387298;
constexpr uint32_t variable_info_version = 0x15f424, variable_info_bytes = 0x10f424;
constexpr uint32_t variable_records_version = 0x3abbc, variable_records_bytes = 0xabbc;
constexpr uint32_t domains_call = 0x64b43a6a, points_call = 0x507b4b59, status_call = 0x21537ad4,
                   offsets_get_call = 0x23f1b133, offsets_set_call = 0x0733e009;
constexpr size_t mask_bytes = 1024;
constexpr uint32_t version(uint32_t size, uint32_t v) { return size | v << 16; }

void call(Api& api, uint32_t id, Bytes& b, const std::string& what) {
    try {
        api.call(id, b);
    } catch (const std::exception& e) {
        throw std::runtime_error(what + ": " + e.what() +
                                 ". MechControl knows the V/F curve packets of NVIDIA driver 621.24.");
    }
}
uint64_t hash(const Bytes& b) { // FNV-1a
    uint64_t h = 14695981039346656037ull;
    for (auto c : b) h = (h ^ c) * 1099511628211ull;
    return h;
}
std::string hex(std::span<const uint8_t> b) {
    static const char digits[] = "0123456789abcdef";
    std::string s;
    for (auto c : b) {
        s += digits[c >> 4];
        s += digits[c & 15];
    }
    return s;
}
Bytes unhex(const std::string& s) {
    auto digit = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::runtime_error("Malformed V/F curve record");
    };
    if (s.size() % 2) throw std::runtime_error("Malformed V/F curve record");
    Bytes b(s.size() / 2);
    for (size_t i = 0; i < b.size(); ++i) b[i] = uint8_t(digit(s[2 * i]) << 4 | digit(s[2 * i + 1]));
    return b;
}
std::string list(const std::vector<size_t>& v) {
    std::string s;
    for (auto i : v) s += (s.empty() ? "" : ", ") + std::to_string(i);
    return s;
}

// The graphics points with the offsets packet that a write sends back.
struct Clock {
    Points points;
    std::vector<int> index; // the driver's point index of each graphics point
    Bytes control;
    uint32_t control_version = 0;
};
// Reads with the first packet version the driver accepts (status 3, 2, 1; offsets 2, 1), as vfctl does.
Bytes first_accepted(Api& api, uint32_t id, std::initializer_list<std::pair<uint32_t, uint32_t>> versions,
                     const Bytes& mask_from, const char* what, uint32_t& used) {
    std::string error;
    for (auto [v, size] : versions) {
        auto b = packet(size, version(size, v));
        std::copy_n(mask_from.begin() + 4, 32, b.begin() + 4);
        try {
            api.call(id, b);
            used = v;
            return b;
        } catch (const std::exception& e) {
            error = e.what();
        }
    }
    throw std::runtime_error(std::string(what) + ": " + error);
}
Clock read_clock(Api& api) {
    auto domains = packet(0x928, version(0x928, 1));
    call(api, domains_call, domains, "Reading the clock domains failed");
    std::optional<size_t> graphics;
    for (int i = 0; i < 32; ++i) {
        size_t p = 0x28 + i * 0x48;
        // Type 0 is programmable; clock 0 is graphics.
        if ((u32(domains, 4) & (1u << i)) && u32(domains, p) == 0 && u32(domains, p + 4) == 0) graphics = p;
    }
    if (!graphics) throw std::runtime_error("The driver lists no programmable graphics clock");
    auto info = packet(0x182c, version(0x182c, 1));
    call(api, points_call, info, "Reading the curve points failed");
    uint32_t status_version = 0, control_version = 0;
    auto status = first_accepted(api, status_call, {{3, 0x15b0c}, {2, 0x1c28}, {1, 0x1c28}}, info,
                                 "Reading the curve failed", status_version);
    auto control = first_accepted(api, offsets_get_call, {{2, 0x2420}, {1, 0x2420}}, info,
                                  "Reading the curve offsets failed", control_version);
    Clock c{{{}, int32_t(u32(domains, *graphics + 0x2c)), int32_t(u32(domains, *graphics + 0x28))},
            {},
            std::move(control),
            control_version};
    bool base = status_version == 3 && status[0x24];
    for (int i = domains[*graphics + 0x30]; i <= domains[*graphics + 0x31] && i < 255; ++i) {
        if (!(u32(info, 4 + i / 32 * 4) & (1u << (i % 32)))) continue;
        size_t s = status_version == 3 ? 0x68 + i * 0x15c : 0x44 + i * 0x1c;
        Point p;
        p.final_khz = u32(status, s + 4);
        p.uv = u32(status, s + 8);
        if (base) p.base_khz = u32(status, s + 0xc);
        p.offset_khz = int32_t(u32(c.control, 0x44 + i * 0x24 + 0x14));
        p.programmable = u32(info, 0x44 + i * 0x18) == 0;
        c.points.graphics.push_back(p);
        c.index.push_back(i);
    }
    if (c.points.graphics.empty()) throw std::runtime_error("The driver lists no graphics curve points");
    return c;
}
// The points whose offset changes; refuses offsets the driver does not take.
std::vector<size_t> changes(const Clock& c, const std::vector<int>& khz) {
    auto& g = c.points.graphics;
    if (khz.size() != g.size())
        throw std::runtime_error("The curve has " + std::to_string(khz.size()) +
                                 " point offsets, but the driver lists " + std::to_string(g.size()) +
                                 " graphics points");
    std::vector<size_t> out;
    for (size_t k = 0; k < g.size(); ++k) {
        if (khz[k] < c.points.offset_min_khz || khz[k] > c.points.offset_max_khz)
            throw std::runtime_error("Point " + std::to_string(k) + ": offset " + std::to_string(khz[k]) +
                                     " kHz is outside the driver's limits");
        if (khz[k] == g[k].offset_khz) continue;
        if (!g[k].programmable)
            throw std::runtime_error("Point " + std::to_string(k) + " is a fixed point and takes no offset");
        out.push_back(k);
    }
    return out;
}
bool afterburner_running() {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{sizeof e};
    bool found = false;
    for (BOOL ok = Process32FirstW(s, &e); ok && !found; ok = Process32NextW(s, &e))
        found = !_wcsicmp(e.szExeFile, L"MSIAfterburner.exe");
    CloseHandle(s);
    return found;
}
void refuse_afterburner() {
    if (afterburner_running())
        throw std::runtime_error(
            "MSI Afterburner is running and keeps its own copy of the curve offsets. Close it first; nothing was written.");
}
// One SetControl per equation, each on a read of that equation alone (the only form tested), then a full read back.
void write_equations(Api& api, const Bytes& info, const Bytes& live, const std::map<size_t, Bytes>& want) {
    size_t done = 0;
    auto stopped = [&] {
        return done
                   ? "; the equations written before it stay written, and Restore stock in the V/F curve tab undoes them"
                   : "; nothing was written";
    };
    for (auto& [i, r] : want) {
        auto one = packet(equation_records_bytes, equation_records_version);
        one[4 + i / 8] = uint8_t(1u << (i % 8));
        call(api, equation_get_call, one, std::string("Reading V/F equation ") + std::to_string(i) + " failed");
        if (!std::ranges::equal(record(one, i), record(live, i)))
            throw std::runtime_error("V/F equation " + std::to_string(i) + " changed while it was written" + stopped());
        std::ranges::copy(r, one.begin() + equation_header + i * record_size);
        try {
            api.call(equation_set_call, one);
        } catch (const std::exception& e) {
            throw std::runtime_error("The driver refused V/F equation " + std::to_string(i) + " (" + e.what() + ")" +
                                     stopped());
        }
        ++done;
    }
    auto after = records(api, info);
    std::vector<size_t> bad;
    for (size_t i = 0; i < equation_count(info); ++i) {
        auto w = want.find(i);
        if (!std::ranges::equal(record(after, i),
                                w != want.end() ? std::span<const uint8_t>(w->second) : record(live, i)))
            bad.push_back(i);
    }
    if (!bad.empty()) throw std::runtime_error("V/F equations " + list(bad) + " did not read back as written");
}
Applied write(Api& api, const Bytes& info, const Bytes& stock, const std::map<int, Bytes>& records,
              const std::vector<int>& offsets) {
    auto n = equation_count(info);
    for (auto& [i, r] : records)
        if (i < 0 || size_t(i) >= n || r.size() != record_size) throw std::runtime_error("Malformed V/F curve record");
    // Every check before the first write.
    if (!changes(read_clock(api), offsets).empty()) refuse_afterburner();
    auto live = vf::records(api, info);
    std::map<size_t, Bytes> want;
    for (size_t i = 0; i < n; ++i) {
        auto r = records.find(int(i));
        auto w = r != records.end() ? std::span<const uint8_t>(r->second) : record(stock, i);
        if (!std::ranges::equal(record(live, i), w)) want.emplace(i, Bytes(w.begin(), w.end()));
    }
    write_equations(api, info, live, want);
    return {int(want.size()), set_offsets(api, offsets)};
}
std::optional<Bytes> stock_for(Api& api, const Bytes& info, Gpu& now, const std::filesystem::path& directory) {
    now = gpu(api.identity, info);
    return load_stock(directory, now, info);
}
const char* no_stock =
    "The stock V/F equations of this GPU are not recorded. Open the V/F curve tab once to record them";
}

config::Json to_json(const Curve& c) {
    char device[8], equations[20];
    sprintf_s(device, "%04X", c.gpu.device);
    sprintf_s(equations, "%016llX", static_cast<unsigned long long>(c.gpu.equations));
    auto records = config::Json::object();
    for (auto& [i, r] : c.records) records[std::to_string(i)] = hex(r);
    return {{"gpu", {{"device", device}, {"vbios", c.gpu.vbios}, {"driver", c.gpu.driver}, {"equations", equations}}},
            {"records", std::move(records)},
            {"offsetsKhz", c.offsets_khz}};
}
Curve curve_from_json(const config::Json& j) {
    Curve c;
    auto& g = j.at("gpu");
    auto number = [](const std::string& s, int digits) {
        if (s.size() != size_t(digits) || s.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            throw std::runtime_error("Malformed V/F curve GPU");
        return std::stoull(s, nullptr, 16);
    };
    c.gpu.device = uint16_t(number(g.at("device").get<std::string>(), 4));
    c.gpu.vbios = g.at("vbios").get<std::string>();
    c.gpu.driver = g.at("driver").get<std::string>();
    c.gpu.equations = number(g.at("equations").get<std::string>(), 16);
    config::object(j.at("records"));
    for (auto& [key, value] : j.at("records").items()) {
        if (key.empty() || key.size() > 5 || key.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("Malformed V/F curve record");
        auto r = unhex(value.get<std::string>());
        if (r.size() != record_size) throw std::runtime_error("Malformed V/F curve record");
        c.records.emplace(std::stoi(key), std::move(r));
    }
    config::decode(j.at("offsetsKhz"), c.offsets_khz);
    return c;
}

size_t equation_count(const Bytes& b) {
    if (b.size() < equation_header) throw std::runtime_error("Truncated V/F equation list");
    size_t n = 0;
    for (size_t i = 0; i < mask_bytes * 8; ++i)
        if (b[4 + i / 8] & (1u << (i % 8))) {
            if (i != n)
                throw std::runtime_error(
                    "The driver lists its V/F equations with gaps, which MechControl does not know");
            ++n;
        }
    return n;
}
Bytes equation_info(Api& api) {
    auto b = packet(equation_info_bytes, equation_info_version);
    call(api, equation_info_call, b, "Reading the V/F equation list failed");
    b.resize(equation_header + equation_count(b) * info_size);
    return b;
}
Gpu gpu(const Identity& id, const Bytes& info) { return {id.device, id.bios, id.driver, hash(info)}; }
Bytes records(Api& api, const Bytes& info) {
    auto b = packet(equation_records_bytes, equation_records_version);
    std::copy_n(info.begin() + 4, mask_bytes, b.begin() + 4);
    call(api, equation_get_call, b, "Reading the V/F equations failed");
    b.resize(equation_header + equation_count(info) * record_size);
    return b;
}
Bytes variable_info(Api& api) {
    auto b = packet(variable_info_bytes, variable_info_version);
    call(api, variable_info_call, b, "Reading the V/F variable list failed");
    return b;
}
Bytes variable_records(Api& api, const Bytes& info) {
    auto b = packet(variable_records_bytes, variable_records_version);
    std::copy_n(info.begin() + 4, 32, b.begin() + 4);
    call(api, variable_get_call, b, "Reading the V/F variables failed");
    return b;
}
std::span<const uint8_t> record(const Bytes& records, size_t i) {
    if (equation_header + (i + 1) * record_size > records.size()) throw std::runtime_error("Truncated V/F equations");
    return std::span(records).subspan(equation_header + i * record_size, record_size);
}

Points points(Api& api) { return read_clock(api).points; }
int set_offsets(Api& api, const std::vector<int>& khz) {
    auto c = read_clock(api);
    auto changed = changes(c, khz);
    if (changed.empty()) return 0;
    refuse_afterburner();
    auto b = c.control;
    put(b, 0, version(uint32_t(b.size()), c.control_version));
    for (auto k : changed) put(b, 0x44 + c.index[k] * 0x24 + 0x14, uint32_t(khz[k]));
    call(api, offsets_set_call, b, "Writing the curve offsets failed");
    Sleep(300);
    auto after = read_clock(api);
    std::vector<size_t> bad;
    for (size_t k = 0; k < khz.size(); ++k)
        if (after.points.graphics[k].offset_khz != khz[k]) bad.push_back(k);
    if (!bad.empty()) throw std::runtime_error("The offsets of points " + list(bad) + " did not read back as written");
    for (int i = 0; i < 255; ++i)
        if (std::ranges::find(c.index, i) == c.index.end() &&
            u32(after.control, 0x44 + i * 0x24 + 0x14) != u32(c.control, 0x44 + i * 0x24 + 0x14))
            throw std::runtime_error("Writing the graphics offsets changed the offset of curve point " +
                                     std::to_string(i));
    return int(changed.size());
}

Applied apply(Api& api, const Curve& c, const std::filesystem::path& directory) {
    auto info = equation_info(api);
    Gpu now;
    auto stock = stock_for(api, info, now, directory);
    std::string why;
    if (c.gpu.device != now.device)
        why = "another GPU";
    else if (c.gpu.vbios != now.vbios)
        why = "VBIOS " + c.gpu.vbios + ", but this GPU has " + now.vbios;
    else if (c.gpu.driver != now.driver)
        why = "driver " + c.gpu.driver + ", but the driver is now " + now.driver;
    else if (c.gpu.equations != now.equations)
        why = "another equation list than the driver has now";
    if (!why.empty())
        throw std::runtime_error("This V/F curve was made for " + why +
                                 ". Nothing was written; make the curve again in the V/F curve tab.");
    if (!stock) throw std::runtime_error(std::string(no_stock) + "; nothing was written.");
    return write(api, info, *stock, c.records, c.offsets_khz);
}
Applied restore(Api& api, const std::filesystem::path& directory) {
    auto info = equation_info(api);
    Gpu now;
    auto stock = stock_for(api, info, now, directory);
    if (!stock) throw std::runtime_error(std::string(no_stock) + "; nothing was written.");
    return write(api, info, *stock, {}, std::vector<int>(points(api).graphics.size(), 0));
}

std::filesystem::path store_directory() {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &p)))
        throw std::runtime_error("Cannot locate ProgramData");
    std::filesystem::path path(p);
    CoTaskMemFree(p);
    return path / L"MechControl" / L"VfCurve";
}
std::filesystem::path stock_path(const std::filesystem::path& directory, const Gpu& g) {
    char name[160];
    sprintf_s(name, "%04X-%s-%s-%016llX.bin", g.device, g.vbios.c_str(), g.driver.c_str(),
              static_cast<unsigned long long>(g.equations));
    for (char* c = name; *c; ++c)
        if (!isalnum(static_cast<unsigned char>(*c)) && *c != '.' && *c != '-') *c = '_';
    return directory / name;
}
std::optional<Bytes> load_stock(const std::filesystem::path& directory, const Gpu& g, const Bytes& info) {
    auto path = stock_path(directory, g);
    if (!std::filesystem::exists(path)) return {};
    auto text = config::read_text(path);
    Bytes b(text.begin(), text.end());
    if (b.size() != equation_header + equation_count(info) * record_size ||
        !std::equal(info.begin() + 4, info.begin() + 4 + mask_bytes, b.begin() + 4))
        throw std::runtime_error("The recorded stock V/F equations do not match the driver's equation list: " +
                                 path.string());
    return b;
}
void save_stock(const std::filesystem::path& directory, const Gpu& g, const Bytes& records) {
    std::filesystem::create_directories(directory);
    config::write_text(stock_path(directory, g),
                       std::string_view(reinterpret_cast<const char*>(records.data()), records.size()), true);
}
}
