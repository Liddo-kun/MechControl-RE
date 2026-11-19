// The tuner: VFCURVE blackbox/vfe-ui.py and the calculation of vfe-ui.html.
#include "vf_tuner.h"
#include "config_text.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>

namespace mech::nv::vf {
namespace {
constexpr double off_uv = -1000000.0; // the fixed part of the switch voltage that turns the slow curve off
constexpr double foot_slope = 12.0;   // MHz per mV: a line steeper than this where it sets the clock is the foot
using Cells = std::vector<bool>;      // point x sample temperature
using CellMap = std::map<int, Cells>;

std::string g(double x) { // Python's format(x, "g")
    char b[32];
    snprintf(b, sizeof b, "%g", x);
    return b;
}
std::string signed_g(double x) {
    char b[32];
    snprintf(b, sizeof b, "%+g", x);
    return b;
}
std::string text(int i) { return std::to_string(i); }
double f32(double v) { return static_cast<float>(v); }
std::string join(const std::vector<std::string>& v, const std::string& by) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? by : "") + v[i];
    return s;
}
std::string join(const std::vector<int>& v, const std::string& by) {
    std::vector<std::string> s;
    for (int i : v) s.push_back(text(i));
    return join(s, by);
}
bool contains(const std::vector<int>& v, int x) { return std::ranges::find(v, x) != v.end(); }
// A JSON value as a JavaScript number would read it in the page; NaN where it is none.
double number(const config::Json& j) {
    if (j.is_number()) return j.get<double>();
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_null()) return 0;
    return NAN;
}
double number(const config::Json& o, const char* key) {
    return o.is_object() && o.contains(key) ? number(o[key]) : NAN;
}
bool truthy(const config::Json& j) {
    return j.is_boolean()  ? j.get<bool>()
           : j.is_number() ? j.get<double>() != 0
                           : j.is_string() && !j.get<std::string>().empty();
}
std::string local_time(std::chrono::system_clock::time_point t) {
    auto s = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    localtime_s(&tm, &s);
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S", &tm);
    return b;
}
// "k0" to "k9" -> 0 to 9; -1 for any other field.
int coefficient(const std::string& f) {
    return f.size() == 2 && f[0] == 'k' && f[1] >= '0' && f[1] <= '9' ? f[1] - '0' : -1;
}
// Where coefficient j of a formula is in its control record.
size_t coefficient_at(const Equation& e, int j) { return e.type == 3 ? 44 + 4 * j : 76 + 8 * j; }
// A control record with field values written into it (vfe-ui.py record()).
Bytes with_fields(std::span<const uint8_t> record, const Equation& e, const std::map<std::string, double>& fields) {
    Bytes r(record.begin(), record.end());
    auto put_f32 = [&](size_t p, double v) {
        float f = float(v);
        std::memcpy(r.data() + p, &f, 4);
    };
    for (auto& [f, v] : fields) {
        if (f == "lo")
            put_f32(4, v);
        else if (f == "hi")
            put_f32(8, v);
        else if (coefficient(f) >= 0)
            put_f32(coefficient_at(e, coefficient(f)), v);
        else if (f == "crit")
            put_f32(48, v);
        else if (f == "ref")
            r[45] = uint8_t(v);
        else if (f == "func" || f == "max")
            r[44] = uint8_t(v);
    }
    return r;
}
}

Tuner::Tuner(Model model, Meta mt) : m(std::move(model)), meta(std::move(mt)) {
    if (meta.uv.size() < 2) throw std::runtime_error("Malformed V/F curve points");
    m.at(m.E, meta.root);
    tree = m.tree(meta.root);
    std::map<int, int> channels;
    for (int i : tree)
        if (auto& e = m.E[i]; e.leaf())
            for (int j = 0; j < e.nvars; ++j)
                if (size_t(e.vars[j]) < m.V.size() && m.V[e.vars[j]].type == 8) ++channels[m.V[e.vars[j]].channel];
    for (auto [c, n] : channels)
        if (!main_channel || n > channels[*main_channel]) main_channel = c;
    // How to use the fast curve at every voltage. A switch compares the point voltage with an equation (the switch
    // voltage); its true branch is the fast curve. If the switch voltage starts with a fixed amount, that amount becomes
    // -1,000 mV (tested on Jon's laptop); otherwise the compare is made against the constant 0 (not tested on hardware).
    std::vector<std::string> how;
    bool tested = true, any = false;
    for (int i : tree) {
        auto& e = m.E[i];
        if (!m.is_switch(e)) continue;
        any = true;
        auto& b = m.at(m.E, e.refidx);
        if (b.leaf() && b.lo == b.hi) {
            switch_fields[e.refidx] = {{"lo", f32(off_uv)}, {"hi", f32(off_uv)}};
            how.push_back("the fixed part of the switch voltage (equation " + text(e.refidx) + ") is set to " +
                          g(off_uv / 1000) + " mV, so no point is below the switch voltage");
        } else if (e.func == 1 || e.func == 2) {
            switch_fields[i] = {{"ref", 0}, {"crit", 0.0}};
            tested = false;
            how.push_back("equation " + text(i) + " compares the voltage with 0 instead of the switch voltage");
        } else
            throw std::runtime_error("V/F equation " + text(i) + " is a switch this tab cannot turn off");
    }
    if (!any)
        switch_text = "No slow curve was found on this GPU; the curve is edited as it is.";
    else {
        switch_text =
            std::string("Apply always disables the slow curve, the lower curve NVIDIA switches to below the kink") +
            (tested ? "." : " (by a kind of write that is untested).");
        switch_detail = "The slow curve is disabled like this: " + join(how, "; ") + ".";
    }
    fast = edited(m.E, switch_fields);
    discover();
    form_names();
    if (meta.equal < 0.9 * points())
        refusal = "The calculation equals the driver's curve at only " + text(meta.equal) + " of " + text(points()) +
                  " points on this GPU, so the tab cannot be trusted here and writes nothing.";
}

