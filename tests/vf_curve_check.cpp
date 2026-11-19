// V/F curve checks. Without arguments: synthetic equations and a fake driver (CTest). Local modes, which need data
// that is not in the repository:
//   --oracle <dump dir> <cases.json> <out.json>   the calculation for the comparison with Jon's Python tool
//   --live <vfe-tune.json> [<dump dir>]           WRITES: stock, then the tune, on the real GPU; then checks that the
//                                                 live state equals what it was. The dump seeds the stock records.
#include "vf_tuner.h"
#include "config_text.h"
#include <cmath>
#include <cstring>
#include <iostream>

using namespace mech;
using namespace mech::nv;
using namespace mech::nv::vf;
namespace {
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
    std::cout << "PASS " << message << '\n';
}
template <class F> std::string refusal(F f) {
    try {
        f();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}
void put_f32(Bytes& b, size_t p, float v) { std::memcpy(b.data() + p, &v, 4); }
void put_u16(Bytes& b, size_t p, int v) {
    b[p] = uint8_t(v);
    b[p + 1] = uint8_t(v >> 8);
}
constexpr float fmax = 3.4028234663852886e38f;

// A small equation list shaped like the RTX 5090 Laptop GPU's (VFCURVE notes.md): a switch to a slow curve below a
// switch voltage; the fast curve is the lowest of a steep foot and, by voltage segment, a low line (with a second line
// above it) or a high line. The low line carries a channel 3 correction, the high line a cold correction.
struct Synthetic {
    Bytes info = packet(0x98444, 0xd8444), records = packet(0x158444, 0x158444);
    Bytes variable_info = packet(0x10f424, 0x15f424), variable_records = packet(0xabbc, 0x3abbc);
    int equations = 0, variables = 0;
    Synthetic() {
        var(9, -fmax, fmax);                          // 0: point voltage
        var(8, -100, 200, 0, 0, 4);                   // 1: channel 4
        var(13, -fmax, fmax, 0, 0, 0, 2000);          // 2: chip value
        var(8, -100, 200, 0, 0, 3);                   // 3: channel 3
        var(13, -1, -1, 0, 0, 0, -1);                 // 4: -1
        var(2, -fmax, fmax, 1, 4);                    // 5: -channel 4
        var(3, 0, fmax, 3, 5);                        // 6: channel 3 above channel 4, never below 0
        var(15, 0, 1);                                // 7: the flag
        compare(0, 1, 1, 1, 2, 3, 180, 3000);         // 0: root, the switch at equation 1
        formula(3, 2, {0, 0, 0}, 700000, 700000);     // 1: the switch voltage
        choice(4, 5);                                 // 2: the fast curve
        formula(3, 0, {200, 0.0018f, 0});             // 3: the slow curve
        line({-18000, 0, 0, 0.03f});                  // 4: the foot
        compare(0, 1, 900000, 0, 7, 16);              // 5: the voltage segments
        line({300, 0, 0, 0.002f, 0, 0, -1}, 9);       // 6: low
        line({750, 0, 0, 0.0015f, 0, 0, -0.5f}, 13);  // 7: high
        line({365, 0, 0, 0.002f, 0, 0, -1});          // 8: a second line above low
        compare(6, 0, 0, 0, 10, 11, -fmax, fmax, 12); // 9: +30 while channel 3 is not above channel 4
        formula(3, 2, {0, 0, 0}, 30, 30);             // 10
        formula(3, 2, {0, 0, 0}, 0, 0);               // 11
        formula(3, 2, {0, 0, 0}, -30, -30);           // 12: the fixed -30 after it
        compare(1, 1, 30, 0, 14, 15);                 // 13: a correction below 30 C
        formula(3, 2, {0, 0, 0}, 0, 0);               // 14
        formula(3, 0, {-40, 0, 0}, -100, 0);          // 15
        choice(6, 8);                                 // 16
    }
    void var(int type, float lo, float hi, int a = 0, int b = 0, int channel = 0, int value = 0) {
        int i = variables++;
        variable_info[4 + i / 8] |= uint8_t(1u << (i % 8));
        size_t v = 296 + i * 4356, k = 104 + i * 172;
        put(variable_info, v, uint32_t(type));
        variable_info[v + 268] = uint8_t(a);
        variable_info[v + 269] = uint8_t(b);
        variable_info[v + 276] = uint8_t(channel);
        put(variable_info, v + 284, uint32_t(value));
        put_f32(variable_records, k + 4, lo);
        put_f32(variable_records, k + 8, hi);
    }
    size_t add(int type, int var, float lo, float hi, int next) {
        int i = equations++;
        info[4 + i / 8] |= uint8_t(1u << (i % 8));
        records[4 + i / 8] |= uint8_t(1u << (i % 8));
        size_t a = 1092 + i * 76;
        put(info, a, uint32_t(type));
        put_u16(info, a + 4, var);
        put_u16(info, a + 6, next < 0 ? 0xffff : next);
        put_f32(records, 1092 + i * 172 + 4, lo);
        put_f32(records, 1092 + i * 172 + 8, hi);
        return a;
    }
    size_t control(int i) { return 1092 + i * 172; }
    void compare(int var, int func, float crit, int ref, int yes, int no, float lo = -fmax, float hi = fmax,
                 int next = -1) {
        auto a = add(1, var, lo, hi, next);
        int i = equations - 1;
        put_u16(info, a + 42, yes);
        put_u16(info, a + 44, no);
        records[control(i) + 44] = uint8_t(func);
        records[control(i) + 45] = uint8_t(ref);
        if (ref)
            put(records, control(i) + 48, uint32_t(crit));
        else
            put_f32(records, control(i) + 48, crit);
    }
    void choice(int a, int b) {
        auto p = add(2, 0, -fmax, fmax, -1);
        put_u16(info, p + 42, a);
        put_u16(info, p + 44, b);
    }
    void formula(int type, int var, std::array<float, 3> k, float lo = -fmax, float hi = fmax, int next = -1) {
        add(type, var, lo, hi, next);
        for (int j = 0; j < 3; ++j) put_f32(records, control(equations - 1) + 44 + 4 * j, k[j]);
    }
    // A main formula in chip value, voltage and temperature with ten coefficients.
    void line(std::vector<float> k, int next = -1) {
        add(6, 2, -fmax, fmax, next);
        auto c = control(equations - 1);
        put(records, c + 68, 3);
        for (int j = 0; j < 3; ++j) put(records, c + 48 + 8 * j, uint32_t(std::array{2, 0, 1}[j]));
        put(records, c + 152, 10);
        k.resize(10);
        for (int j = 0; j < 10; ++j) put_f32(records, c + 76 + 8 * j, k[j]);
    }
    Bytes info_trimmed() const { return Bytes(info.begin(), info.begin() + 1092 + equations * 76); }
    Bytes records_trimmed() const { return Bytes(records.begin(), records.begin() + 1092 + equations * 172); }
};

// The driver as the V/F packets see it: the synthetic equations and a client curve of 96 graphics points (grid 498.75
// mV + 7.5 mV, listed rounded up to 5 mV, clock step 15 MHz, at 45 C) plus four memory points.
struct Driver {
    Synthetic s;
    Bytes live = s.records;
    std::vector<uint32_t> base_khz, uv;
    std::array<int32_t, 100> offsets{};
    std::vector<int> sets; // equation SetControl calls: the equation each wrote
    int offset_sets = 0;
    bool refuse_records = false;
    Driver() {
        Model m(s.info_trimmed(), s.records_trimmed(), s.variable_info, s.variable_records);
        std::vector<double> raw;
        for (int p = 0; p < 96; ++p) {
            double u = 498750 + 7500.0 * p;
            uv.push_back(uint32_t(std::ceil(u / 5000 - 1e-9) * 5000));
            raw.push_back(Env(m, m.E, u, 45).equ(0));
        }
        for (double r : hold(raw)) base_khz.push_back(uint32_t(to_step(r, 15) * 1000));
    }
    void call(uint32_t id, Bytes& b) {
        switch (id) {
        case 0x8d49471c:
            if (b.size() != s.info.size() || u32(b, 0) != 0xd8444)
                throw std::runtime_error("NVIDIA packet call failed (-9)");
            b = s.info;
            break;
        case 0x4c75c9fe: {
            if (refuse_records || u32(b, 0) != 0x158444) throw std::runtime_error("NVIDIA packet call failed (-9)");
            Bytes mask(b.begin() + 4, b.begin() + 1028);
            b = live;
            std::copy(mask.begin(), mask.end(), b.begin() + 4);
            for (int i = 0; i < s.equations; ++i)
                if (!(mask[i / 8] & (1u << (i % 8)))) std::fill_n(b.begin() + 1092 + i * 172, 172, uint8_t(0));
            break;
        }
        case 0x68b798c4: {
            int bits = 0, which = -1;
            for (int i = 0; i < 8192; ++i)
                if (b[4 + i / 8] & (1u << (i % 8))) {
                    ++bits;
                    which = i;
                }
            if (bits != 1) throw std::runtime_error("fixture: one equation per SetControl");
            std::copy_n(b.begin() + 1092 + which * 172, 172, live.begin() + 1092 + which * 172);
            sets.push_back(which);
            break;
        }
        case 0xb9da41d6: b = s.variable_info; break;
        case 0x5d387298: b = s.variable_records; break;
        case 0x64b43a6a: { // clock domains: graphics points 0-95, memory 96-99
            put(b, 4, 3);
            put(b, 0x28 + 0x28, uint32_t(1000000));
            put(b, 0x28 + 0x2c, uint32_t(-1000000));
            b[0x28 + 0x30] = 0;
            b[0x28 + 0x31] = 95;
            put(b, 0x28 + 0x48 + 4, 4);
            b[0x28 + 0x48 + 0x30] = 96;
            b[0x28 + 0x48 + 0x31] = 99;
            break;
        }
        case 0x507b4b59:
            for (int i = 0; i < 100; ++i) b[4 + i / 8] |= uint8_t(1u << (i % 8));
            put(b, 0x44 + 96 * 0x18, 1); // fixed memory points
            put(b, 0x44 + 97 * 0x18, 1);
            break;
        case 0x21537ad4:
            if (u32(b, 0) >> 16 != 3) throw std::runtime_error("NVIDIA packet call failed (-9)");
            b[0x24] = 1;
            for (int i = 0; i < 96; ++i) {
                size_t p = 0x68 + i * 0x15c;
                put(b, p + 4, uint32_t(base_khz[i] + offsets[i]));
                put(b, p + 8, uv[i]);
                put(b, p + 0xc, base_khz[i]);
            }
            break;
        case 0x23f1b133:
            for (int i = 0; i < 100; ++i) put(b, 0x44 + i * 0x24 + 0x14, uint32_t(offsets[i]));
            break;
        case 0x0733e009:
            ++offset_sets;
            for (int i = 0; i < 100; ++i) offsets[i] = int32_t(u32(b, 0x44 + i * 0x24 + 0x14));
            break;
        default: throw std::runtime_error("fixture: unexpected NVAPI call");
        }
    }
};
std::filesystem::path fresh(const std::filesystem::path& root, const char* name) {
    auto p = root / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
}
void fixtures(const std::filesystem::path& root) {
    Synthetic s;
    Model m(s.info_trimmed(), s.records_trimmed(), s.variable_info, s.variable_records);
    {
        auto at = [&](const Equations& E, double mv, double celsius, std::map<int, double> hotter = {}) {
            return Env(m, E, mv * 1000, celsius, 0, hotter).equ(0);
        };
        double slow = 200 + double(0.0018f) * 650000, low = 300 + double(0.002f) * 750000 - 20;
        check(at(m.E, 650, 59) == slow && at(m.E, 600, 59) == 200 + double(0.0018f) * 600000,
              "below the switch voltage the stock curve is the slow curve");
        check(at(m.E, 750, 20) == low && at(m.E, 750, 20, {{3, 1}}) == low - 30,
              "a line's correction applies while channel 3 is above channel 4");
        check(at(m.E, 950, 29.5) == 750 + double(0.0015f) * 950000 - 0.5 * 29.5 - 40 &&
                  at(m.E, 950, 30) == 750 + double(0.0015f) * 950000 - 15,
              "the cold correction applies below 30 C only");
        auto fast = m.E;
        fast[1].lo = fast[1].hi = -1000000;
        check(at(fast, 500, 59) == 180 && at(fast, 650, 59) == -18000 + double(0.03f) * 650000,
              "with the switch voltage below every point the fast curve applies, down to the floor");
    }
    check(to_step(2752.0, 7.5) == 2752.5 && to_step(2751.9, 7.5) == 2745 && snap(2167, 7.5) == 2167.5,
          "the driver's rounding: whole MHz, then the highest step whose whole MHz is not above it");
    {
        auto w = with_offsets({2422.5, 2415, 2422.5, 2437.5, 2400}, {7.5, 7.5, -7.5, 15, 0}, 7.5);
        check(w == std::vector<double>({2422.5, 2422.5, 2422.5, 2452.5, 2452.5}),
              "the offset rule: +7.5 adds nothing at an x.5 clock, multiples of 15 add exactly, the curve never falls");
        check(with_offsets({2400}, {0.5}, 7.5)[0] == 2407.5 && with_offsets({2400}, {-38}, 7.5)[0] == 2362.5,
              "an offset counts as a multiple of 15 MHz, plus 7 MHz when anything was cut");
    }

    Driver d;
    Api api([&](auto id, auto& b) { d.call(id, b); });
    api.identity.device = 0x2c58;
    api.identity.bios = "98.03.5e.00.1e";
    api.identity.driver = "621.24";
    auto store = fresh(root, "store");
    auto t = vf::open(api, store);
    auto info = equation_info(api);
    auto g = gpu(api.identity, info);
    check(load_stock(store, g, info) == s.records_trimmed() &&
              std::filesystem::exists(stock_path(store, g).replace_extension(".json")),
          "the tuner's first use records the live records as stock and keeps the clock curve it found");
    check(t.meta.root == 0 && t.meta.v0 == 498750 && t.meta.vstep == 7500 && t.meta.step == 15 && t.meta.equal == 96 &&
              t.meta.voltages == "grid" && t.refusal.empty(),
          "the clock equation, point grid and 15 MHz step are found from the driver's curve");
    auto ids = [&] {
        std::vector<std::string> v;
        for (auto& sec : t.sections) v.push_back(sec.id);
        return v;
    };
    check(
        ids() == std::vector<std::string>({"foot", "low", "high"}) && t.sections[0].heads == std::vector{4} &&
            t.sections[1].heads == std::vector({6, 8}) && t.sections[2].heads == std::vector{7},
        "the steep line is the foot, each line that sets the clock a card, and a second line joins the line below it");
    check(t.sections[1].note.find("8 is a second line 60-70 MHz above it") != std::string::npos &&
              t.sections[1].lo == 656.25 && t.sections[1].hi == 896.25 && t.sections[2].lo == 903.75,
          "a card's range is where its line is evaluated and a line outside the foot sets the clock");
    check(t.rules.size() == 2 && t.rules[0].id == "ch3" && t.rules[0].equs == std::vector({10, 12}) &&
              t.rules[0].hot == 3 && t.rules[0].cold == 4 && t.rules[1].id == "cold" &&
              t.rules[1].equs == std::vector{15} && t.rules[1].crit == 30 &&
              t.rules[1].text.find("When the temperature is below 30 °C, the curve is lowered by ") == 0,
          "the channel 3 and cold corrections are found and described");
    check(t.switch_fields.size() == 1 && t.switch_fields.at(1).at("lo") == -1000000 && t.main_channel == 4 &&
              t.slots.at(6).v == 3 && t.slots.at(6).v2 == 4 && t.slots.at(6).t == 6 && t.slots.at(6).vt == 9,
          "the switch is turned off at its fixed voltage, and the slider terms are found from each formula's inputs");

    auto tune = t.tune(config::parse(R"({"global": 30, "ver": 2, "sec": {"low": {"l": -20, "r": -20}},
        "rules": {"cold": true}, "manual": {"0.hi": 2900}, "po": 15, "pp": {"10": 7.5}})"));
    auto curve = t.compile(tune, g);
    auto k0 = [&](int i) {
        float f;
        std::memcpy(&f, curve.records.at(i).data() + 76, 4);
        return f;
    };
    float hi, lo15;
    std::memcpy(&hi, curve.records.at(0).data() + 8, 4);
    std::memcpy(&lo15, curve.records.at(15).data() + 4, 4);
    std::vector<int> changed;
    for (auto& [i, r] : curve.records) changed.push_back(i);
    check(
        changed == std::vector({0, 1, 4, 6, 7, 8, 15}) && k0(6) == 310 && k0(8) == 375 && k0(7) == 780 &&
            k0(4) == -17970 && hi == 2900 && lo15 == 0 && curve.offsets_khz[10] == 22500 &&
            curve.offsets_khz[11] == 15000,
        "a tune compiles to the changed records (sliders, a typed limit, a removed correction, the switch) and offsets");
    check(t.tune(t.state(tune)).cards.at("low").l == -20 && t.compile(t.tune(t.state(tune)), g) == curve &&
              curve_from_json(config::parse(to_json(curve).dump())) == curve,
          "a tune and a compiled curve survive their JSON");

    auto applied = apply(api, curve, store);
    check(applied.equations == 7 && applied.offsets == 96 && d.sets.size() == 7 && d.offset_sets == 1,
          "apply writes each changed equation alone, then the offsets");
    bool same = true;
    for (int i = 0; i < s.equations; ++i) {
        auto it = curve.records.find(i);
        same = same &&
               std::ranges::equal(record(d.live, i), it != curve.records.end() ? std::span<const uint8_t>(it->second)
                                                                               : record(s.records, i));
    }
    check(same && d.offsets[10] == 22500 && d.offsets[96] == 0, "the driver then has the curve");
    check(apply(api, curve, store).equations == 0 && d.sets.size() == 7 && d.offset_sets == 1,
          "applying the same curve again writes nothing");
    auto calc = t.calc(t.edited(t.fields(tune)), 59);
    auto fit = t.fit(t.edited(t.fields(tune)), calc.clock);
    check(fit.equal == 96 && calc.win[0] == Calc::floor && calc.win[40] == 6 && calc.lines.at(8)[40].has_value() &&
              !calc.lines.at(7)[40].has_value(),
          "the chart's calculation says what sets each point and draws each card's lines");
    check(restore(api, store).equations == 7 && std::ranges::equal(d.live, s.records) && d.offsets[10] == 0,
          "restore writes the stock records and zero offsets");

    auto other = curve;
    other.gpu.driver = "625.10";
    d.sets.clear();
    auto why = refusal([&] { apply(api, other, store); });
    check(why.find("driver 625.10, but the driver is now 621.24") != std::string::npos && d.sets.empty(),
          "a curve made for another driver is refused before any write");
    other = curve;
    other.gpu.equations ^= 1;
    check(!refusal([&] { apply(api, other, store); }).empty() && d.sets.empty(),
          "a curve made for another equation list is refused");
    check(refusal([&] { apply(api, curve, fresh(root, "empty")); }).find("not recorded") != std::string::npos &&
              d.sets.empty(),
          "a curve is refused where the GPU's stock records were never recorded");
    other = curve;
    other.offsets_khz[3] = 2000000;
    check(!refusal([&] { apply(api, other, store); }).empty() && d.sets.empty() && d.offset_sets == 2,
          "an offset outside the driver's limits is refused before the equations are written");
    d.refuse_records = true;
    check(refusal([&] { apply(api, curve, store); }).find("621.24") != std::string::npos && d.sets.empty(),
          "a driver that refuses the packets is reported, and nothing is written");
    d.refuse_records = false;

    auto bad = s;
    put(bad.info, 1092 + 9 * 76, 7);
    check(refusal([&] {
              Model(bad.info_trimmed(), bad.records_trimmed(), bad.variable_info, bad.variable_records);
          }).find("type MechControl does not know") != std::string::npos,
          "an unknown equation type is refused");
    auto gaps = s;
    gaps.info[4] &= ~uint8_t(2);
    check(!refusal([&] { equation_count(gaps.info); }).empty(), "an equation list with gaps is refused");
    config::write_text(stock_path(store, g), "short", false);
    check(!refusal([&] { load_stock(store, g, info); }).empty(), "a damaged stock file is refused");
}

// ---- local modes ----
Bytes file(const std::filesystem::path& p) {
    auto t = config::read_text(p);
    return Bytes(t.begin(), t.end());
}
config::Json rows_json(const std::vector<Row>& rows) {
    auto out = config::Json::array();
    for (auto& r : rows)
        out.push_back({{"equ", r.equ},
                       {"kind", r.kind},
                       {"when", r.when},
                       {"used", r.used},
                       {"var", r.var},
                       {"yes", r.yes},
                       {"no", r.no},
                       {"a", r.a},
                       {"b", r.b}});
    return out;
}
int oracle(const std::filesystem::path& dump, const std::filesystem::path& cases_path,
           const std::filesystem::path& out_path) {
    auto cases = config::read_json(cases_path);
    auto info = file(dump / "equinfo4.bin"), stock = file(dump / "equctl.bin");
    auto vinfo = file(dump / "varinfo5.bin"), vrec = file(dump / "varctl.bin");
    auto meta = meta_from_json(config::read_json(dump / "meta.json"));
    auto opt = [](int k) { return k < 0 ? config::Json(nullptr) : config::Json("k" + std::to_string(k)); };
    config::Json out;
    for (auto& [chip, value] : cases["chips"].items()) {
        Model model(info, stock, vinfo, vrec);
        if (!value.is_null()) model.V.at(cases["chipVariable"].get<int>()).value = value.get<int>();
        Tuner t(model, meta);
        config::Json o;
        for (auto& s : t.sections) {
            config::Json chains = config::Json::array();
            for (auto& [h, rows] : s.chains) chains.push_back({{"head", h}, {"rows", rows_json(rows)}});
            o["sections"].push_back({{"id", s.id},
                                     {"name", s.name},
                                     {"lo", s.lo},
                                     {"hi", s.hi},
                                     {"lo0", s.lo0},
                                     {"hi0", s.hi0},
                                     {"heads", s.heads},
                                     {"note", s.note},
                                     {"pts", {s.p0, s.p1}},
                                     {"chains", chains}});
        }
        for (auto& r : t.rules)
            o["rules"].push_back({{"id", r.id},
                                  {"kind", r.kind},
                                  {"title", r.title},
                                  {"text", r.text},
                                  {"equs", r.equs},
                                  {"hot", r.hot},
                                  {"cold", r.cold},
                                  {"chan", r.channel},
                                  {"crit", r.crit},
                                  {"side", r.side}});
        for (auto& [i, d] : t.switch_fields) o["switch"]["fields"][std::to_string(i)] = d;
        o["switch"]["text"] = t.switch_text;
        o["switch"]["detail"] = t.switch_detail;
        for (auto& [h, k] : t.slots)
            o["slots"][std::to_string(h)] = {{"c", opt(k.c)}, {"v", opt(k.v)},   {"v2", opt(k.v2)},
                                             {"t", opt(k.t)}, {"t2", opt(k.t2)}, {"vt", opt(k.vt)}};
        o["struct"] = rows_json(t.structure);
        o["knames"] = t.knames;
        o["formnote"] = t.formnote;
        o["heads"] = t.heads;
        auto temps = cases["temps"].get<std::vector<double>>();
        for (double T : temps) {
            o["stock"][config::decimal_text(T, 1)] = t.curve(t.m.E, T);
            o["fast"][config::decimal_text(T, 1)] = t.curve(t.fast, T);
        }
        for (auto& c : cases["cases"]) {
            config::Json r = {{"name", c["name"]}};
            try {
                auto tune = t.tune(c["state"]);
                auto f = t.fields(tune);
                for (auto& [i, d] : f) r["fields"][std::to_string(i)] = d;
                auto E = t.edited(f);
                for (double T : temps) {
                    r["curves"][config::decimal_text(T, 1)] = t.curve(E, T);
                    for (auto& rule : t.rules)
                        if (rule.kind == "diff")
                            r["preview"][config::decimal_text(T, 1)] = t.curve(E, T, 0, {{rule.hot, 1.0}});
                }
                r["offsets"] = t.offsets(tune);
                if (chip == "jon") r["compiled"] = to_json(t.compile(tune, {}));
            } catch (const std::exception& e) {
                r["error"] = e.what();
            }
            o["cases"].push_back(r);
        }
        out["chips"][chip] = o;
    }
    // The offset rule against the driver's readings.
    for (auto& r : cases["offsetReadings"])
        out["offsetRule"].push_back(
            with_offsets(r["stock"].get<std::vector<double>>(), r["off"].get<std::vector<double>>(), 7.5));
    // The clock curve search and the readback fit on a driver curve with the records it came from.
    for (auto& p : cases["driverCurves"]) {
        Points points;
        for (size_t i = 0; i < p["uv"].size(); ++i)
            points.graphics.push_back({p["uv"][i].get<uint32_t>(), p["baseKhz"][i].get<uint32_t>(), 0, 0, true});
        Model live(info, file(p["records"].get<std::string>()), vinfo, vrec);
        auto found = find_meta(live, points);
        Tuner t(Model(info, stock, vinfo, vrec), meta);
        std::vector<double> drv;
        for (double x : driver_stock(points)) drv.push_back(snap(x, meta.step));
        auto f = t.fit(t.edited(t.live_fields(live.stock)), drv);
        config::Json diffs = config::Json::array();
        for (auto& d : f.diffs)
            diffs.push_back(
                {{"point", d.point}, {"driver", d.driver}, {"computed", d.computed}, {"raw", d.raw}, {"edge", d.edge}});
        config::Json lf;
        for (auto& [i, d] : t.live_fields(live.stock)) lf[std::to_string(i)] = d;
        out["driverCurves"].push_back({{"name", p["name"]},
                                       {"meta", to_json(found)},
                                       {"liveFields", lf},
                                       {"fit",
                                        {{"T", f.celsius},
                                         {"Tlo", f.lo},
                                         {"Thi", f.hi},
                                         {"equal", f.equal},
                                         {"edge", f.edge},
                                         {"diffs", diffs},
                                         {"computed", f.computed}}}});
    }
    config::write_json(out_path, out, false);
    std::cout << "wrote " << out_path.string() << '\n';
    return 0;
}

int live(const std::filesystem::path& tune_path, const std::filesystem::path& dump) {
    Api api;
    auto info = equation_info(api);
    auto g = gpu(api.identity, info);
    auto store = store_directory();
    std::cout << "GPU " << api.identity.name << ", device " << std::hex << g.device << std::dec << ", VBIOS " << g.vbios
              << ", driver " << g.driver << ", " << equation_count(info) << " equations\nStock store "
              << stock_path(store, g).string() << '\n';
    if (!dump.empty()) {
        auto dumped = file(dump / "equinfo4.bin");
        dumped.resize(info.size());
        check(dumped == info, "the dump's equation list equals the driver's");
        auto records = file(dump / "equctl.bin");
        records.resize(equation_header + equation_count(info) * record_size); // as records() returns them
        if (auto saved = load_stock(store, g, info))
            check(*saved == records, "the recorded stock equals the dump");
        else {
            save_stock(store, g, records);
            std::filesystem::last_write_time(stock_path(store, g),
                                             std::filesystem::last_write_time(dump / "equctl.bin"));
            std::cout << "recorded the dump's records as this GPU's stock\n";
        }
    }
    auto t = vf::open(api, store);
    std::cout << "clock curve: equation " << t.meta.root << ", " << t.points() << " points from " << t.mv(0)
              << " mV, step " << t.meta.step << " MHz; equal to the driver at " << t.meta.equal << " points at "
              << t.meta.temperature << " C; stock recorded " << t.meta.recorded
              << "; refusal: " << (t.refusal.empty() ? "none" : t.refusal) << '\n';
    auto before = records(api, info);
    auto before_points = points(api);
    auto tune = t.tune(config::read_json(tune_path).at("state"));
    auto curve = t.compile(tune, g);
    bool equal = true;
    for (size_t i = 0; i < equation_count(info); ++i) {
        auto it = curve.records.find(int(i));
        equal = equal &&
                std::ranges::equal(record(before, i), it != curve.records.end() ? std::span<const uint8_t>(it->second)
                                                                                : record(t.m.stock, i));
    }
    for (size_t k = 0; k < before_points.graphics.size(); ++k)
        equal = equal && before_points.graphics[k].offset_khz == curve.offsets_khz[k];
    std::cout << "the compiled tune " << (equal ? "equals" : "DIFFERS FROM") << " the GPU's live state ("
              << curve.records.size() << " records)\n";
    auto read = [&] { return points(api); };
    auto report = [&](const char* what, const Equations& E, bool wrote) {
        auto settled = t.settle(E, wrote, read);
        std::vector<double> drv;
        for (double x : driver_stock(settled)) drv.push_back(snap(x, t.meta.step));
        auto f = t.fit(E, drv);
        std::cout << what << ": the driver's curve equals the calculation at " << f.equal << " of " << t.points()
                  << " points at " << f.celsius << " C (" << f.edge << " one step apart at a rounding edge)\n";
        return f;
    };
    auto r = restore(api, store);
    std::cout << "restore: " << r.equations << " equations, " << r.offsets << " offsets written\n";
    report("stock", t.m.E, r.equations > 0);
    auto now = records(api, info);
    bool stock =
        std::ranges::equal(std::span(now).subspan(equation_header), std::span(t.m.stock).subspan(equation_header));
    for (auto& p : points(api).graphics) stock = stock && p.offset_khz == 0;
    check(stock, "live: restore leaves the stock records and zero offsets");
    auto a = apply(api, curve, store);
    std::cout << "apply: " << a.equations << " equations, " << a.offsets << " offsets written\n";
    report("tune", t.edited(t.fields(tune)), a.equations > 0);
    now = records(api, info);
    auto after_points = points(api);
    bool back = std::ranges::equal(std::span(now).subspan(equation_header), std::span(before).subspan(equation_header));
    for (size_t k = 0; k < after_points.graphics.size(); ++k)
        back = back && after_points.graphics[k].offset_khz == before_points.graphics[k].offset_khz;
    check(back, "live: applying the tune restores the records and offsets the GPU had before");
    auto again = apply(api, curve, store);
    check(again.equations == 0 && again.offsets == 0, "live: applying it again writes nothing");
    return equal ? 0 : 1;
}
}
int main(int argc, char** argv) {
    try {
        std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "--oracle" && argc == 5) return oracle(argv[2], argv[3], argv[4]);
        if (mode == "--live" && (argc == 3 || argc == 4)) return live(argv[2], argc == 4 ? argv[3] : "");
        auto root = std::filesystem::path(L"C:/Temp/tandem/mechcontrol-check/vf-curve") /
                    std::to_wstring(GetCurrentProcessId());
        std::filesystem::create_directories(root);
        fixtures(root);
        std::filesystem::remove_all(root);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAIL " << ex.what() << '\n';
        return 1;
    }
}
