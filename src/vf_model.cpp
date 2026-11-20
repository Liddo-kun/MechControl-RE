// The equation model, its evaluation and the search for the clock curve: VFCURVE blackbox/vfe.py.
#include "vf_tuner.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace mech::nv::vf {
namespace {
constexpr size_t variable_header = 296, variable_info_size = 4356, variable_control_header = 104,
                 variable_record_size = 172;
double f32(std::span<const uint8_t> b, size_t p) {
    float f;
    std::memcpy(&f, b.data() + p, 4);
    return f;
}
void put_f32(Bytes& b, size_t p, double v) {
    float f = float(v);
    std::memcpy(b.data() + p, &f, 4);
}
int u16(std::span<const uint8_t> b, size_t p) { return b[p] | b[p + 1] << 8; }
// Where coefficient j of a formula is in its control record.
size_t coefficient_at(const Equation& e, int j) { return e.type == 3 ? 44 + 4 * j : 76 + 8 * j; }
}

Model::Model(const Bytes& info, const Bytes& records, const Bytes& variable_info, const Bytes& variable_records)
    : stock(records) {
    auto n = equation_count(info);
    if (info.size() < equation_header + n * info_size) throw std::runtime_error("Truncated V/F equation list");
    E.resize(n);
    for (size_t i = 0; i < n; ++i) {
        auto a = std::span(info).subspan(equation_header + i * info_size, info_size);
        auto& e = E[i];
        e.type = int(u32(a, 0));
        e.var = u16(a, 4);
        e.next = u16(a, 6) == 0xffff ? -1 : u16(a, 6);
        if (e.type == 1) {
            e.yes = u16(a, 42);
            e.no = u16(a, 44);
        } else if (e.type == 2) {
            e.a = u16(a, 42);
            e.b = u16(a, 44);
        } else if (!e.leaf())
            throw std::runtime_error("V/F equation " + std::to_string(i) + " has a type MechControl does not know (" +
                                     std::to_string(e.type) + ")");
        e = read(int(i), vf::record(records, i));
    }
    if (variable_info.size() < variable_header || variable_records.size() < variable_control_header)
        throw std::runtime_error("Truncated V/F variable list");
    size_t count = 0;
    for (size_t i = 0; i < 256; ++i)
        if (variable_info[4 + i / 8] & (1u << (i % 8))) {
            if (i != count)
                throw std::runtime_error(
                    "The driver lists its V/F variables with gaps, which MechControl does not know");
            ++count;
        }
    if (variable_info.size() < variable_header + count * variable_info_size ||
        variable_records.size() < variable_control_header + count * variable_record_size)
        throw std::runtime_error("Truncated V/F variable list");
    V.resize(count);
    for (size_t i = 0; i < count; ++i) {
        auto v = std::span(variable_info).subspan(variable_header + i * variable_info_size, variable_info_size);
        auto k = std::span(variable_records)
                     .subspan(variable_control_header + i * variable_record_size, variable_record_size);
        auto& x = V[i];
        x.type = int(u32(v, 0));
        x.lo = f32(k, 4);
        x.hi = f32(k, 8);
        if (x.type == 2 || x.type == 3) {
            x.a = v[268];
            x.b = v[269];
        }
        if (x.type == 8) x.channel = v[276];
        if (x.type == 13) x.value = int32_t(u32(v, 284));
    }
    // What a variable depends on: the point voltage and thermal channels.
    sym.resize(count);
    std::vector<char> state(count); // 1 while being worked out, 2 when done
    auto work = [&](auto& self, size_t i) -> const std::set<int>& {
        if (state[i] == 2) return sym[i];
        if (state[i] == 1) throw std::runtime_error("The V/F variables refer to each other in a loop");
        state[i] = 1;
        auto& x = V[i];
        if (x.type == 9) sym[i] = {voltage};
        if (x.type == 8) sym[i] = {x.channel};
        if (x.type == 2 || x.type == 3) {
            if (size_t(x.a) >= count || size_t(x.b) >= count)
                throw std::runtime_error("V/F variable " + std::to_string(i) +
                                         " refers to a variable that does not exist");
            auto s = self(self, x.a);
            auto& t = self(self, x.b);
            s.insert(t.begin(), t.end());
            sym[i] = std::move(s);
        }
        state[i] = 2;
        return sym[i];
    };
    for (size_t i = 0; i < count; ++i) varies.push_back(work(work, i).contains(voltage));
}
Equation Model::read(int i, std::span<const uint8_t> c) const {
    auto e = E[i];
    e.lo = f32(c, 4);
    e.hi = f32(c, 8);
    if (e.type == 1) {
        e.func = c[44];
        e.ref = c[45];
        e.crit = f32(c, 48);
        e.refidx = int(std::min<uint32_t>(u32(c, 48), INT_MAX));
    } else if (e.type == 2)
        e.max = c[44];
    else if (e.type == 3) {
        e.nvars = 1;
        e.vars[0] = e.var;
        e.nk = 3;
    } else {
        auto nv = u32(c, 68), nk = u32(c, 152);
        if (nv > 3 || nk > 10)
            throw std::runtime_error("V/F equation " + std::to_string(i) +
                                     " has a formula layout MechControl does not know");
        e.nvars = int(nv);
        e.nk = int(nk);
        for (int j = 0; j < e.nvars; ++j) e.vars[j] = int(std::min<uint32_t>(u32(c, 48 + 8 * j), INT_MAX));
    }
    for (int j = 0; j < e.nk; ++j) e.k[j] = f32(c, coefficient_at(e, j));
    return e;
}
Bytes Model::record(int i, const Equation& e) const {
    auto& s = E[i];
    auto stock_record = vf::record(stock, i);
    Bytes r(stock_record.begin(), stock_record.end());
    if (e.lo != s.lo) put_f32(r, 4, e.lo);
    if (e.hi != s.hi) put_f32(r, 8, e.hi);
    for (int j = 0; j < e.nk; ++j)
        if (e.k[j] != s.k[j]) put_f32(r, coefficient_at(e, j), e.k[j]);
    if (e.type == 1) {
        if (e.func != s.func) r[44] = uint8_t(e.func);
        if (e.ref != s.ref) r[45] = uint8_t(e.ref);
        if (e.crit != s.crit) put_f32(r, 48, e.crit);
    }
    if (e.type == 2 && e.max != s.max) r[44] = uint8_t(e.max);
    return r;
}
const Equation& Model::at(const Equations& list, int i) const {
    if (i < 0 || size_t(i) >= list.size())
        throw std::runtime_error("A V/F equation refers to equation " + std::to_string(i) + ", which does not exist");
    return list[i];
}
bool Model::is_switch(const Equation& e) const {
    return e.type == 1 && e.ref && size_t(e.var) < sym.size() && sym[e.var].contains(voltage);
}
std::vector<int> Model::kids(const Equation& e) const {
    std::vector<int> k;
    if (e.type == 1) {
        k = {e.yes, e.no};
        if (e.ref) k.push_back(e.refidx);
    }
    if (e.type == 2) k = {e.a, e.b};
    if (e.next >= 0) k.push_back(e.next);
    return k;
}
std::set<int> Model::tree(int root) const {
    std::set<int> seen;
    std::vector<int> stack{root};
    while (!stack.empty()) {
        int i = stack.back();
        stack.pop_back();
        if (seen.insert(i).second)
            for (int k : kids(at(E, i))) stack.push_back(k);
    }
    return seen;
}