std::vector<double> Tuner::raw_curve(const Equations& E, double celsius, int flag,
                                     const std::map<int, double>& hotter) const {
    std::vector<double> raw;
    for (double u : meta.uv) raw.push_back(Env(m, E, u, celsius, flag, hotter).equ(meta.root));
    return hold(std::move(raw));
}
std::vector<double> Tuner::curve(const Equations& E, double celsius, int flag,
                                 const std::map<int, double>& hotter) const {
    auto c = raw_curve(E, celsius, flag, hotter);
    for (auto& x : c) x = to_step(x, meta.step);
    return c;
}
Calc Tuner::calc(const Equations& E, double celsius, const std::map<int, double>& hotter) const {
    std::set<int> shown; // the cards' lines
    for (auto& s : sections) shown.insert(s.heads.begin(), s.heads.end());
    Calc out;
    int n = points();
    for (int h : shown) out.lines[h].assign(n, std::nullopt);
    auto& top = m.at(E, meta.root);
    for (int p = 0; p < n; ++p) {
        Env env(m, E, meta.uv[p], celsius, 0, hotter);
        int head = -1;
        // The page's evaluator: the value of equation i and the main formula that set it.
        auto eq = [&](auto& self, int i) -> double {
            auto& e = m.at(E, i);
            double r;
            int h = -1;
            if (e.type == 1) {
                double crit = e.ref ? self(self, e.refidx) : e.crit;
                r = self(self, env.test(e, crit) ? e.yes : e.no);
                h = head;
            } else if (e.type == 2) {
                double a = self(self, e.a);
                int ha = head;
                double b = self(self, e.b);
                if (e.max ? a >= b : a <= b) {
                    r = a;
                    h = ha;
                } else {
                    r = b;
                    h = head;
                }
            } else {
                r = env.formula(e);
                if (shown.contains(i)) h = i;
            }
            r = std::max(e.lo, std::min(r, e.hi));
            if (e.next >= 0) {
                r += self(self, e.next);
                if (h < 0) h = head;
            }
            if (shown.contains(i)) out.lines[i][p] = r;
            head = h;
            return r;
        };
        double raw = eq(eq, meta.root);
        int win = std::abs(top.lo) < unbounded && raw == top.lo   ? Calc::floor
                  : std::abs(top.hi) < unbounded && raw == top.hi ? Calc::ceiling
                                                                  : head;
        if (p && raw < out.raw[p - 1]) {
            raw = out.raw[p - 1];
            ++out.held;
            win = Calc::held_flat;
        }
        out.raw.push_back(raw);
        out.win.push_back(win);
        out.clock.push_back(to_step(raw, meta.step));
    }
    return out;
}

Equations Tuner::edited(const Equations& base, const Fields& fields) const {
    auto E = base;
    for (auto& [i, d] : fields) {
        auto& e = E.at(i);
        for (auto& [f, v] : d) {
            if (f == "lo")
                e.lo = v;
            else if (f == "hi")
                e.hi = v;
            else if (f == "crit")
                e.crit = v;
            else if (f == "ref")
                e.ref = int(v);
            else if (f == "func")
                e.func = int(v);
            else if (f == "max")
                e.max = int(v);
            else if (f.size() == 2 && f[0] == 'k')
                e.k[f[1] - '0'] = v;
        }
    }
    return E;
}
Equations Tuner::edited(const Fields& fields) const { return edited(m.E, fields); }

// ---- discovery: which equations can matter on this chip ----
namespace {
// An evaluation that records the visited leaves, the leaves on the result path and the visited nodes.
double trace(Env& env, int i, std::set<int>& vis, std::set<int>& nodes, std::vector<int>& path) {
    auto& e = env.m.at(env.E, i);
    nodes.insert(i);
    double r;
    path.clear();
    if (e.type == 1) {
        double crit = e.crit;
        if (e.ref) {
            std::set<int> v2, n2;
            std::vector<int> p2;
            crit = trace(env, e.refidx, v2, n2, p2);
        }
        r = trace(env, env.test(e, crit) ? e.yes : e.no, vis, nodes, path);
    } else if (e.type == 2) {
        std::vector<int> pa, pb;
        double a = trace(env, e.a, vis, nodes, pa), b = trace(env, e.b, vis, nodes, pb);
        bool first = e.max ? a >= b : a <= b;
        r = first ? a : b;
        path = first ? pa : pb;
    } else {
        r = env.formula(e);
        vis.insert(i);
        path = {i};
    }
    r = std::max(e.lo, std::min(r, e.hi));
    if (e.next >= 0) {
        std::vector<int> pn;
        r += trace(env, e.next, vis, nodes, pn);
        path.insert(path.end(), pn.begin(), pn.end());
    }
    return r;
}
std::vector<std::pair<std::string, std::string>> section_names(size_t n) {
    if (n == 1) return {{"main", "Main line"}};
    if (n == 2) return {{"low", "Low"}, {"high", "High"}};
    if (n == 3) return {{"low", "Low"}, {"mid", "Middle"}, {"high", "High"}};
    std::vector<std::pair<std::string, std::string>> out{{"low", "Low"}};
    for (size_t k = 1; k + 1 < n; ++k) out.push_back({"mid" + std::to_string(k), "Middle, line " + std::to_string(k)});
    out.push_back({"high", "High"});
    return out;
}
}

std::string Tuner::span(const Cells& cells) const {
    size_t nt = ts_.size();
    std::vector<int> ps;
    std::set<size_t> ts;
    for (size_t c = 0; c < cells.size(); ++c)
        if (cells[c]) {
            if (ps.empty() || ps.back() != int(c / nt)) ps.push_back(int(c / nt));
            ts.insert(c % nt);
        }
    auto v = g(mv(ps.front())) + (ps.size() > 1 ? "-" + g(mv(ps.back())) : "") + " mV";
    bool lo = *ts.begin() > 0, hi = *ts.rbegin() < nt - 1;
    double first = ts_[*ts.begin()], next = hi ? ts_[*ts.rbegin() + 1] : 0;
    return v + (lo && hi ? ", " + g(first) + " to " + g(next) + " °C"
                : hi     ? ", below " + g(next) + " °C"
                : lo     ? ", from " + g(first) + " °C"
                         : "");
}
std::optional<std::pair<int, int>> Tuner::chan_diff(int v) const {
    // (hot, cold) if variable v is channel hot minus channel cold, never below 0.
    auto& x = m.V[v];
    if (x.type != 3 || x.lo != 0) return {};
    auto neg = [&](const Variable& y) -> std::optional<int> { // y = a channel times the constant -1
        if (y.type != 2) return {};
        for (auto [c, k] : {std::pair(&m.V.at(y.a), &m.V.at(y.b)), std::pair(&m.V.at(y.b), &m.V.at(y.a))})
            if (c->type == 8 && k->type == 13 && k->lo == k->hi && k->hi == -1.0) return c->channel;
        return {};
    };
    auto &a = m.V.at(x.a), &b = m.V.at(x.b);
    for (auto [pos, ng] : {std::pair(&b, &a), std::pair(&a, &b)})
        if (pos->type == 8 && neg(*ng)) return std::pair(pos->channel, *neg(*ng));
    return {};
}
std::string Tuner::varname(int v) const {
    auto& x = m.V.at(v);
    if (x.type == 9) return "voltage (uV)";
    if (x.type == 8)
        return x.channel == main_channel ? "temperature (°C)" : "temperature of channel " + text(x.channel) + " (°C)";
    if (m.sym[v].empty()) return "chip value " + g(Env(m, m.E, meta.uv[0], 0.0).var(v));
    if (auto cd = chan_diff(v))
        return "channel " + text(cd->first) + " above channel " + text(cd->second) + " (°C, never below 0)";
    return "variable " + text(v);
}
std::vector<Row> Tuner::chain_rows(int head, const CellMap& win, const CellMap& vis) const {
    // The chain that starts at a head, with compares on chip values decided (on the fast curve).
    std::vector<Row> rows;
    std::map<int, std::set<int>> memo;
    std::set<int> seen;
    auto deps = [&](auto& self, int i) -> std::set<int> { // what the chain's value depends on
        if (auto it = memo.find(i); it != memo.end()) return it->second;
        memo[i] = {};
        auto& e = m.at(fast, i);
        std::set<int> d;
        if (e.type == 1) d = m.sym.at(e.var);
        if (e.leaf() && e.lo != e.hi)
            for (int j = 0; j < e.nvars; ++j) d.insert(m.sym.at(e.vars[j]).begin(), m.sym.at(e.vars[j]).end());
        for (int k : m.kids(e)) {
            auto s = self(self, k);
            d.insert(s.begin(), s.end());
        }
        return memo[i] = d;
    };
    static const char* ops[] = {"=", ">=", ">"};
    static const char* nots[] = {"is not", "<", "<="};
    auto walk = [&](auto& self, int i, const std::string& when) -> void {
        while (i >= 0) {
            auto& e = m.at(fast, i);
            if (e.leaf()) {
                if (!(e.lo == e.hi && e.hi == 0) && seen.insert(i).second) {
                    Row r{i, "leaf", when};
                    if (win.contains(i))
                        r.used = "sets the clock at " + span(win.at(i));
                    else if (vis.contains(i))
                        r.used = "compared at " + span(vis.at(i)) + ", never the lowest";
                    else
                        r.used = "not reached on this chip";
                    rows.push_back(r);
                }
            } else if (e.type == 1) {
                auto dv = m.sym.at(e.var);
                if (e.ref) {
                    auto s = deps(deps, e.refidx);
                    dv.insert(s.begin(), s.end());
                }
                if (dv.empty()) { // decided by chip values
                    Env env(m, fast, meta.uv[0], 0.0);
                    double crit = e.ref ? env.equ(e.refidx) : e.crit;
                    self(self, env.test(e, crit) ? e.yes : e.no, when);
                } else {
                    auto name = varname(e.var);
                    if (!e.ref && seen.insert(i).second) {
                        Row r{i, "cmp", when};
                        r.var = name;
                        r.yes = e.yes;
                        r.no = e.no;
                        rows.push_back(r);
                    }
                    auto c = e.ref ? "equation " + text(e.refidx) : g(e.crit);
                    auto shortname = name.substr(0, name.find(" ("));
                    auto prefix = when.empty() ? "" : when + " and ";
                    self(self, e.yes, prefix + shortname + " " + ops[e.func % 3] + " " + c);
                    self(self, e.no, prefix + shortname + " " + nots[e.func % 3] + " " + c);
                }
            } else {
                if (seen.insert(i).second) {
                    Row r{i, "mm", when};
                    r.a = e.a;
                    r.b = e.b;
                    rows.push_back(r);
                }
                self(self, e.a, when);
                self(self, e.b, when);
            }
            i = e.next;
        }
    };
    walk(walk, head, "");
    return rows;
}

