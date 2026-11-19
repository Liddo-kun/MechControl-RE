// V/F curve checks. Without arguments: synthetic equations and a fake driver (CTest). Local modes, which need data
// that is not in the repository:
//   --oracle <dump dir> <cases.json> <out.json>   the calculation for the comparison with Jon's Python tool
//   --live <vfe-tune.json> [<dump dir>]           WRITES: stock, then the tune, on the real GPU; then checks that the
//                                                 live state equals what it was. The dump seeds the stock records.
#include "vf_fixture.h"
#include "config_text.h"
#include <iostream>

using namespace mech;
using namespace mech::nv;
using namespace mech::nv::vf;
using namespace vf_fixture;
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