Env::Env(const Model& model, const Equations& list, double uv, double celsius, const std::vector<int>* watch)
    : m(model), E(list), watch_(watch), uv_(uv), celsius_(celsius), value_(model.V.size()), known_(model.V.size()),
      kind_(list.size()), kept_(list.size()) {
    if (!watch_) return;
    if (watch_->size() != E.size()) throw std::logic_error("V/F watch list of another equation list");
    for (int place : *watch_) watched.resize(std::max(watched.size(), size_t(place + 1)));
}
void Env::voltage(double uv) {
    uv_ = uv;
    for (size_t i = 0; i < known_.size(); ++i)
        if (m.varies[i]) known_[i] = 0;
    std::ranges::fill(watched, std::nullopt);
}
double Env::var(int i) {
    if (i < 0 || size_t(i) >= m.V.size())
        throw std::runtime_error("A V/F equation refers to variable " + std::to_string(i) + ", which does not exist");
    if (known_[i]) return value_[i];
    auto& x = m.V[i];
    double r;
    switch (x.type) {
    case 13: r = x.value; break;
    case 8: r = celsius_; break;
    case 9: r = uv_; break;
    case 15: r = 0; break;
    case 2: r = var(x.a) * var(x.b); break;
    case 3: r = var(x.a) + var(x.b); break;
    default:
        throw std::runtime_error("V/F variable " + std::to_string(i) + " has a type MechControl does not know (" +
                                 std::to_string(x.type) + ")");
    }
    known_[i] = 1;
    return value_[i] = std::max(x.lo, std::min(r, x.hi));
}
bool Env::test(const Equation& e, double crit) {
    double x = var(e.var);
    return e.func == 0 ? x == crit : e.func == 1 ? x >= crit : x > crit;
}
double Env::formula(const Equation& e) {
    std::array<double, 3> x{};
    for (int j = 0; j < e.nvars; ++j) x[j] = var(e.vars[j]);
    auto& k = e.k;
    if (e.type == 3) return k[0] + k[1] * x[0] + k[2] * x[0] * x[0];
    if (e.nk < 6 || e.nvars < 2 || (e.nk == 10 && e.nvars < 3))
        throw std::runtime_error("A V/F formula has a layout MechControl does not know");
    double r = k[0] + k[1] * x[0] + k[2] * x[0] * x[0] + k[3] * x[1] + k[4] * x[1] * x[1] + k[5] * x[0] * x[1];
    if (e.nk == 10) r += k[6] * x[2] + k[7] * x[2] * x[2] + k[8] * x[0] * x[2] + k[9] * x[1] * x[2];
    return r;
}
// Whether equation i (a valid index) can be kept: nothing under it reads a variable that depends on the voltage, and
// nothing under it is watched (a watched equation is evaluated at every point that reaches it). Anything unusual (a
// loop, a reference that does not exist) counts as depending on the voltage, so the evaluation meets it as it is.
bool Env::kept(int i) {
    if (kind_[i]) return kind_[i] == 3;
    kind_[i] = 1;
    auto& e = E[i];
    auto steady = [&](int v) { return v >= 0 && size_t(v) < m.varies.size() && !m.varies[v]; };
    auto under = [&](int k) { return k >= 0 && size_t(k) < E.size() && kept(k); };
    bool keep = !(watch_ && (*watch_)[i] >= 0);
    if (e.type == 1) keep = keep && steady(e.var) && under(e.yes) && under(e.no) && (!e.ref || under(e.refidx));
    if (e.type == 2) keep = keep && under(e.a) && under(e.b);
    for (int j = 0; keep && e.leaf() && j < e.nvars; ++j) keep = steady(e.vars[j]);
    keep = keep && (e.next < 0 || under(e.next));
    kind_[i] = keep ? 3 : 2;
    return keep;
}
double Env::equ(int i, std::vector<int>* path, std::vector<char>* seen) {
    auto& e = m.at(E, i);
    if (++depth_ > 10000) throw std::runtime_error("The V/F equations refer to each other in a loop");
    double r;
    if (seen) {
        (*seen)[i] = 1;
        r = value(e, i, path, seen);
    } else if (kept(i)) {
        auto& k = kept_[i];
        if (!k.known) {
            // Its path is worked out apart, as a choice above it may drop a part of `path` again.
            size_t from = scratch_.size();
            double v = value(e, i, &scratch_, nullptr);
            k = {v, leaves_.size(), leaves_.size() + scratch_.size() - from, true};
            leaves_.insert(leaves_.end(), scratch_.begin() + from, scratch_.end());
            scratch_.resize(from);
        }
        if (path) path->insert(path->end(), leaves_.begin() + k.from, leaves_.begin() + k.to);
        r = k.value;
    } else
        r = value(e, i, path, seen);
    --depth_;
    return r;
}
double Env::value(const Equation& e, int i, std::vector<int>* path, std::vector<char>* seen) {
    double r;
    if (e.type == 1)
        r = equ(test(e, e.ref ? equ(e.refidx) : e.crit) ? e.yes : e.no, path, seen);
    else if (e.type == 2) {
        // Both sides add their formulas to the path; the side not chosen is taken out again.
        size_t from = path ? path->size() : 0;
        double a = equ(e.a, path, seen);
        size_t middle = path ? path->size() : 0;
        double b = equ(e.b, path, seen);
        bool first = e.max ? a >= b : a <= b;
        r = first ? a : b;
        if (path && first) path->resize(middle);
        if (path && !first) path->erase(path->begin() + from, path->begin() + middle);
    } else {
        r = formula(e);
        if (path) path->push_back(i);
    }
    r = std::max(e.lo, std::min(r, e.hi));
    if (e.next >= 0) r += equ(e.next, path, seen);
    if (watch_)
        if (int place = (*watch_)[i]; place >= 0) watched[place] = r;
    return r;
}
double to_step(double x, double step) { return (std::ceil((std::floor(x + 1e-9) + 1) / step - 1e-9) - 1) * step; }
std::vector<double> curve(const Model& m, const Equations& E, int root, const std::vector<double>& uv, double celsius,
                          double step) {
    std::vector<double> c;
    if (uv.empty()) return c;
    c.reserve(uv.size());
    Env env(m, E, uv[0], celsius);
    double top = -INFINITY;
    for (double u : uv) {
        env.voltage(u);
        c.push_back(top = std::max(env.equ(root), top));
    }
    for (auto& x : c) x = to_step(x, step);
    return c;
}
std::vector<double> with_offsets(const std::vector<double>& base, const std::vector<double>& off, double st) {
    std::vector<double> out(base.size());
    double top = -INFINITY;
    for (size_t p = 0; p < base.size(); ++p) {
        double o = off[p], a = std::abs(o), k = std::floor(a / (2 * st) + 1e-9),
               q = 2 * st * k + (a - 2 * st * k > 1e-6 ? std::floor(st) : 0),
               x = std::floor(base[p]) + (o > 0 ? q : -q);
        top = std::max(top, to_step(x, st));
        out[p] = top;
    }
    return out;
}
double snap(double v, double step) {
    double c = std::ceil(v / step - 1e-9) * step;
    return c - v < 1 ? c : v;
}