void Tuner::discover() {
    // The main formulas on this chip (leaves without limits), grouped into cards: the steep lines at the bottom are
    // the foot, every other line that sets the clock gets its own card, and a line that never sets it joins the line
    // it stays closest above. Also the corrections that can be removed.
    std::set<double> t;
    for (int x = -5; x < 106; x += 5) t.insert(x);
    for (int i : tree) {
        auto& e = fast[i];
        if (e.type != 1 || e.ref) continue;
        auto& s = m.sym.at(e.var);
        if (s.size() != 1 || s.contains(voltage)) continue;
        if (e.crit >= -5 && e.crit <= 105) t.insert(e.crit);
        if (e.crit - 0.5 >= -5 && e.crit - 0.5 <= 105) t.insert(e.crit - 0.5);
    }
    ts_.assign(t.begin(), t.end());
    int n = points();
    size_t nt = ts_.size(), cells = size_t(n) * nt;
    CellMap vis, win, nodes;
    auto mark = [&](CellMap& map, int i, size_t c) {
        auto& v = map[i];
        if (v.empty()) v.assign(cells, false);
        v[c] = true;
    };
    for (size_t ti = 0; ti < nt; ++ti)
        for (int p = 0; p < n; ++p) {
            std::set<int> v, nd;
            std::vector<int> path;
            Env env(m, fast, meta.uv[p], ts_[ti]);
            trace(env, meta.root, v, nd, path);
            size_t c = size_t(p) * nt + ti;
            for (int i : v) mark(vis, i, c);
            for (int i : path) mark(win, i, c);
            for (int i : nd)
                if (!v.contains(i)) mark(nodes, i, c);
        }
    auto each = [&](const Cells& cs, auto f) {
        for (size_t c = 0; c < cs.size(); ++c)
            if (cs[c]) f(int(c / nt), ts_[c % nt], c);
    };
    for (auto& [i, cs] : vis)
        if (auto& e = m.E[i]; e.leaf() && e.lo <= -unbounded && e.hi >= unbounded) heads.push_back(i);
    if (heads.empty())
        throw std::runtime_error("No main formulas (formulas without limits) found under V/F equation " +
                                 text(meta.root));
    for (int h : heads) {
        auto& e = m.E[h];
        auto role = [&](int v) {
            auto& x = m.V.at(v);
            return x.type == 9 ? 'V' : x.type == 8 ? 'T' : m.sym[v].empty() ? 'c' : '?';
        };
        std::vector<char> roles;
        for (int j = 0; j < e.nvars; ++j) roles.push_back(role(e.vars[j]));
        std::vector<int> lin = e.type == 3 ? std::vector{1} : std::vector{1, 3, 6},
                         sq = e.type == 3 ? std::vector{2} : std::vector{2, 4, 7};
        auto k = [&](int j) { return j < e.nk ? j : -1; };
        auto jv = std::ranges::find(roles, 'V'), jt = std::ranges::find(roles, 'T');
        Slots s;
        if (jv != roles.end()) {
            s.v = k(lin.at(jv - roles.begin()));
            s.v2 = k(sq.at(jv - roles.begin()));
        }
        if (jt != roles.end()) {
            s.t = k(lin.at(jt - roles.begin()));
            s.t2 = k(sq.at(jt - roles.begin()));
        }
        if (jv != roles.end() && jt != roles.end() && e.type == 6) {
            auto a = std::min(jv, jt) - roles.begin(), b = std::max(jv, jt) - roles.begin();
            s.vt = k(a == 0 && b == 1 ? 5 : a == 0 ? 8 : 9);
        }
        slots[h] = s;
    }
    std::map<size_t, int> cellwin;
    for (int h : heads)
        if (win.contains(h)) each(win[h], [&](int, double, size_t c) { cellwin[c] = h; });
    auto chain = [&](int h, int p, double celsius) { return Env(m, fast, meta.uv[p], celsius).equ(h); };
    auto rng = [&](const std::vector<int>& ps) {
        return std::pair(mv(*std::ranges::min_element(ps)), mv(*std::ranges::max_element(ps)));
    };
    auto points_of = [&](const Cells& cs) {
        std::vector<int> ps;
        each(cs, [&](int p, double, size_t) { ps.push_back(p); });
        return ps;
    };
    std::map<int, std::optional<double>> slope;
    for (int h : heads) {
        std::vector<double> sl;
        if (win.contains(h))
            each(win[h], [&](int p, double celsius, size_t) {
                if (p + 1 < n)
                    sl.push_back((chain(h, p + 1, celsius) - chain(h, p, celsius)) /
                                 ((meta.uv[p + 1] - meta.uv[p]) / 1000));
            });
        std::sort(sl.begin(), sl.end());
        if (!sl.empty()) slope[h] = sl[sl.size() / 2];
    }
    std::vector<int> winning, foot, main, standby;
    for (int h : heads) (win.contains(h) ? winning : standby).push_back(h);
    for (int h : winning) (slope[h] && *slope[h] > foot_slope ? foot : main).push_back(h);
    if (main.empty()) std::swap(main, foot);
    std::map<int, std::optional<int>> partner;
    std::map<int, std::pair<double, double>> gap;
    for (int h : standby) {
        std::vector<std::pair<int, std::vector<double>>> gaps; // in the order first seen
        each(vis[h], [&](int p, double celsius, size_t c) {
            auto w = cellwin.find(c);
            if (w == cellwin.end()) return;
            auto it = std::ranges::find_if(gaps, [&](auto& x) { return x.first == w->second; });
            if (it == gaps.end()) it = gaps.insert(gaps.end(), {w->second, {}});
            it->second.push_back(chain(h, p, celsius) - chain(w->second, p, celsius));
        });
        if (gaps.empty()) {
            partner[h] = std::nullopt;
            continue;
        }
        auto average = [](const std::vector<double>& v) {
            double s = 0;
            for (double x : v) s += x;
            return s / double(v.size());
        };
        auto best = gaps.begin();
        for (auto it = gaps.begin(); it != gaps.end(); ++it)
            if (average(it->second) < average(best->second)) best = it;
        partner[h] = best->first;
        gap[h] = {std::floor(*std::ranges::min_element(best->second) / 10) * 10,
                  std::ceil(*std::ranges::max_element(best->second) / 10) * 10};
    }
    auto second = [&](int h) {
        std::string s;
        for (int x : standby)
            if (partner[x] == h)
                s += " " + text(x) + " is a second line " + g(gap[x].first) + "-" + g(gap[x].second) +
                     " MHz above it and moves with it.";
        return s;
    };
    if (!foot.empty()) {
        std::ranges::stable_sort(
            foot, [&](int a, int b) { return std::ranges::count(win[a], true) > std::ranges::count(win[b], true); });
        Section s{"foot", "Foot of the curve"};
        std::tie(s.lo, s.hi) = rng(points_of(win[foot[0]]));
        s.heads = foot;
        for (int x : standby)
            if (partner[x] && contains(foot, *partner[x])) s.heads.push_back(x);
        std::vector<std::string> at;
        for (int h : foot) at.push_back(text(h) + " at " + span(win[h]));
        s.note = "Steep lines that cut the curve off at low voltage: " + join(at, "; ") + ".";
        for (int h : foot) s.note += second(h);
        sections.push_back(s);
    }
    std::ranges::stable_sort(main, [&](int a, int b) { return rng(points_of(win[a])) < rng(points_of(win[b])); });
    auto names = section_names(main.size());
    for (size_t k = 0; k < main.size(); ++k) {
        int h = main[k];
        Section s{names[k].first, names[k].second};
        std::tie(s.lo0, s.hi0) = rng(points_of(win[h]));
        // The card's voltage range: where the line is evaluated and a line outside the foot sets the clock. Two lines
        // that share a voltage segment (the clock is the lower one) get the same range.
        std::vector<int> ps;
        each(vis[h], [&](int p, double, size_t c) {
            if (auto w = cellwin.find(c); w != cellwin.end() && contains(main, w->second)) ps.push_back(p);
        });
        if (ps.empty())
            throw std::runtime_error("Main formula " + text(h) + " sets the clock nowhere outside the foot");
        std::tie(s.lo, s.hi) = rng(ps);
        s.heads = {h};
        for (int x : standby)
            if (partner[x] == h) s.heads.push_back(x);
        s.note = "Stock: " + text(h) + " sets the clock at " + span(win[h]) + "." + second(h);
        sections.push_back(s);
    }
    std::vector<int> lost;
    for (int x : standby)
        if (!partner[x]) lost.push_back(x);
    if (!lost.empty()) {
        Section s{"other", "Other lines"};
        std::vector<int> ps;
        for (int x : lost) {
            auto q = points_of(vis[x]);
            ps.insert(ps.end(), q.begin(), q.end());
        }
        std::tie(s.lo, s.hi) = rng(ps);
        s.lo0 = s.lo;
        s.hi0 = s.hi;
        s.heads = lost;
        s.note = "Lines that are evaluated but never compared with a line that sets the clock.";
        sections.push_back(s);
    }
    std::set<int> listed;
    for (auto& s : sections) {
        std::vector<int> ps;
        for (int h : s.heads)
            if (vis.contains(h)) {
                auto q = points_of(vis[h]);
                ps.insert(ps.end(), q.begin(), q.end());
            }
        s.p0 = *std::ranges::min_element(ps);
        s.p1 = *std::ranges::max_element(ps);
        if (s.id == "foot") {
            s.lo0 = s.lo;
            s.hi0 = s.hi;
        }
        for (int h : s.heads) {
            s.chains.push_back({h, chain_rows(h, win, vis)});
            for (auto& r : s.chains.back().second) listed.insert(r.equ);
        }
    }
    find_rules();
    // The structure above the heads: the compares on voltage or temperature and the choices that were visited.
    for (auto& [i, cs] : nodes) {
        if (listed.contains(i)) continue;
        auto& e = m.E[i];
        Row r{i, "", "", "used at " + span(cs)};
        if (e.type == 2) {
            r.kind = "mm";
            r.a = e.a;
            r.b = e.b;
        } else if (e.type == 1 && !e.ref && !m.sym.at(e.var).empty()) {
            r.kind = "cmp";
            r.var = varname(e.var);
            r.yes = e.yes;
            r.no = e.no;
        } else
            continue;
        structure.push_back(r);
    }
}

