#pragma once
#include "vf_curve.h"
#include <array>
#include <functional>
#include <set>

// The V/F curve tuner behind the V/F curve tab, ported from Jon's VFCURVE tools (vfe.py, vfe-ui.py and the
// calculation of vfe-ui.html): the equation model and its evaluation, which equation is the clock curve at which
// point voltages, the lines, cards, corrections and slow-curve switch of this chip, a tune (the page's slider state)
// turned into equation records, and the driver's rounding, never-falling and offset rules. Nothing here assumes one
// GPU's equation numbers, point count, grid or chip value. Only the tab uses it.
namespace mech::nv::vf {
constexpr double unbounded = 3.0e38; // a limit at or beyond this is no limit
constexpr double float_max = 3.4028234663852886e38;

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
};
struct Variable {
    int type = 0; // 2 a·b, 3 a+b, 8 a thermal channel, 9 the point voltage (µV), 13 a constant, 15 a flag
    double lo = 0, hi = 0;
    int a = 0, b = 0, channel = 0;
    int32_t value = 0;
};
using Equations = std::vector<Equation>;
// Field values by equation: "lo", "hi", "k0" to "k9", "crit", "ref", "func", "max".
using Fields = std::map<int, std::map<std::string, double>>;
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
    std::vector<int> kids(const Equation&, bool refs = true) const;
    std::set<int> tree(int root) const;
};
// One evaluation (vfe.py Env): a point voltage, one temperature on every thermal channel (plus `hotter` degrees on
// some), and the type-15 flag. Variable values are kept for the evaluation.
class Env {
public:
    Env(const Model&, const Equations&, double uv, double celsius, int flag = 0,
        const std::map<int, double>& hotter = {});
    double var(int);
    bool test(const Equation&, double crit);
    double formula(const Equation&); // a type 3 or 6 equation before its limits
    double equ(int);
    const Model& m;
    const Equations& E;

private:
    double uv_, celsius_;
    int flag_, depth_ = 0;
    std::map<int, double> hotter_;
    std::vector<double> value_;
    std::vector<char> known_;
};
// The driver's rounding: the clock is cut to whole MHz; the result is the highest step whose whole-MHz value is not
// above that (2752.0 gives 2752.5 at 7.5 MHz steps; 2751.9 gives 2745).
double to_step(double mhz, double step);
// The driver never lets the curve go down as voltage rises: a point that computes lower takes the point before it.
std::vector<double> hold(std::vector<double>);
// The driver's offset rule (measured): in whole MHz, the offset cut to a multiple of two steps plus the whole part
// of one step if anything was cut, rounded down to a step, never falling.
std::vector<double> with_offsets(const std::vector<double>& base, const std::vector<double>& offsets, double step);
// A clock as the driver reports it (whole MHz, cut off) back on its step: 2167 is 2167.5.
double snap(double mhz, double step);
// The stock clock per graphics point in MHz as the driver reports it (final minus offset where it does not report
// the stock clock, which is exact only without offsets; refused when offsets are set).
std::vector<double> driver_stock(const Points&);

// Where the clock curve is: the equation, the point voltages (µV) and the clock step, found by matching the driver's
// curve (vfe.py meta.json), and when the stock records were taken.
struct Meta {
    int root = 0;
    std::vector<double> uv;
    std::optional<double> v0, vstep; // when uv is a straight grid
    double step = 7.5;
    int equal = 0;          // points equal to the driver's curve
    double temperature = 0; // at this temperature
    std::string voltages;   // "grid" or "listed"
    std::vector<int> candidates;
    std::string recorded;
};
config::Json to_json(const Meta&);
Meta meta_from_json(const config::Json&);
// Tries every top-level equation that depends on the voltage, point voltage candidates and clock steps of 7.5 and
// 15 MHz, and keeps the one that equals the driver's curve at the most points at some temperature (vfe.py find_meta).
Meta find_meta(const Model&, const Points&);

