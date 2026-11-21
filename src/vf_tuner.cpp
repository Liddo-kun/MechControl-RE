// The tuner: VFCURVE blackbox/vfe-ui.py and the calculation of vfe-ui.html.
#include "vf_tuner.h"
#include <algorithm>
#include <cmath>

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
}

Tuner::Tuner(Model model, Meta mt) : m(std::move(model)), meta(std::move(mt)) {
    if (meta.uv.size() < 2) throw std::runtime_error("Malformed V/F curve points");
    m.at(m.E, meta.root);
    tree_ = m.tree(meta.root);
    std::map<int, int> channels;
    for (int i : tree_)
        if (auto& e = m.E[i]; e.leaf())
            for (int j = 0; j < e.nvars; ++j)
                if (size_t(e.vars[j]) < m.V.size() && m.V[e.vars[j]].type == 8) ++channels[m.V[e.vars[j]].channel];
    for (auto [c, n] : channels)
        if (!main_channel || n > channels[*main_channel]) main_channel = c;
    // How to use the fast curve at every voltage. A switch compares the point voltage with an equation (the switch
    // voltage); its true branch is the fast curve. If the switch voltage starts with a fixed amount, that amount becomes
    // -1,000 mV (tested on Jon's laptop); otherwise the compare is made against the constant 0 (not tested on hardware).
    std::vector<std::string> how;
    bool tested = true;
    for (int i : tree_) {
        auto& e = m.E[i];
        if (!m.is_switch(e)) continue;
        auto& b = m.at(m.E, e.refidx);
        if (b.leaf() && b.lo == b.hi) {
            switches[e.refidx] = true;
            how.push_back("the fixed part of the switch voltage (equation " + text(e.refidx) + ") is set to " +
                          g(off_uv / 1000) + " mV, so no point is below the switch voltage");
        } else if (e.func == 1 || e.func == 2) {
            switches[i] = false;
            tested = false;
            how.push_back("equation " + text(i) + " compares the voltage with 0 instead of the switch voltage");
        } else
            throw std::runtime_error("V/F equation " + text(i) + " is a switch this tab cannot turn off");
    }
    if (switches.empty())
        switch_text = "No slow curve was found on this GPU; the curve is edited as it is.";
    else {
        switch_text =
            std::string("Apply disables the slow curve, the lower curve NVIDIA switches to below the kink") +
            (tested ? "." : " (by a kind of write that is untested).") +
            " Untick to leave it on under an overclock found on the GPU; changing an equation ticks it again.";
        switch_detail = "The slow curve is disabled like this: " + join(how, "; ") + ".";
    }
    fast = m.E;
    switch_off(fast);
    discover();
    form_names();
    if (meta.equal < 0.9 * points())
        refusal = "The calculation equals the driver's curve at only " + text(meta.equal) + " of " + text(points()) +
                  " points on this GPU, so the tab cannot be trusted here and writes nothing.";
}