void Tuner::find_rules() {
    // Two kinds of correction inside the lines' chains, which the tab can remove (both limits of their equations
    // become 0). "diff": +A while channel X is not hotter than channel Y (else 0), with a fixed -A in the same chain.
    // "temp": a correction that applies only below (or from) a temperature, on lines outside the foot.
    struct Diff {
        int v, hot, cold;
        double amount;
        std::vector<int> plus, minus, heads;
    };
    struct Temp {
        int v, func;
        double crit;
        std::string side;
        std::vector<int> leaves;
        std::set<std::string> secs;
    };
    std::vector<Diff> diff;
    std::vector<Temp> temp;
    auto is_zero = [&](int i) {
        auto& e = m.at(m.E, i);
        return e.leaf() && e.lo == 0 && e.hi == 0 && e.next < 0;
    };
    auto is_fixed = [&](int i) {
        auto& e = m.at(m.E, i);
        return e.leaf() && e.lo == e.hi && e.lo != 0 && e.next < 0;
    };
    for (auto& sec : sections)
        for (auto& [h, rows] : sec.chains) {
            std::vector<int> free; // zeroing a leaf keeps what is chained after it
            for (auto& r : rows) {
                auto& e = m.E[r.equ];
                if (r.kind == "leaf" && r.when.empty() && e.leaf() && e.lo == e.hi && e.hi != 0) free.push_back(r.equ);
            }
            for (auto& r : rows) {
                if (r.kind != "cmp") continue;
                auto& e = m.E[r.equ];
                int v = e.var, yes = e.yes, no = e.no;
                auto cd = chan_diff(v);
                if (cd && e.func == 0 && e.crit == 0.0 && is_fixed(yes) && is_zero(no)) {
                    double a = m.E[yes].lo;
                    auto minus = std::ranges::find_if(free, [&](int f) { return m.E[f].lo == -a; });
                    if (minus == free.end()) continue;
                    auto d = std::ranges::find_if(diff, [&](auto& x) { return x.v == v; });
                    if (d == diff.end()) d = diff.insert(diff.end(), {v, cd->first, cd->second, a});
                    if (!contains(d->plus, yes)) d->plus.push_back(yes);
                    if (!contains(d->minus, *minus)) d->minus.push_back(*minus);
                    d->heads.push_back(h);
                } else if (m.V.at(v).type == 8 && (e.func == 1 || e.func == 2) && !e.ref) {
                    std::string side;
                    int leaf;
                    if (is_zero(yes) && !is_zero(no) && m.E[no].leaf() && m.E[no].next < 0) {
                        side = "below";
                        leaf = no;
                    } else if (is_zero(no) && !is_zero(yes) && m.E[yes].leaf() && m.E[yes].next < 0) {
                        side = "from";
                        leaf = yes;
                    } else
                        continue;
                    auto t = std::ranges::find_if(temp, [&](auto& x) {
                        return x.v == v && x.func == e.func && x.crit == e.crit && x.side == side;
                    });
                    if (t == temp.end()) t = temp.insert(temp.end(), {v, e.func, e.crit, side});
                    if (!contains(t->leaves, leaf)) t->leaves.push_back(leaf);
                    t->secs.insert(sec.id);
                }
            }
        }
    auto effect = [&](const Equations& a, const Equations& b, double celsius) -> std::string {
        // How curve b differs from curve a at this temperature, in words, or nothing.
        auto ca = curve(a, celsius), cb = curve(b, celsius);
        std::vector<std::pair<int, double>> ch;
        for (int p = 0; p < points(); ++p)
            if (cb[p] != ca[p]) ch.push_back({p, cb[p] - ca[p]});
        if (ch.empty()) return "";
        double lo = INFINITY, hi = -INFINITY;
        for (auto [p, d] : ch) {
            lo = std::min(lo, d);
            hi = std::max(hi, d);
        }
        double v0 = mv(ch.front().first), v1 = mv(ch.back().first);
        auto amount = hi < 0   ? "lowered by " + g(-hi) + (lo != hi ? "-" + g(-lo) : "") + " MHz"
                      : lo > 0 ? "raised by " + g(lo) + (lo != hi ? "-" + g(hi) : "") + " MHz"
                               : "changed by " + signed_g(lo) + " to " + signed_g(hi) + " MHz";
        return amount +
               (ch.back().first == points() - 1 ? " from " + g(v0) + " mV up" : " at " + g(v0) + "-" + g(v1) + " mV");
    };
    for (auto& d : diff) {
        Rule r;
        r.id = "ch" + text(d.hot);
        r.kind = "diff";
        r.hot = d.hot;
        r.cold = d.cold;
        r.equs = d.plus;
        r.equs.insert(r.equs.end(), d.minus.begin(), d.minus.end());
        r.title = "Channel " + text(d.hot) + " correction";
        r.text = "When temp channel " + text(d.hot) + " is not above temp channel " + text(d.cold) +
                 ", the curve is applied normally. When channel " + text(d.hot) + " rises above channel " +
                 text(d.cold) + ", a " + signed_g(-d.amount) + " MHz correction is applied. Channels " + text(d.hot) +
                 " and " + text(d.cold) + " are currently unknown temp readings.";
        rules.push_back(r);
    }
    std::ranges::stable_sort(temp, [](auto& a, auto& b) { return a.crit < b.crit; });
    int below = 0, from = 0;
    for (auto& t : temp) {
        if (t.secs == std::set<std::string>{"foot"}) continue;
        int ch = m.V[t.v].channel;
        bool is_below = t.side == "below";
        Fields zero;
        for (int i : t.leaves) zero[i] = {{"lo", 0.0}, {"hi", 0.0}};
        auto eff = effect(edited(fast, zero), fast, is_below ? t.crit - 0.5 : t.crit + 0.5);
        Rule r;
        r.id = is_below ? (++below == 1 ? "cold" : "cold" + text(below)) : (++from == 1 ? "hot" : "hot" + text(from));
        r.kind = "temp";
        r.channel = ch;
        r.crit = t.crit;
        r.side = t.side;
        r.equs = t.leaves;
        auto when = is_below      ? "below " + g(t.crit) + " °C"
                    : t.func == 1 ? "at " + g(t.crit) + " °C or more"
                                  : "above " + g(t.crit) + " °C";
        std::vector<std::string> cols;
        for (auto& c : columns)
            if (is_below ? c.calc < t.crit : t.func == 1 ? c.calc >= t.crit : c.calc > t.crit)
                cols.push_back(g(c.celsius));
        std::string show = " No column of this tab is in that range.";
        if (!cols.empty()) {
            auto last = cols.back();
            cols.pop_back();
            auto named = cols.empty() ? last : join(cols, ", ") + " and " + last;
            bool many = !cols.empty();
            show = " The " + named + " °C column" + (many ? "s" : "") + " show" + (many ? "" : "s") + " it.";
        }
        r.title = std::string(is_below ? "Cold" : "Hot") + " correction, " + when;
        r.text = "When " + (ch == main_channel ? std::string("the temperature") : "temp channel " + text(ch)) + " is " +
                 when + ", the curve is " + (eff.empty() ? "not changed at any point" : eff) + "." + show;
        rules.push_back(r);
    }
}

