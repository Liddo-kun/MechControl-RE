#pragma once
#include "vf_curve.h"
#include <array>
#include <set>

// The V/F curve tuner behind the V/F curve tab, from Jon's VFCURVE tools (vfe.py, vfe-ui.py and the calculation of
// vfe-ui.html): the equation model and its evaluation, which equation is the clock curve at which point voltages, the
// lines, cards, corrections and slow-curve switch of this chip, a tune (the tab's slider state) turned into equations
// and their control records, and the driver's rounding, never-falling and offset rules. Nothing here assumes one GPU's
// equation numbers, point count, grid or chip value. Only the tab uses it.
namespace mech::nv::vf {
constexpr double unbounded = 3.0e38; // a limit at or beyond this is no limit

struct Equation {
    int type = 0, var = 0, next = -1; // next: an equation whose value is added, or -1
    double lo = 0, hi = 0;            // the result is kept between these
    // 1: compares variable `var` with `crit` (ref 0) or equation `refidx` (ref 1) by func (0 =, 1 >=, 2 >).
    int yes = 0, no = 0, func = 0, ref = 0, refidx = 0;
    double crit = 0;
    int a = 0, b = 0, max = 0;  // 2: the lowest (max 1: highest) of equations a and b
    int nvars = 0, nk = 0;      // 3, 6: a formula in up to three variables
    std::array<int, 3> vars{};  // 3: k0 + k1 x + k2 x²; 6: the six or ten terms of
    std::array<double, 10> k{}; // x, y and z up to second order (vfe.py poly)
    bool leaf() const { return type == 3 || type == 6; }
    bool operator==(const Equation&) const = default;
};
struct Variable {
    int type = 0; // 2 a·b, 3 a+b, 8 a thermal channel, 9 the point voltage (µV), 13 a constant, 15 a flag
    double lo = 0, hi = 0;
    int a = 0, b = 0, channel = 0;
    int32_t value = 0;
};
using Equations = std::vector<Equation>;
constexpr int voltage = -1; // in Model::sym

// The driver's equations and variables, from their info and control records (vfe.py Model).
struct Model {
    Equations E;
    std::vector<Variable> V;
    std::vector<std::set<int>> sym; // what each variable depends on: thermal channels and `voltage`
    Bytes stock;                    // the control records E was read from
    Model(const Bytes& info, const Bytes& records, const Bytes& variable_info, const Bytes& variable_records);
    const Equation& at(const Equations&, int) const; // throws for an index outside the list
    bool is_switch(const Equation&) const;
    std::vector<int> kids(const Equation&) const;
    std::set<int> tree(int root) const;
    // Equation i as a control record reads, and the reverse: stock record i with the fields in which `e` differs from
    // E[i] written into it.
    Equation read(int i, std::span<const uint8_t> record) const;
    Bytes record(int i, const Equation& e) const;
};
// One evaluation: a point voltage, one temperature on every thermal channel, the type-15 flag 0. Variable values are
// kept for the evaluation.
class Env {
public:
    Env(const Model&, const Equations&, double uv, double celsius);
    double var(int);
    bool test(const Equation&, double crit);
    // The value of equation i. `path` gets the formulas the result comes from (a choice keeps its chosen side's, `next`
    // adds its own), `seen` every equation visited; neither looks into a compare's reference.
    double equ(int i, std::vector<int>* path = nullptr, std::set<int>* seen = nullptr);
    std::vector<std::optional<double>>* values = nullptr; // where set: each evaluated equation's value
    const Model& m;
    const Equations& E;

private:
    double formula(const Equation&); // a type 3 or 6 equation before its limits
    double uv_, celsius_;
    int depth_ = 0;
    std::vector<double> value_;
    std::vector<char> known_;
};
// The driver's rounding: the clock is cut to whole MHz; the result is the highest step whose whole-MHz value is not
// above that (2752.0 gives 2752.5 at 7.5 MHz steps; 2751.9 gives 2745).
double to_step(double mhz, double step);
// The clock curve: equation `root` at each point voltage, never falling (the driver gives a point that computes lower
// the clock of the point before it), on the driver's steps.
std::vector<double> curve(const Model&, const Equations&, int root, const std::vector<double>& uv, double celsius,
                          double step);
// The driver's offset rule (measured): in whole MHz, the offset cut to a multiple of two steps plus the whole part
// of one step if anything was cut, rounded down to a step, never falling.
std::vector<double> with_offsets(const std::vector<double>& base, const std::vector<double>& offsets, double step);
// A clock as the driver reports it (whole MHz, cut off) back on its step: 2167 is 2167.5.
double snap(double mhz, double step);

// Where the clock curve is: the equation, the point voltages (µV), the clock step and at how many points that equals
// the driver's curve (vfe.py meta.json).
struct Meta {
    int root = 0;
    std::vector<double> uv;
    double step = 7.5;
    int equal = 0;
};
config::Json to_json(const Meta&);
Meta meta_from_json(const config::Json&);
// Tries every top-level equation that depends on the voltage, point voltage candidates and clock steps of 7.5 and
// 15 MHz, and keeps the one that equals the driver's curve at the most points at some temperature (vfe.py find_meta).
Meta find_meta(const Model&, const Points&);

// A tune: the tab's slider state, as the `state` of vfe-tune.json.
struct Card {
    double l = 0, r = 0, bend = 0, tl = 0, tr = 0, tbend = 0;
};
struct Tune {
    double global = 0, po = 0;            // whole curve; offset at every point
    std::map<int, double> pp;             // offset added at single points
    std::map<std::string, Card> cards;    // by card id
    std::optional<double> floor, ceiling; // the clock equation's limits, where set
    std::map<std::string, bool> rules;    // corrections removed
};
// One row of a line's table or of the structure table (vfe-ui.py chain_rows).
struct Row {
    int equ = 0;
    std::string kind; // "leaf", "cmp" (a test) or "mm" (a choice)
    std::string when, used, var;
    int yes = 0, no = 0, a = 0, b = 0;
};
// Which coefficient of a main formula multiplies 1, V, V², T, T² and V·T; -1 where it has no such term.
struct Slots {
    int c = 0, v = -1, v2 = -1, t = -1, t2 = -1, vt = -1;
};
// A card: one line (or the foot) with the lines that move with it.
struct Section {
    std::string id, name;
    double lo = 0, hi = 0; // its voltage range (mV)
    std::vector<int> heads;
    std::string note;
    int p0 = 0, p1 = 0;                                   // the points where its lines are evaluated
    std::vector<std::pair<int, std::vector<Row>>> chains; // per head
};
// A correction NVIDIA applies that the tab can remove (both limits of its equations become 0).
struct Rule {
    std::string id, title, text;
    std::vector<int> equs;
};
struct Column {
    int celsius;
    double calc;      // the temperature it is calculated at
    const char* band; // its column in a reference CSV, or empty
};
// The tab's columns, one per 10 °C band. The 60 column is calculated at 59 °C, where the measured 55-64 °C band
// equals the calculation (from 60 °C the equations use other rules).
inline constexpr std::array<Column, 8> columns{{{10, 10, ""},
                                                {20, 20, ""},
                                                {30, 30, "26_35"},
                                                {40, 40, "36_44"},
                                                {50, 50, "45_54"},
                                                {60, 59, "55_64"},
                                                {70, 70, "65_74"},
                                                {80, 80, ""}}};
constexpr double reference_celsius = 59; // the temperature sliders change nothing here
// A curve as the chart shows it: per point the clock, the value before rounding, what set it (a main formula, or
// floor, ceiling or held) and each main formula's own line where it was evaluated.
struct Calc {
    static constexpr int floor = -1, ceiling = -2, held_flat = -3, none = -4; // none: no main formula
    std::vector<double> clock, raw;
    std::vector<int> win;
    std::map<int, std::vector<std::optional<double>>> lines;
};
// The driver's curve read back against the calculation, at the temperature (0.1 °C steps) where they are equal at
// the most points.
struct Fit {
    double celsius = 0;
    int equal = 0;   // points whose stock clock equals the calculation
    int offsets = 0; // points whose final clock is not the stock clock with its offset by the offset rule
};
// The equations on the GPU as far as their records say, which differ from stock, and which differ in bytes the
// model does not read.
struct Live {
    Equations E;
    std::vector<int> changed, undescribed;
};

class Tuner {
public:
    Tuner(Model, Meta);
    Model m;
    Meta meta;
    std::optional<int> main_channel; // the thermal channel most of its formulas use
    // The equations Apply always writes to turn the slow curve off: true where the equation is the fixed part of a
    // switch voltage, which becomes -1,000 mV; false where it is a switch that is made to compare with 0.
    std::map<int, bool> switches;
    std::string switch_text, switch_detail;
    Equations fast;         // stock with the slow curve off
    std::vector<int> heads; // main formulas: formulas without limits that are reached
    std::map<int, Slots> slots;
    std::vector<Section> sections;
    std::vector<Rule> rules;
    std::vector<Row> structure; // the tests and choices above the lines
    std::array<std::string, 10> knames;
    std::string formnote;
    std::string refusal; // why the tab must not write on this GPU; empty when it may