void Tuner::switch_off(Equations& E) const {
    for (auto [i, fixed] : switches) {
        auto& e = E[i];
        if (fixed)
            e.lo = e.hi = off_uv;
        else {
            e.ref = e.refidx = 0;
            e.crit = 0;
        }
    }
}
std::vector<double> Tuner::curve(const Equations& E, double celsius) const {
    return vf::curve(m, E, meta.root, meta.uv, celsius, meta.step);
}
Calc Tuner::calc(const Equations& E, double celsius) const {
    Calc out;
    int n = points();
    std::vector<std::vector<std::optional<double>>*> lines; // as watched
    for (int h : lines_) lines.push_back(&out.lines[h]);
    for (auto line : lines) line->resize(n);
    out.raw.reserve(n);
    out.win.reserve(n);
    out.clock.reserve(n);
    auto& top = m.at(E, meta.root);
    Env env(m, E, meta.uv[0], celsius, &watch_);
    std::vector<int> path;
    for (int p = 0; p < n; ++p) {
        env.voltage(meta.uv[p]);
        path.clear();
        double raw = env.equ(meta.root, &path);
        // The main formula that sets it: the first card's line on the result's path.
        auto head = std::ranges::find_if(path, [&](int i) { return watch_[i] >= 0; });
        int win = std::abs(top.lo) < unbounded && raw == top.lo   ? Calc::floor
                  : std::abs(top.hi) < unbounded && raw == top.hi ? Calc::ceiling
                  : head != path.end()                            ? *head
                                                                  : Calc::none;
        if (p && raw < out.raw[p - 1]) {
            raw = out.raw[p - 1];
            win = Calc::held_flat;
        }
        for (size_t k = 0; k < lines.size(); ++k) (*lines[k])[p] = env.watched[k];
        out.raw.push_back(raw);
        out.win.push_back(win);
        out.clock.push_back(to_step(raw, meta.step));
    }
    return out;
}
// ---- discovery: which equations can matter on this chip ----
namespace {
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
    for (int i : tree_) {
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
    std::vector<char> seen(fast.size());
    std::vector<int> path;
    for (size_t ti = 0; ti < nt; ++ti) {
        Env env(m, fast, meta.uv[0], ts_[ti]);
        for (int p = 0; p < n; ++p) {
            std::ranges::fill(seen, char(0));
            path.clear();
            env.voltage(meta.uv[p]);
            env.equ(meta.root, &path, &seen);
            size_t c = size_t(p) * nt + ti;
            for (int i = 0; i < int(seen.size()); ++i)
                if (seen[i]) mark(fast[i].leaf() ? vis : nodes, i, c);
            for (int i : path) mark(win, i, c);
        }
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
        for (int h : s.heads) {
            s.chains.push_back({h, chain_rows(h, win, vis)});
            for (auto& r : s.chains.back().second) listed.insert(r.equ);
        }
    }
    // The cards' lines, which calc() watches.
    for (auto& s : sections) lines_.insert(lines_.end(), s.heads.begin(), s.heads.end());
    std::ranges::sort(lines_);
    lines_.erase(std::unique(lines_.begin(), lines_.end()), lines_.end());
    watch_.assign(m.E.size(), -1);
    for (size_t k = 0; k < lines_.size(); ++k) watch_[lines_[k]] = int(k);
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
        auto zeroed = fast;
        for (int i : t.leaves) zeroed[i].lo = zeroed[i].hi = 0;
        auto eff = effect(zeroed, fast, is_below ? t.crit - 0.5 : t.crit + 0.5);
        Rule r;
        r.id = is_below ? (++below == 1 ? "cold" : "cold" + text(below)) : (++from == 1 ? "hot" : "hot" + text(from));
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
Equations Tuner::equations(const Tune& t) const {
    auto E = m.E;
    auto checked = [](int i, double v) { // a 32-bit value the driver takes
        if (!std::isfinite(v) || std::abs(v) > std::numeric_limits<float>::max())
            throw std::runtime_error("Equation " + text(i) + " would get a value outside the driver's 32-bit range");
        return double(float(v));
    };
    // Added clock = off + tilt·u + bend·(1 - u²) + (temp + ttilt·u)·t + tbend·t², with u = (mV - Vc)/h (-1 at the
    // card's left end, +1 at its right end) and t = (T - Tref)/10. In the formula the voltage is in µV: k0 constant, V,
    // V², T, T² and V·T.
    std::map<int, std::array<double, 10>> add;
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
                if (slot >= 0 && d != 0 && !std::isnan(d)) add[hd][slot] += d;
        }
    }
    for (auto& [h, ks] : add)
        for (int j = 0; j < 10; ++j)
            if (ks[j] != 0) E[h].k[j] = checked(h, float(m.E[h].k[j] + ks[j]));
    if (t.floor) E[meta.root].lo = checked(meta.root, *t.floor);
    if (t.ceiling) E[meta.root].hi = checked(meta.root, *t.ceiling);
    for (auto& r : rules)
        if (auto it = t.rules.find(r.id); it != t.rules.end() && it->second)
            for (int i : r.equs) E[i].lo = E[i].hi = 0;
    if (!t.keep_slow) switch_off(E);
    return E;
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
    auto E = equations(t);
    Curve c{gpu};
    for (int i = 0; i < int(E.size()); ++i)
        if (auto r = m.record(i, E[i]); !std::ranges::equal(r, record(m.stock, i))) c.records.emplace(i, std::move(r));
    for (double o : offsets(t)) c.offsets_khz.push_back(int(std::lround(o * 1000)));
    c.memory_khz = int(std::lround(t.mem * 1000));
    return c;
}
config::Json Tuner::fields(const Equations& E) const {
    auto out = config::Json::object();
    auto limit = [](double v) { return std::abs(v) >= unbounded ? config::Json() : config::Json(v); };
    for (size_t i = 0; i < E.size(); ++i) {
        auto &e = E[i], &s = m.E[i];
        auto f = config::Json::object();
        if (e.lo != s.lo) f["lo"] = limit(e.lo);
        if (e.hi != s.hi) f["hi"] = limit(e.hi);
        for (int j = 0; j < e.nk; ++j)
            if (e.k[j] != s.k[j]) f["k" + text(j)] = e.k[j];
        if (e.crit != s.crit) f["crit"] = e.crit;
        for (auto [name, a, b] : {std::tuple("ref", e.ref, s.ref), {"func", e.func, s.func}, {"max", e.max, s.max}})
            if (a != b) f[name] = double(a);
        if (!f.empty()) out[text(int(i))] = std::move(f);
    }
    return out;
}
Live Tuner::live(const Bytes& records) const {
    Live out{m.E};
    for (int i = 0; i < int(m.E.size()); ++i) {
        auto r = record(records, i);
        if (std::ranges::equal(r, record(m.stock, i))) continue;
        out.changed.push_back(i);
        out.E[i] = m.read(i, r);
        if (!std::ranges::equal(m.record(i, out.E[i]), r)) out.undescribed.push_back(i);
    }
    return out;
}