void Tuner::form_names() {
    // Names of the ten terms, from the inputs of the main formulas (the most common order), and a note.
    auto nm = [&](int v) -> std::pair<std::string, std::string> {
        auto& x = m.V.at(v);
        if (x.type == 9) return {"V", "the point voltage in µV"};
        if (x.type == 8) return {"T", "the channel " + text(x.channel) + " temperature in °C"};
        if (m.sym[v].empty())
            return {"chip", "a chip value (" + g(Env(m, m.E, meta.uv[0], 0.0).var(v)) + " on this GPU)"};
        return {"var" + text(v), "variable " + text(v)};
    };
    std::vector<std::pair<std::array<std::string, 3>, std::vector<int>>> pats; // in the order first seen
    for (int h : heads) {
        auto& e = m.E[h];
        if (e.type != 6 || e.nvars < 3) continue;
        std::array<std::string, 3> key{nm(e.vars[0]).first, nm(e.vars[1]).first, nm(e.vars[2]).first};
        auto it = std::ranges::find_if(pats, [&](auto& p) { return p.first == key; });
        if (it == pats.end()) it = pats.insert(pats.end(), {key, {}});
        it->second.push_back(h);
    }
    if (pats.empty()) {
        knames = {"constant", "k1", "k2", "k3", "k4", "k5", "k6", "k7", "k8", "k9"};
        formnote = "Formulas with three numbers are k0 + k1·x + k2·x².";
        return;
    }
    auto best = pats.begin();
    for (auto it = pats.begin(); it != pats.end(); ++it)
        if (it->second.size() > best->second.size()) best = it;
    std::array<std::string, 3> d;
    for (int j = 0; j < 3; ++j) {
        std::set<std::string> s;
        for (int h : best->second) s.insert(nm(m.E[h].vars[j]).second);
        d[j] = join(std::vector<std::string>(s.begin(), s.end()), " or ");
    }
    auto& [a, b, c] = best->first;
    knames = {"constant", a, a + "²", b, b + "²", a + "·" + b, c, c + "²", a + "·" + c, b + "·" + c};
    formnote =
        "A main formula is k0 + k1·" + a + " + k2·" + a + "² + k3·" + b + " + k4·" + b + "² + k5·" + a + "·" + b +
        " + k6·" + c + " + k7·" + c + "² + k8·" + a + "·" + c + " + k9·" + b + "·" + c + ", with " + a + " " + d[0] +
        ", " + b + " " + d[1] + " and " + c + " " + d[2] +
        (pats.size() == 1 && best->second.size() == heads.size()
             ? ""
             : " (for most main formulas; the others list their inputs in the same order as their equation record)") +
        ". Formulas with three numbers are k0 + k1·x + k2·x².";
}

