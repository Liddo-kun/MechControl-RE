#include "nvidia.h"
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace mech::nv {
static void require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(std::string("Unsupported NVIDIA BIOS: ") + what);
}
uint32_t u32(std::span<const uint8_t> b, size_t p) {
    if (p > b.size() || b.size() - p < 4) throw std::runtime_error("Truncated NVIDIA buffer");
    uint32_t v;
    memcpy(&v, b.data() + p, 4);
    return v;
}
void put(std::span<uint8_t> b, size_t p, uint32_t v) {
    if (p > b.size() || b.size() - p < 4) throw std::runtime_error("Truncated NVIDIA buffer");
    memcpy(b.data() + p, &v, 4);
}
Bytes packet(size_t size, uint32_t version) {
    Bytes b(size);
    put(b, 0, version);
    return b;
}
std::string Bios::version() const {
    std::ostringstream s;
    s << std::uppercase << std::hex << std::setfill('0');
    for (size_t i = 0; i < version_bytes.size(); ++i) {
        if (i) s << '.';
        s << std::setw(2) << unsigned(version_bytes[i]);
    }
    return s.str();
}
Bios Bios::parse(Bytes bytes) {
    require(bytes.size() >= 512 && bytes.size() <= 0x1000000, "ROM size");
    Bios result;
    result.image = std::move(bytes);
    auto& b = result.image;
    auto span = [&](size_t p, size_t n) { require(p <= b.size() && n <= b.size() - p, "truncated ROM"); };
    auto u16 = [&](size_t p) -> uint16_t {
        span(p, 2);
        return uint16_t(b[p] | (b[p + 1] << 8));
    };
    auto u = [&](size_t p) { return u32(b, p); };
    auto sig = [&](size_t p, std::string_view s) {
        span(p, s.size());
        return std::equal(s.begin(), s.end(), b.begin() + p);
    };
    size_t legacy = 0, efi = 0, pcir = 0, pos = 0;
    bool ended = false;
    for (int i = 0; i < 16; ++i) {
        span(pos, 0x1a);
        auto signature = u16(pos);
        require(signature == 0xaa55 || signature == 0x4e56 || signature == 0xbb77, "image signature");
        size_t pci = pos + u16(pos + 0x18);
        require(sig(pci, "PCIR") || sig(pci, "NPDS") || sig(pci, "RGIS"), "PCI directory");
        span(pci, 0x18);
        size_t pci_length = u16(pci + 10);
        require(pci_length >= 0x18, "PCI directory length");
        size_t npde = (pci + pci_length + 15) & ~size_t(15);
        require(sig(npde, "NPDE") && (u16(npde + 4) == 0x100 || u16(npde + 4) == 0x101) && u16(npde + 6) >= 12,
                "NPDE directory");
        span(npde, u16(npde + 6));
        size_t length = u16(npde + 8) * 512, pci_size = u16(pci + 0x10) * 512;
        require(length && pci_size && pci_size <= length && npde + u16(npde + 6) <= pos + length, "image length");
        auto type = b[pci + 0x14];
        if (i == 0) {
            require(type == 0 && length == pci_size && u16(pci + 4) == 0x10de, "NVIDIA legacy image");
            legacy = length;
            pcir = pci;
        } else if (i == 1 && type == 3) {
            require(length == pci_size, "EFI image length");
            efi = length;
        } else
            require(type == 0xe0, "image ordering");
        if (type == 0 || type == 3) {
            span(pos, length);
            unsigned sum = 0;
            for (size_t j = pos; j < pos + length; ++j) sum += b[j];
            require((sum & 255) == 0, "option ROM checksum");
        }
        if (pos + length > b.size()) require(i >= 2 && type == 0xe0, "truncated image");
        ended = (b[npde + 10] & 128) != 0;
        pos += length;
        if (ended) break;
        require(pos < b.size(), "unterminated image chain");
    }
    require(ended, "image count");
    auto map = [&](uint32_t logical, size_t count) {
        require(logical && logical != legacy && logical <= INT_MAX, "logical pointer");
        size_t p = logical + (logical > legacy ? efi : 0);
        span(p, count);
        return p;
    };
    size_t bit = SIZE_MAX;
    for (size_t p = 0; p + 12 <= legacy; ++p)
        if (b[p] == 255 && b[p + 1] == 0xb8 && sig(p + 2, std::string_view("BIT\0", 4))) {
            require(bit == SIZE_MAX, "ambiguous BIT directory");
            bit = p;
        }
    require(bit != SIZE_MAX && u16(bit + 6) == 0x100 && b[bit + 8] == 12 && b[bit + 9] == 6, "BIT header");
    unsigned sum = 0;
    for (size_t i = bit; i < bit + 12; ++i) sum += b[i];
    require((sum & 255) == 0, "BIT checksum");
    int tokens = b[bit + 10];
    require(bit + 12 + tokens * 6 <= legacy, "BIT entries");
    struct Token {
        int version;
        size_t offset, size;
    };
    std::map<uint8_t, Token> entries;
    for (int i = 0; i < tokens; ++i) {
        size_t t = bit + 12 + i * 6;
        size_t size = u16(t + 2);
        require(!entries.contains(b[t]), "duplicate BIT token");
        if (size) entries.emplace(b[t], Token{b[t + 1], map(u16(t + 4), size), size});
    }
    require(entries.contains('B') && entries.at('B').version == 2 && entries.at('B').size >= 5 &&
                entries.contains('P') && entries.at('P').version == 2 && entries.at('P').size >= 0xf8,
            "BIT B/P token");
    auto bios = entries.at('B').offset, power = entries.at('P').offset;
    result.version_bytes = {b[bios + 3], b[bios + 2], b[bios + 1], b[bios], b[bios + 4]};
    size_t table = map(u(power + 0x2c), 4);
    // RTX 50 uses 40 38 66 14. RTX 40 uses 30 32 53 14, or 30 34 5B 14 in later BIOS branches: the same entry
    // fields behind a longer header and stride (layout from mVolt 0.50.8/0.50.9, checked on 4060-4090 Laptop ROMs).
    auto layout = [&](uint8_t version, uint8_t header, uint8_t entry) {
        return b[table] == version && b[table + 1] == header && b[table + 2] == entry && b[table + 3] == 20;
    };
    result.ada = layout(0x30, 0x32, 0x53) || layout(0x30, 0x34, 0x5b);
    if (!result.ada && !layout(0x40, 0x38, 0x66)) {
        char header[16];
        sprintf_s(header, "%02X %02X %02X %02X", b[table], b[table + 1], b[table + 2], b[table + 3]);
        throw std::runtime_error(std::string("Unsupported NVIDIA BIOS: power policy layout ") + header);
    }
    size_t size = b[table + 1], stride = b[table + 2];
    span(table, size + 20 * stride);
    auto entry = [&](int i) { return table + size + i * stride; };
    auto limits = [&](size_t p) {
        auto min = u(p), def = u(p + 4), max = u(p + 8);
        require(min <= def && def <= max && max && max <= INT_MAX, "policy range");
        return Field{static_cast<int>(p), min, def, max};
    };
    if (result.ada) {
        // Identity byte at +0, channel at +1, limits at +2. Entries 7/8/11/12
        // differ from RTX 50 and are not checked; index 14 is not MSVDD.
        auto field = [&](int i, bool identity) {
            auto e = entry(i);
            require(identity && b[e + 1] < 32, "policy identity");
            return limits(e + 2);
        };
        auto p = entry(2), n = entry(13);
        result.power = field(2, b[p] == 1 || b[p] == 0x21);
        result.nv = field(13, uint8_t((b[n + 0x42] << 4) | (b[n] & 15)) == 0x10 && u(n + 0xe) == 0xd00);
        if (b[entry(9)] == 0x15) result.input = field(9, true);
    } else {
        auto field = [&](int i, int type, uint32_t flags) {
            auto e = entry(i);
            require(b[e] == type && u(e + 1) == flags && b[e + 5] < 32, "policy identity");
            return limits(e + 13);
        };
        result.power = field(2, 1, 2);
        for (int i : {7, 8}) {
            auto e = entry(i);
            require(b[e] == 0 && u(e + 1) == 0 && u(e + 13) == 0 && u(e + 17) == 0 && u(e + 21) == 0,
                    "additional board-power policies");
        }
        result.nv = field(13, 7, 1);
        result.ms = field(14, 7, 1);
        if (b[entry(9)] == 3 && u(entry(9) + 1) == 1) result.input = field(9, 3, 1);
        require(b[entry(11)] == 6 && b[entry(12)] == 6 && u(entry(11) + 1) == 1 && u(entry(12) + 1) == 1 &&
                    b[entry(11) + 5] == b[entry(13) + 5] && b[entry(12) + 5] == b[entry(14) + 5] &&
                    b[entry(13) + 5] != b[entry(14) + 5],
                "rail monitor identity");
    }
    if (auto ptr = u(power + 0xf4)) {
        auto profile = map(ptr, 0x58e);
        const uint8_t header[]{0x10, 0x0a, 5, 4, 7, 8, 1, 0x10, 9, 8};
        require(std::equal(std::begin(header), std::end(header), b.begin() + profile), "BIOS profile header");
        for (int i = 0; i < 4; ++i) require((b[profile + 10 + i * 0x3d] & 15) == 0, "active base BIOS profiles");
        for (int i = 0; i < 16; ++i) require((b[profile + 0xfe + i * 0x49] & 15) == 0, "active extended BIOS profiles");
    }
    result.device = u16(pcir + 6);
    return result;
}
std::vector<int> Bios::offsets() const {
    std::vector<int> v{power.offset + 8, nv.offset + 4, nv.offset + 8};
    if (ms) {
        v.push_back(ms->offset + 4);
        v.push_back(ms->offset + 8);
    }
    if (input) {
        if (!ada) v.push_back(input->offset + 4);
        v.push_back(input->offset + 8);
    }
    return v;
}
std::vector<Record> Bios::build(const Override& request) const {
    std::map<int, uint32_t> patches;
    auto add = [&](Field f, uint32_t def, uint32_t max) {
        require(def >= f.min && def && def <= max && max <= INT_MAX, "override range");
        if (def != f.def) patches.emplace(f.offset + 4, def);
        if (max != f.max) patches.emplace(f.offset + 8, max);
    };
    if (request.power) add(power, power.def, *request.power);
    if (request.nv) add(nv, request.nv->def, request.nv->max);
    if (request.ms) {
        require(ms.has_value(), "MSVDD policy");
        add(*ms, request.ms->def, request.ms->max);
    }
    if (request.input) {
        require(input.has_value(), "board input policy");
        add(*input, ada ? input->def : request.input->def, request.input->max);
    }
    std::vector<Record> records;
    for (auto [offset, value] : patches) {
        Bytes data(14);
        std::copy(version_bytes.begin(), version_bytes.end(), data.begin());
        put(data, 5, offset);
        data[9] = 4;
        put(data, 10, value);
        char name[32];
        sprintf_s(name, "romOverride%02X", static_cast<unsigned>(records.size()));
        records.push_back({name, std::move(data)});
    }
    return records;
}
int Rail::allowed(bool unlock) const {
    return static_cast<int>(unlock ? std::max<uint64_t>(stock_max, uint64_t(def) * unlock_percent / 100) : stock_max);
}
Budget Budget::raise(uint32_t requested, Budget baseline) const {
    uint32_t f = std::max(baseline.f7, requested);
    uint64_t p = uint64_t(baseline.platform) + (f - baseline.f7);
    if (p > UINT32_MAX) throw std::runtime_error("NVIDIA shared budget overflow");
    return {f, static_cast<uint32_t>(p)};
}
ProfilePlan plan_profile(const config::GpuSettings& source, bool automatic, bool extend, int lo, int hi, int ulo,
                         int uhi, bool unlocked, bool available, const std::optional<Currents>& rails,
                         const std::function<bool(bool, const config::GpuSettings&)>& restart) {
    ProfilePlan plan{source, unlocked};
    auto& g = plan.settings;
    if ((g.bios_override_enabled == true || g.tgp > hi || g.tgp < lo) && !unlocked && extend && g.tgp >= ulo &&
        g.tgp <= uhi) {
        if (!automatic && available)
            plan.unlock = true;
        else {
            g.tgp = std::clamp(g.tgp, lo, hi);
            g.nvvdd_ma = g.msvdd_ma = g.board_input_ma = {};
            plan.notice =
                "Profile GPU unlock is off/unavailable; power limited to the normal range. Load manually to enable it.";
        }
    }
    if (g.tgp < (plan.unlock && extend ? ulo : lo) || g.tgp > (plan.unlock && extend ? uhi : hi))
        throw std::runtime_error("Profile GPU power exceeds model range");
    plan.restart = plan.unlock && !unlocked;
    if (!rails) {
        g.nvvdd_ma = g.msvdd_ma = g.board_input_ma = {};
        return plan;
    }
    if (g.nvvdd_ma || g.msvdd_ma || g.board_input_ma || (plan.unlock && g.bios_override_enabled != true)) {
        std::string skipped = g.msvdd_ma && !rails->ms ? "MSVDD" : "";
        if (g.board_input_ma && !rails->input) skipped += skipped.empty() ? "board input" : " and board input";
        if (!skipped.empty()) plan.notice = "Profile " + skipped + " current skipped: this GPU has no matching policy.";
        if (plan.unlock && g.bios_override_enabled != true) {
            if (!g.nvvdd_ma) g.nvvdd_ma = rails->nv.def;
            if (!g.msvdd_ma && rails->ms) g.msvdd_ma = rails->ms->def;
            if (!g.board_input_ma && rails->input) g.board_input_ma = rails->input->def;
        }
        auto clamp = [](std::optional<int> v, const std::optional<Rail>& rail, bool unlock,
                        bool active) -> std::optional<int> {
            if (!v || !rail) return {};
            return std::clamp(*v, static_cast<int>(rail->min),
                              active ? static_cast<int>(rail->max) : rail->allowed(unlock));
        };
        auto limit = [&](bool active) {
            g.nvvdd_ma = clamp(g.nvvdd_ma, rails->nv, plan.unlock, active);
            g.msvdd_ma = clamp(g.msvdd_ma, rails->ms, plan.unlock, active);
            g.board_input_ma = clamp(g.board_input_ma, rails->input, plan.unlock, active);
        };
        limit(false);
        if (!plan.restart && restart(plan.unlock, g)) {
            if (!automatic)
                plan.restart = true;
            else {
                limit(true);
                if (restart(plan.unlock, g)) g.nvvdd_ma = g.msvdd_ma = g.board_input_ma = {};
                plan.notice +=
                    " Profile currents need a GPU restart; limited to active ceilings. Load manually to apply them.";
            }
        }
    }
    return plan;
}
}