Fit Tuner::fit(const Equations& E, const Points& read) const {
    int n = points();
    if (int(read.graphics.size()) != n)
        throw std::runtime_error("The driver's curve has " + text(int(read.graphics.size())) +
                                 " points; the tuner expects " + text(n) + ".");
    std::vector<double> driver;
    for (auto& p : read.graphics) driver.push_back(snap(p.base_khz / 1000.0, meta.step));
    auto equal = [&](double celsius) {
        auto c = curve(E, celsius);
        int k = 0;
        for (int p = 0; p < n; ++p) k += c[p] == driver[p];
        return k;
    };
    Fit f{0, -1};
    double t0 = 0;
    for (int t2 = -10; t2 < 211; t2 += 5)
        if (int k = equal(t2 / 2.0); k > f.equal) std::tie(f.equal, t0) = std::pair(k, t2 / 2.0);
    f.equal = -1;
    for (int d = -25; d < 26; ++d)
        if (double x = t0 + d / 10.0; x >= -5 && x <= 105) {
            double celsius = std::round(x * 10) / 10;
            if (int k = equal(celsius); k > f.equal) std::tie(f.equal, f.celsius) = std::pair(k, celsius);
        }
    // The final clock: the driver's stock clock plus its offsets by the measured rule, against what it reports.
    if (std::ranges::any_of(read.graphics, [](auto& p) { return p.offset_khz != 0; })) {
        std::vector<double> o;
        for (auto& p : read.graphics) o.push_back(p.offset_khz / 1000.0);
        auto predicted = with_offsets(driver, o, meta.step);
        for (int p = 0; p < n; ++p) f.offsets += snap(read.graphics[p].final_khz / 1000.0, meta.step) != predicted[p];
    }
    return f;
}