// ---- a tune ----
Card Tuner::load_card(const Section& s, const config::Json& q, const config::Json& t) const {
    // Settings made on the voltage range lo0..hi0 -> the same line with today's sliders on the card's range (older
    // saves used Offset and Tilt, on the range where the line set the stock clock).
    auto from_old = [&](double lo0, double hi0, std::map<std::string, double> o) {
        auto gv = [&](const char* k) { return std::isnan(o[k]) ? 0.0 : o[k]; };
        double h0 = (hi0 - lo0) / 2, a = (s.hi - s.lo) / 2 / h0, b = ((s.lo + s.hi) / 2 - (lo0 + hi0) / 2) / h0;
        double bend = gv("bend") * a * a, tilt = gv("tilt") * a - 2 * gv("bend") * a * b,
               off = gv("off") + gv("tilt") * b + gv("bend") * (1 - b * b) - bend;
        double temp = gv("temp") + gv("ttilt") * b, tt = gv("ttilt") * a;
        return Card{off - tilt, off + tilt, bend, temp - tt, temp + tt, gv("tbend")};
    };
    auto num = [&](const char* k) { return number(q, k); };
    if (!(t.contains("ver") && t["ver"] == 2))
        return from_old(s.lo0, s.hi0,
                        {{"off", num("off")},
                         {"tilt", num("tilt")},
                         {"bend", num("bend")},
                         {"temp", num("temp")},
                         {"ttilt", num("ttilt")},
                         {"tbend", num("tbend")}});
    const config::Json* r = nullptr;
    if (t.contains("ranges") && t["ranges"].is_object() && t["ranges"].contains(s.id)) r = &t["ranges"][s.id];
    if (!r || (r->is_array() && r->size() == 2 && number((*r)[0]) == s.lo && number((*r)[1]) == s.hi)) {
        auto z = [&](const char* k) { return q.is_object() && q.contains(k) ? number(q[k]) : 0.0; };
        return {z("l"), z("r"), z("bend"), z("tl"), z("tr"), z("tbend")};
    }
    return from_old(number((*r)[0]), number((*r)[1]),
                    {{"off", (num("l") + num("r")) / 2},
                     {"tilt", (num("r") - num("l")) / 2},
                     {"bend", num("bend")},
                     {"temp", (num("tl") + num("tr")) / 2},
                     {"ttilt", (num("tr") - num("tl")) / 2},
                     {"tbend", num("tbend")}});
}
Tune Tuner::tune(const config::Json& t) const {
    config::object(t);
    Tune out;
    for (auto& s : sections) out.cards[s.id] = {};
    if (t.contains("global")) out.global = number(t["global"]);
    if (t.contains("po")) out.po = number(t["po"]);
    if (t.contains("pp")) {
        config::object(t["pp"]);
        for (auto& [k, v] : t["pp"].items())
            if (!k.empty() && k.size() < 6 && k.find_first_not_of("0123456789") == std::string::npos)
                out.pp[std::stoi(k)] = number(v);
    }
    if (t.contains("manual")) {
        config::object(t["manual"]);
        for (auto& [k, v] : t["manual"].items()) out.manual[k] = v.is_null() ? std::nullopt : std::optional(number(v));
    }
    if (t.contains("mdelta")) {
        config::object(t["mdelta"]);
        for (auto& [k, v] : t["mdelta"].items()) out.mdelta[k] = number(v);
    }
    for (const char* k : {"rules", "preview"})
        if (t.contains(k) && t[k].is_object())
            for (auto& [id, v] : t[k].items()) (k[0] == 'r' ? out.rules : out.preview)[id] = truthy(v);
    for (auto& s : sections)
        if (t.contains("sec") && t["sec"].is_object() && t["sec"].contains(s.id))
            out.cards[s.id] = load_card(s, t["sec"][s.id], t);
    for (const char* k : {"showStock", "showNew", "showBand", "lines", "ymode"})
        if (t.contains(k)) out.view[k] = t[k];
    return out;
}
config::Json Tuner::state(const Tune& t) const {
    auto j = t.view;
    j["global"] = t.global;
    j["po"] = t.po;
    j["pp"] = config::Json::object();
    for (auto [p, v] : t.pp) j["pp"][text(p)] = v;
    j["sec"] = config::Json::object();
    for (auto& [id, c] : t.cards)
        j["sec"][id] = {{"l", c.l}, {"r", c.r}, {"bend", c.bend}, {"tl", c.tl}, {"tr", c.tr}, {"tbend", c.tbend}};
    j["manual"] = config::Json::object();
    for (auto& [k, v] : t.manual) j["manual"][k] = config::nullable(v);
    j["mdelta"] = t.mdelta;
    j["rules"] = t.rules;
    j["preview"] = t.preview;
    j["ver"] = 2;
    j["ranges"] = config::Json::object();
    for (auto& s : sections) j["ranges"][s.id] = {s.lo, s.hi};
    return j;
}