config::Json to_json(const Meta& m) {
    return {{"root", m.root}, {"grid", {{"uv", m.uv}}}, {"step", m.step}, {"fit", {{"points_equal", m.equal}}}};
}
Meta meta_from_json(const config::Json& j) {
    Meta m;
    m.root = j.at("root").get<int>();
    auto& g = j.at("grid");
    if (g.contains("uv"))
        m.uv = g.at("uv").get<std::vector<double>>();
    else // a straight grid, as vfe.py and the first versions of the tab saved it
        for (int i = 0; i < g.at("npoints").get<int>(); ++i)
            m.uv.push_back(g.at("v0").get<double>() + g.at("vstep").get<double>() * i);
    if (m.uv.size() < 2) throw std::runtime_error("Malformed V/F curve points");
    m.step = j.at("step").get<double>();
    m.equal = j.at("fit").at("points_equal").get<int>();
    return m;
}

Meta find_meta(const Model& m, const Points& points) {
    std::vector<double> stock, uv;
    std::vector<long long> listed;
    for (auto& p : points.graphics) {
        stock.push_back(p.base_khz / 1000.0);
        listed.push_back(p.uv);
        uv.push_back(p.uv);
    }
    size_t n = stock.size();
    if (n < 8) throw std::runtime_error("The driver's curve has fewer than 8 points");
    std::set<int> par;
    for (auto& e : m.E)
        for (int k : m.kids(e)) par.insert(k);
    auto vdep = [&](int i) {
        auto depends = [&](int v) { return size_t(v) < m.sym.size() && m.sym[v].contains(voltage); };
        for (int j : m.tree(i)) {
            auto& e = m.E[j];
            for (int k = 0; k < e.nvars; ++k)
                if (depends(e.vars[k])) return true;
            if (e.type == 1 && depends(e.var)) return true;
        }
        return false;
    };
    std::vector<int> roots;
    for (int i = 0; i < int(m.E.size()); ++i)
        if (!par.contains(i) && vdep(i)) roots.push_back(i);
    if (roots.empty()) throw std::runtime_error("No V/F equation depends on the voltage");
    auto score = [&](int root, const std::vector<double>& uv, double step, double celsius) {
        auto c = curve(m, m.E, root, uv, celsius, step);
        int equal = 0;
        for (size_t i = 0; i < n; ++i) equal += c[i] == snap(stock[i], step);
        return equal;
    };
    // Point voltage candidates: straight grids whose values, rounded up to 5 mV, give the listed voltages, then the
    // listed voltages themselves.
    std::vector<std::vector<double>> cands;
    double est = double(listed.back() - listed.front()) / double(n - 1);
    std::vector<long long> steps;
    for (int d = -1250; d <= 1250; d += 625) steps.push_back((long long)std::nearbyint(est / 625.0) * 625 + d);
    std::stable_sort(steps.begin(), steps.end(),
                     [&](long long a, long long b) { return std::abs(a - est) < std::abs(b - est); });
    for (long long st : steps) {
        if (st <= 0) continue;
        for (long long v0 = listed[0]; v0 > listed[0] - 5000; v0 -= 625) {
            bool fits = true;
            for (size_t i = 0; i < n && fits; ++i)
                fits = std::ceil(double(v0 + st * (long long)i) / 5000.0 - 1e-9) * 5000 == double(listed[i]);
            if (!fits) continue;
            auto& c = cands.emplace_back();
            for (size_t i = 0; i < n; ++i) c.push_back(double(v0 + st * (long long)i));
        }
    }
    cands.push_back(uv);
    int best = -1;
    Meta meta;
    double t0 = 0;
    for (int r : roots)
        for (auto* c : {&cands.front(), &cands.back()})
            for (double st : {7.5, 15.0})
                for (int t = -5; t < 106; t += 10)
                    if (int s = score(r, *c, st, t); s > best) std::tie(best, meta.root, meta.step, t0) = std::tuple(s, r, st, double(t));
    best = -1;
    for (auto& c : cands)
        for (int t2 = int(t0 * 2) - 20; t2 < int(t0 * 2) + 21; ++t2)
            if (int s = score(meta.root, c, meta.step, t2 / 2.0); s > best) {
                best = s;
                meta.uv = c;
            }
    meta.equal = best;
    return meta;
}
}