Tune Tuner::tune(const config::Json& j) const {
    auto refuse = [](const std::string& what) {
        return std::runtime_error("The settings were saved for other V/F curve cards or by an older version (" +
                                  what + ")");
    };
    config::object(j);
    if (!j.contains("ver") || j["ver"] != 2) throw refuse("version");
    if (j.value("ranges", config::Json()) != state({})["ranges"]) throw refuse("card ranges");
    auto part = [&](const char* key) -> const config::Json& { // an object, empty where it is missing
        static const auto none = config::Json::object();
        if (!j.contains(key)) return none;
        config::object(j[key]);
        return j[key];
    };
    Tune t;
    config::field(j, "global", t.global);
    config::field(j, "po", t.po);
    config::field(j, "mem", t.mem);
    config::field(j, "keepSlow", t.keep_slow);
    for (auto& [k, v] : part("pp").items()) {
        if (k.empty() || k.size() > 5 || k.find_first_not_of("0123456789") != std::string::npos ||
            std::stoi(k) >= points())
            throw refuse("an offset at point " + k);
        t.pp[std::stoi(k)] = v.get<double>();
    }
    for (auto& [id, q] : part("sec").items()) {
        if (std::ranges::none_of(sections, [&](auto& s) { return s.id == id; })) throw refuse("card " + id);
        t.cards[id] = {q.value("l", 0.0), q.value("r", 0.0), q.value("bend", 0.0),
                       q.value("tl", 0.0), q.value("tr", 0.0), q.value("tbend", 0.0)};
    }
    for (auto& [k, v] : part("manual").items()) {
        auto root = text(meta.root);
        if ((k != root + ".lo" && k != root + ".hi") || !v.is_number()) throw refuse("a number typed into " + k);
        (k.ends_with("lo") ? t.floor : t.ceiling) = v.get<double>();
    }
    if (!part("mdelta").empty()) throw refuse("numbers typed into the tables");
    for (auto& [id, v] : part("rules").items()) {
        bool on = v.get<bool>();
        if (std::ranges::any_of(rules, [&](auto& r) { return r.id == id; }))
            t.rules[id] = on;
        else if (on)
            throw refuse("correction " + id);
    }
    return t;
}
Tune Tuner::adopt(const Points& read) const {
    Tune t;
    t.keep_slow = true;
    std::map<int, int> count;
    for (auto& g : read.graphics) ++count[g.offset_khz];
    double grid = 2 * meta.step;
    if (!count.empty()) {
        double common = std::ranges::max_element(count, {}, [](auto& c) { return c.second; })->first / 1000.0;
        t.po = std::trunc(common / grid) * grid;
    }
    for (int p = 0; p < int(read.graphics.size()) && p < points(); ++p)
        if (double rest = read.graphics[p].offset_khz / 1000.0 - t.po) t.pp[p] = rest;
    if (auto m = std::ranges::find_if(read.memory, &Point::programmable); m != read.memory.end())
        t.mem = m->offset_khz / 1000.0;
    return t;
}
config::Json Tuner::state(const Tune& t) const {
    config::Json j{{"ver", 2}, {"global", t.global}, {"po", t.po}, {"mem", t.mem}, {"pp", config::Json::object()},
                   {"sec", config::Json::object()}, {"ranges", config::Json::object()},
                   {"manual", config::Json::object()}, {"rules", t.rules}};
    for (auto [p, v] : t.pp) j["pp"][text(p)] = v;
    for (auto& s : sections) {
        auto it = t.cards.find(s.id);
        auto c = it != t.cards.end() ? it->second : Card{};
        j["sec"][s.id] = {{"l", c.l}, {"r", c.r}, {"bend", c.bend}, {"tl", c.tl}, {"tr", c.tr}, {"tbend", c.tbend}};
        j["ranges"][s.id] = {s.lo, s.hi};
    }
    if (t.floor) j["manual"][text(meta.root) + ".lo"] = *t.floor;
    if (t.ceiling) j["manual"][text(meta.root) + ".hi"] = *t.ceiling;
    if (t.keep_slow) j["keepSlow"] = true;
    return j;
}

std::map<std::string, std::vector<double>> Tuner::reference(const std::filesystem::path& path) const {
    std::map<std::string, std::vector<double>> sheet;
    for (auto& c : columns)
        if (*c.band) sheet[c.band].assign(points(), NAN);
    auto content = config::read_text(path);
    std::string_view rest = content;
    if (rest.starts_with("\xEF\xBB\xBF")) rest.remove_prefix(3);
    std::vector<std::string_view> head;
    while (!rest.empty()) {
        auto line = rest.substr(0, rest.find('\n'));
        rest.remove_prefix(std::min(rest.size(), line.size() + 1));
        if (line.ends_with('\r')) line.remove_suffix(1);
        std::vector<std::string_view> cells;
        for (size_t at = 0; at <= line.size();) {
            auto end = std::min(line.find(',', at), line.size());
            cells.push_back(line.substr(at, end - at));
            at = end + 1;
        }
        if (head.empty()) {
            head = cells;
            continue;
        }
        auto get = [&](std::string_view name) {
            auto it = std::ranges::find(head, name);
            return it != head.end() && size_t(it - head.begin()) < cells.size() ? std::string(cells[it - head.begin()])
                                                                                : "";
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
    auto now = gpu(api.identity, info);
    auto variables = variable_info(api);
    auto variable_control = variable_records(api, variables);
    auto live = records(api, info);
    auto stock = load_stock(directory, now, info);
    auto meta_path = stock_path(directory, now).replace_extension(".json");
    std::optional<Meta> meta;
    if (!stock) {
        save_stock(directory, now, live);
        stock = live;
    } else if (std::filesystem::exists(meta_path))
        meta = meta_from_json(config::read_json(meta_path));
    if (!meta) {
        // The driver's curve comes from the live records, which may not be stock.
        meta = find_meta(Model(info, live, variables, variable_control), points(api));
        config::write_json(meta_path, to_json(*meta), true);
    }
    return Tuner(Model(info, *stock, variables, variable_control), *meta);
}
}