Fields Tuner::fields(const Tune& t) const {
    // The amount the sliders add, by "equation.field". Added clock = off + tilt·u + bend·(1 - u²) + (temp + ttilt·u)·t
    // + tbend·t², with u = (mV - Vc)/h (-1 at the card's left end, +1 at its right end) and t = (T - Tref)/10. In the
    // formula the voltage is in µV: k0 constant, V, V², T, T² and V·T.
    std::map<std::string, double> add;
    for (auto& s : sections) {
        auto it = t.cards.find(s.id);
        auto q = it != t.cards.end() ? it->second : Card{};
        double vc = (s.lo + s.hi) / 2, h = (s.hi - s.lo) / 2;
        double off = (q.l + q.r) / 2, tilt = (q.r - q.l) / 2, bend = q.bend, temp = (q.tl + q.tr) / 2,
               ttilt = (q.tr - q.tl) / 2, tbend = q.tbend;
        double r = reference_celsius, a = ttilt / (10 * h);
        double d0 = t.global + off + bend - tilt * vc / h - bend * vc * vc / (h * h) - temp * r / 10 + a * vc * r +
                    tbend * r * r / 100;
        double dv = (tilt / h + 2 * bend * vc / (h * h) - a * r) / 1e3, d4 = -bend / (h * h) / 1e6;
        double d6 = temp / 10 - a * vc - 2 * r * tbend / 100, d7 = tbend / 100, d9 = a / 1e3;
        for (int hd : s.heads) {
            auto& k = slots.at(hd);
            for (auto [slot, d] : {std::pair(k.c, d0), {k.v, dv}, {k.v2, d4}, {k.t, d6}, {k.t2, d7}, {k.vt, d9}})
                if (slot >= 0 && d != 0 && !std::isnan(d)) add["" + text(hd) + ".k" + text(slot)] += d;
        }
    }
    std::map<std::string, double> forced; // the removed corrections
    for (auto& r : rules)
        if (auto it = t.rules.find(r.id); it != t.rules.end() && it->second)
            for (int i : r.equs) forced[text(i) + ".lo"] = forced[text(i) + ".hi"] = 0;
    auto parse = [&](const std::string& key) {
        auto dot = key.find('.');
        if (dot == std::string::npos || dot == 0 || key.find_first_not_of("0123456789") != dot)
            throw std::runtime_error("The tune names an unknown field: " + key);
        return std::pair(std::stoi(key.substr(0, dot)), key.substr(dot + 1));
    };
    // The page's base value: none for a limit at or beyond 3e38, a compare's crit while it uses an equation, and a
    // coefficient the formula does not have.
    auto base = [&](int i, const std::string& f) -> std::optional<double> {
        auto& e = m.at(m.E, i);
        auto limit = [](double v) { return std::abs(v) >= unbounded ? std::nullopt : std::optional(v); };
        if (f == "lo") return limit(e.lo);
        if (f == "hi") return limit(e.hi);
        if (f == "crit") return e.ref ? std::nullopt : std::optional(e.crit);
        if (f == "func") return e.func;
        if (f == "max") return e.max;
        if (coefficient(f) >= 0 && coefficient(f) < e.nk) return e.k[coefficient(f)];
        return std::nullopt;
    };
    auto slider = [&](int i, const std::string& f) {
        auto s = slots.find(i);
        if (s == slots.end() || !std::ranges::any_of(sections, [&](auto& c) { return contains(c.heads, i); }))
            return false;
        for (int k : {s->second.c, s->second.v, s->second.v2, s->second.t, s->second.t2, s->second.vt})
            if (k >= 0 && f == "k" + text(k)) return true;
        return false;
    };
    std::set<std::string> keys;
    for (auto* map : {&add, &forced}) // add, mdelta, manual and forced, as the page collects them
        for (auto& [k, v] : *map) keys.insert(k);
    for (auto& [k, v] : t.mdelta) keys.insert(k);
    for (auto& [k, v] : t.manual) keys.insert(k);
    std::map<int, std::map<std::string, std::optional<double>>> raw;
    for (auto& key : keys) {
        auto [i, f] = parse(key);
        auto b = base(i, f);
        std::optional<double> v;
        if (forced.contains(key))
            v = forced[key];
        else if (slider(i, f)) {
            auto md = t.mdelta.find(key);
            v = f32(b.value_or(NAN) + (add.contains(key) ? add[key] : 0) +
                    (md != t.mdelta.end() && !std::isnan(md->second) ? md->second : 0));
        } else if (auto mn = t.manual.find(key); mn != t.manual.end())
            v = mn->second;
        else
            v = b;
        bool same = v.has_value() == b.has_value() && (!v || *v == *b);
        if (!same) raw[i][f] = v;
    }
    for (auto& [i, d] : switch_fields)
        for (auto& [f, v] : d) raw[i][f] = v;
    // vfe-ui.py clean(): checked 32-bit values; the switch always off.
    Fields out;
    for (auto& [i, d] : raw) {
        if (!tree.contains(i)) throw std::runtime_error("Equation " + text(i) + " is not part of the clock curve");
        auto& e = m.E[i];
        for (auto& [f, v] : d) {
            auto bad = [&] {
                return std::runtime_error("Equation " + text(i) + ": '" + f + "' cannot be set to that value");
            };
            if (f == "ref" && e.type == 1 && v && (*v == 0 || *v == 1)) {
                out[i][f] = *v;
                continue;
            }
            if (f == "lo" || f == "hi") {
                if (!v) {
                    out[i][f] = f == "lo" ? -float_max : float_max;
                    continue;
                }
            } else if (e.leaf() && coefficient(f) >= 0 && coefficient(f) < e.nk) {
            } else if (f == "crit" && e.type == 1 && (!e.ref || (d.contains("ref") && d.at("ref") == 0.0))) {
            } else if (f == "func" && e.type == 1 && v && (*v == 0 || *v == 1 || *v == 2)) {
                out[i][f] = *v;
                continue;
            } else if (f == "max" && e.type == 2 && v && (*v == 0 || *v == 1)) {
                out[i][f] = *v;
                continue;
            } else
                throw bad();
            if (!v || !std::isfinite(*v) || std::abs(*v) > float_max) throw bad();
            out[i][f] = f32(*v);
        }
    }
    for (auto& [i, d] : switch_fields)
        for (auto& [f, v] : d) out[i][f] = v;
    return out;
}
std::vector<double> Tuner::offsets(const Tune& t) const {
    std::vector<double> o(points());
    for (int p = 0; p < points(); ++p) {
        auto it = t.pp.find(p);
        double add = it != t.pp.end() && !std::isnan(it->second) ? it->second : 0;
        o[p] = std::max(-1000.0, std::min(1000.0, t.po + add));
    }
    return o;
}
Curve Tuner::compile(const Tune& t, const Gpu& gpu) const {
    Curve c{gpu};
    for (auto& [i, d] : fields(t)) {
        auto stock = record(m.stock, i);
        auto r = with_fields(stock, m.E[i], d);
        if (!std::ranges::equal(r, stock)) c.records.emplace(i, std::move(r));
    }
    for (double o : offsets(t)) c.offsets_khz.push_back(int(std::lround(o * 1000)));
    return c;
}

