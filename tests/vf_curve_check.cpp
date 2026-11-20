// V/F curve checks on synthetic equations and a fake driver. The comparison with Jon's Python tool and the round trip
// on the real GPU are local tools beside the evidence (re/evidence/vf-curve).
#include "vf_fixture.h"
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
        auto at = [&](const Equations& E, double mv, double celsius) { return Env(m, E, mv * 1000, celsius).equ(0); };
        double slow = 200 + double(0.0018f) * 650000;
        check(at(m.E, 650, 59) == slow && at(m.E, 600, 59) == 200 + double(0.0018f) * 600000,
              "below the switch voltage the stock curve is the slow curve");
        check(at(m.E, 750, 20) == 300 + double(0.002f) * 750000 - 20, "a line's fixed correction is added");
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
              meta_from_json(config::read_json(stock_path(store, g).replace_extension(".json"))).uv == t.meta.uv,
          "the tuner's first use records the live records as stock and keeps the clock curve it found");
    check(t.meta.root == 0 && t.meta.uv[0] == 498750 && t.meta.uv[1] == 506250 && t.meta.step == 15 &&
              t.meta.equal == 96 && t.refusal.empty(),
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
              t.rules[1].id == "cold" && t.rules[1].equs == std::vector{15} &&
              t.rules[1].text.find("When the temperature is below 30 °C, the curve is lowered by ") == 0,
          "the channel 3 and cold corrections are found and described");
    check(t.switches == std::map<int, bool>{{1, true}} && t.fast[1].lo == -1000000 && t.main_channel == 4 &&
              t.slots.at(6).v == 3 && t.slots.at(6).v2 == 4 && t.slots.at(6).t == 6 && t.slots.at(6).vt == 9,
          "the switch is turned off at its fixed voltage, and the slider terms are found from each formula's inputs");

    Tune tune;
    tune.global = 30;
    tune.cards["low"] = {-20, -20};
    tune.rules["cold"] = true;
    tune.ceiling = 2900;
    tune.po = 15;
    tune.pp[10] = 7.5;
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
    auto saved = t.state(tune);
    check(t.tune(saved).cards.at("low").l == -20 && t.compile(t.tune(saved), g) == curve &&
              curve_from_json(config::parse(to_json(curve).dump())) == curve,
          "a tune and a compiled curve survive their JSON");
    auto changed_to = [&](const char* key, config::Json value) {
        auto j = saved;
        j[key] = std::move(value);
        return refusal([&] { t.tune(j); });
    };
    check(!changed_to("ver", 1).empty() && !changed_to("ranges", {{"low", {650, 900}}}).empty() &&
              !changed_to("manual", {{"6.k0", 1}}).empty() && !changed_to("mdelta", {{"6.k0", 1}}).empty() &&
              changed_to("preview", {{"ch3", true}}).empty(),
          "a tune of another version, other cards or typed table numbers is refused");

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
    auto now = t.live(records(api, info));
    check(now.E == t.equations(tune) && now.changed == changed && now.undescribed.empty(),
          "the equations read back from the driver are the tune's");
    check(apply(api, curve, store).equations == 0 && d.sets.size() == 7 && d.offset_sets == 1,
          "applying the same curve again writes nothing");
    d.follow = true;
    auto calc = t.calc(t.equations(tune), 59);
    check(t.fit(now.E, points(api)).equal == 96 && calc.win[0] == Calc::floor && calc.win[40] == 6 &&
              calc.lines.at(8)[40].has_value() && !calc.lines.at(7)[40].has_value(),
          "the readback equals the calculation, and the chart's calculation says what sets each point");
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
}
int main() {
    try {
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