    int points() const { return int(meta.uv.size()); }
    double mv(int point) const { return meta.uv[point] / 1000; }
    std::vector<double> curve(const Equations&, double celsius) const;
    Calc calc(const Equations&, double celsius) const;
    // Stock with the tune and the slow curve off, in the driver's 32-bit values. Throws for a value that cannot be
    // written.
    Equations equations(const Tune&) const;
    std::vector<double> offsets(const Tune&) const; // MHz per point
    Curve compile(const Tune&, const Gpu&) const;
    // How equations differ from stock, by equation and field ("lo", "hi", "k0" to "k9", "crit", "ref", "func",
    // "max"), as vfe-tune.json lists them; a limit at or beyond 3e38 is null.
    config::Json fields(const Equations&) const;
    Live live(const Bytes& records) const;
    Fit fit(const Equations&, const Points&) const;
    // A tune saved by state(): version 2 on this GPU's cards and their ranges. Refuses anything else.
    Tune tune(const config::Json& state) const;
    config::Json state(const Tune&) const;
    // A reference CSV (columns domain, index, actual_<band>): per band the clock per point, NaN where it has none.
    std::map<std::string, std::vector<double>> reference(const std::filesystem::path&) const;

private:
    std::set<int> tree_;     // the clock equation's equations
    std::vector<double> ts_; // sample temperatures of the discovery
    std::string span(const std::vector<bool>& cells) const;
    std::string varname(int v) const;
    std::optional<std::pair<int, int>> chan_diff(int v) const;
    std::vector<Row> chain_rows(int head, const std::map<int, std::vector<bool>>& win,
                                const std::map<int, std::vector<bool>>& vis) const;
    void switch_off(Equations&) const;
    void discover();
    void find_rules();
    void form_names();
};
// Opens the tuner on the GPU (Get calls only, apart from files in `directory`). On its first use on a GPU the live
// records become the stock records; where no clock curve was found yet, it is found from the driver's curve.
Tuner open(Api&, const std::filesystem::path& directory);
}