Fields Tuner::live_fields(const Bytes& live) const {
    Fields out;
    for (int i = 0; i < int(m.E.size()); ++i) {
        auto r = record(live, i);
        if (std::ranges::equal(r, record(m.stock, i))) continue;
        auto& e = m.E[i];
        auto at = [&](size_t p) {
            float f;
            std::memcpy(&f, r.data() + p, 4);
            return double(f);
        };
        auto& d = out[i];
        if (at(4) != e.lo) d["lo"] = at(4);
        if (at(8) != e.hi) d["hi"] = at(8);
        if (e.leaf())
            for (int j = 0; j < e.nk; ++j) {
                double v = at(coefficient_at(e, j));
                if (v != e.k[j]) d["k" + text(j)] = v;
            }
        else if (e.type == 1) {
            if (r[44] != e.func) d["func"] = r[44];
            if (r[45] != e.ref) d["ref"] = r[45];
            if (!r[45] && (e.ref || at(48) != e.crit)) d["crit"] = at(48);
        } else if (e.type == 2 && r[44] != e.max)
            d["max"] = r[44];
        // A difference that these fields do not describe.
        if (!std::ranges::equal(with_fields(record(m.stock, i), e, d), r)) d["other"] = 1;
    }
    return out;
}

Fit Tuner::fit(const Equations& E, const std::vector<double>& stock) const {
    // The temperature (0.1 °C steps) at which the computed curve equals the driver's at the most points.
    int n = points();
    auto score = [&](double celsius) {
        auto c = curve(E, celsius);
        int equal = 0;
        double off = 0;
        for (int p = 0; p < n; ++p) {
            equal += c[p] == stock[p];
            off -= std::abs(c[p] - stock[p]);
        }
        return std::tuple(equal, off, c);
    };
    int best = -1;
    double t0 = 0;
    for (int t2 = -10; t2 < 211; t2 += 5)
        if (int s = std::get<0>(score(t2 / 2.0)); s > best) std::tie(best, t0) = std::pair(s, t2 / 2.0);
    struct Cand {
        int equal;
        double off, celsius;
        std::vector<double> c;
    };
    std::vector<Cand> cand;
    for (int d = -25; d < 26; ++d)
        if (double x = t0 + d / 10.0; x >= -5 && x <= 105) {
            double celsius = std::round(x * 10) / 10;
            auto [e, o, c] = score(celsius);
            cand.push_back({e, o, celsius, std::move(c)});
        }
    auto top = cand.begin();
    for (auto it = cand.begin(); it != cand.end(); ++it)
        if (it->equal > top->equal) top = it;
    Fit f;
    f.celsius = top->celsius;
    f.lo = INFINITY;
    f.hi = -INFINITY;
    for (auto& c : cand)
        if (c.equal == top->equal && c.off == top->off) {
            f.lo = std::min(f.lo, c.celsius);
            f.hi = std::max(f.hi, c.celsius);
        }
    f.equal = top->equal;
    f.computed = top->c;
    auto raws = raw_curve(E, f.celsius);
    for (int p = 0; p < n; ++p) {
        if (f.computed[p] == stock[p]) continue;
        bool edge = std::abs(stock[p] - f.computed[p]) == meta.step &&
                    std::abs(raws[p] - std::max(stock[p], f.computed[p])) < 1.0;
        f.diffs.push_back({p, stock[p], f.computed[p], std::round(raws[p] * 1000) / 1000, edge});
        f.edge += edge;
    }
    return f;
}
Points Tuner::settle(const Equations& E, bool wrote, const std::function<Points()>& read) const {
    auto fallback = curve(E, 50.0, 1);
    bool has_fallback = fallback != curve(E, 50.0);
    auto start = std::chrono::steady_clock::now();
    auto c = read();
    if (!wrote) return c;
    std::optional<std::vector<double>> last;
    auto stock = [&](const Points& p) {
        auto s = driver_stock(p);
        for (auto& x : s) x = snap(x, meta.step);
        return s;
    };
    for (;;) {
        double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (waited >= 20) break;
        c = read();
        auto s = stock(c);
        int same = 0;
        for (size_t p = 0; p < s.size() && p < fallback.size(); ++p) same += s[p] == fallback[p];
        bool is_fallback = has_fallback && same > 0.8 * points();
        if (waited >= 5 && !is_fallback && last == s) break;
        last = s;
        Sleep(500);
    }
    return c;
}

std::map<std::string, std::vector<double>> Tuner::reference(const std::filesystem::path& path) const {
    std::map<std::string, std::vector<double>> sheet;
    for (auto& c : columns)
        if (*c.band) sheet[c.band].assign(points(), NAN);
    auto content = config::read_text(path);
    if (content.starts_with("\xEF\xBB\xBF")) content.erase(0, 3);
    std::istringstream in(content);
    std::string line;
    std::vector<std::string> head;
    auto split = [](const std::string& s) {
        std::vector<std::string> cells;
        std::stringstream ss(s);
        std::string c;
        while (std::getline(ss, c, ',')) cells.push_back(c);
        return cells;
    };
    while (std::getline(in, line)) {
        if (line.ends_with('\r')) line.pop_back();
        auto cells = split(line);
        if (head.empty()) {
            head = cells;
            continue;
        }
        auto get = [&](const std::string& name) -> std::string {
            auto it = std::ranges::find(head, name);
            return it == head.end() || size_t(it - head.begin()) >= cells.size() ? "" : cells[it - head.begin()];
        };
        auto index = get("index");
        if (get("domain") != "graphics" || index.empty() ||
            index.find_first_not_of("0123456789") != std::string::npos || index.size() > 6 ||
            std::stoi(index) >= points())
            continue;
        for (auto& [band, values] : sheet)
            if (auto v = get("actual_" + band); !v.empty()) values[std::stoi(index)] = snap(std::stod(v), meta.step);
    }
    return sheet;
}

Tuner open(Api& api, const std::filesystem::path& directory) {
    auto info = equation_info(api);
    auto gpu_now = gpu(api.identity, info);
    auto variables = vf::variable_info(api);
    auto variable_control = vf::variable_records(api, variables);
    auto live = records(api, info);
    auto stock = load_stock(directory, gpu_now, info);
    auto meta_path = stock_path(directory, gpu_now).replace_extension(".json");
    std::optional<Meta> meta;
    std::string recorded;
    if (!stock) {
        save_stock(directory, gpu_now, live);
        stock = live;
        recorded = local_time(std::chrono::system_clock::now());
    } else if (std::filesystem::exists(meta_path))
        meta = meta_from_json(config::read_json(meta_path));
    else
        recorded = local_time(std::chrono::clock_cast<std::chrono::system_clock>(
            std::filesystem::last_write_time(stock_path(directory, gpu_now))));
    if (!meta) {
        // The driver's curve comes from the live records, which may not be stock.
        meta = find_meta(Model(info, live, variables, variable_control), points(api));
        meta->recorded = recorded;
        config::write_json(meta_path, to_json(*meta), true);
    }
    return Tuner(Model(info, *stock, variables, variable_control), *meta);
}
}