// A tune: the page's slider state, as the `state` of vfe-tune.json.
struct Card {
    double l = 0, r = 0, bend = 0, tl = 0, tr = 0, tbend = 0;
};
struct Tune {
    double global = 0, po = 0;                           // whole curve; offset at every point
    std::map<int, double> pp;                            // offset added at single points
    std::map<std::string, Card> cards;                   // by card id
    std::map<std::string, std::optional<double>> manual; // "equation.field" typed in; empty: no limit
    std::map<std::string, double> mdelta;                // typed into a slider's number: the difference
    std::map<std::string, bool> rules, preview;          // corrections removed; previewed on the chart
    config::Json view = config::Json::object();          // what the page shows, kept as read
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
    double lo = 0, hi = 0, lo0 = 0, hi0 = 0; // its voltage range (mV); where it sets the stock fast curve
    std::vector<int> heads;
    std::string note;
    int p0 = 0, p1 = 0;                                   // the points where its lines are evaluated
    std::vector<std::pair<int, std::vector<Row>>> chains; // per head
};
// A correction NVIDIA applies that the tab can remove (both limits of its equations become 0).
struct Rule {
    std::string id, kind; // "diff": channel `hot` above `cold`; "temp": below or from `crit` on `channel`
    std::string title, text;
    std::vector<int> equs;
    int hot = 0, cold = 0, channel = 0;
    double crit = 0;
    std::string side; // "below" or "from"
};
struct Column {
    int celsius;
    double calc;      // the temperature it is calculated at
    const char* band; // its column in a reference CSV, or empty
};
// The page's columns, one per 10 °C band. The 60 column is calculated at 59 °C, where the measured 55-64 °C band
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
    int held = 0;
};
// How the driver's curve compares with the calculation (vfe-ui.py fit).
struct Fit {
    double celsius = 0, lo = 0, hi = 0; // the best temperature; the range that fits equally
    int equal = 0, edge = 0;            // points equal; one step apart at a rounding edge
    struct Diff {
        int point;
        double driver, computed, raw;
        bool edge;
    };
    std::vector<Diff> diffs;
    std::vector<double> computed;
};

// After an equation write the driver shows a slower fallback curve for 2-4 s. A read is settled from 5 s after the write
// when it is not that curve and its stock clocks equal the read's before it; at 20 s any read is taken.
struct Settle {
    std::vector<double> fallback; // the fallback curve; empty where it equals the curve
    double step = 7.5;
    std::optional<std::vector<double>> last;
    bool done(const Points&, double waited);
};

class Tuner {
public:
    Tuner(Model, Meta);
    Model m;
    Meta meta;
    std::set<int> tree;              // the clock equation's equations
    std::optional<int> main_channel; // the thermal channel most of its formulas use
    Fields switch_fields;            // what Apply always writes to turn the slow curve off
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
    std::vector<double> raw_curve(const Equations&, double celsius, int flag = 0,
                                  const std::map<int, double>& hotter = {}) const;
    std::vector<double> curve(const Equations&, double celsius, int flag = 0,
                              const std::map<int, double>& hotter = {}) const;
    Calc calc(const Equations&, double celsius, const std::map<int, double>& hotter = {}) const;
    Equations edited(const Fields&) const; // stock with these values
    // The page's parts of fields(): what the sliders add by "equation.field"; the stock value (none for a limit at or
    // beyond 3e38, a compare's crit while it uses an equation and a coefficient the formula lacks); whether the sliders
    // move a field; and the value a field has with a tune (vfe-ui.html value()).
    std::map<std::string, double> additions(const Tune&) const;
    std::optional<double> stock_value(int equation, const std::string& field) const;
    bool slider(int equation, const std::string& field) const;
    std::optional<double> value(const Tune&, const std::map<std::string, double>& additions, int equation,
                                const std::string& field) const;
    // The page's fields(): everything the tune changes from stock, plus the switch, checked and rounded to the
    // driver's 32-bit values as vfe-ui.py clean() does. Throws for a value that cannot be written.
    Fields fields(const Tune&) const;
    std::vector<double> offsets(const Tune&) const; // MHz per point
    Curve compile(const Tune&, const Gpu&) const;
    // Fields of live control records that differ from stock; "other" marks a difference no field describes.
    Fields live_fields(const Bytes& live) const;
    Fit fit(const Equations&, const std::vector<double>& driver_stock) const;
    Settle settling(const Equations&) const;
    // Reads every 0.5 s until a read is settled (only once if nothing was written). Returns the last read.
    Points settle(const Equations&, bool wrote, const std::function<Points()>& read) const;
    Tune tune(const config::Json& state) const; // older saves are converted to today's cards
    config::Json state(const Tune&) const;
    // A reference CSV (columns domain, index, actual_<band>): per band the clock per point, NaN where it has none.
    std::map<std::string, std::vector<double>> reference(const std::filesystem::path&) const;

private:
    std::vector<double> ts_; // sample temperatures of the discovery
    std::string span(const std::vector<bool>& cells) const;
    std::string varname(int v) const;
    std::optional<std::pair<int, int>> chan_diff(int v) const;
    std::vector<Row> chain_rows(int head, const std::map<int, std::vector<bool>>& win,
                                const std::map<int, std::vector<bool>>& vis) const;
    void discover();
    void find_rules();
    void form_names();
    Equations edited(const Equations&, const Fields&) const;
    Card load_card(const Section&, const config::Json& card, const config::Json& state) const;
};
// Opens the tuner on the GPU (Get calls only, apart from files in `directory`). On its first use on a GPU the live
// records become the stock records; where no clock curve was found yet, it is found from the driver's curve.
Tuner open(Api&, const std::filesystem::path& directory);
}
